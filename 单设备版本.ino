#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <EEPROM.h>
#include <TimeLib.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// 硬件配置
#define DS18B20_PIN 4
#define RELAY_PIN 5

// 网络配置
const char *ssid = "WIFI";
const char *password = "WIFI";

// 工作时间段结构体（以当天分钟数表示）
struct TimePeriod {
  bool enabled = false;
  uint16_t start = 0;  // 开始时间（分钟）
  uint16_t end = 0;    // 结束时间（分钟）
};

// 系统参数结构体
struct SystemParams {
  float targetTemp = 35.0;  // 默认目标温度35°C
  float hysteresis = 5.0;   // 默认滞后值5°C
  bool relayState = false;
  bool modified = false;
  unsigned long maxRunDurationMinutes = 5;
  unsigned long restDurationMinutes = 10;
  TimePeriod periods[4];  // 最多4个工作时间段
  uint32_t crc;
};

OneWire oneWire(DS18B20_PIN);
DallasTemperature sensors(&oneWire);
ESP8266WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "192.168.2.1", 28800, 3600000); // 路由器NTP，无时区偏移，1小时同步
SystemParams params;

// 全局变量
unsigned long relayStartTime = 0;
bool inWaitPeriod = false;
bool manualMode = false;
float lastValidTemp = 25.0;
String protectionEndTime = "";  // 保护结束时间
unsigned long lastCycleTime = 0;  // 循环控制时间戳
bool inCycleRunning = false;       // 是否在运行阶段
bool tempConversionStarted = false; // 异步温度请求标志
unsigned long tempRequestTime = 0;  // 温度请求时间戳
bool heatingEnabled = false;        // 跟踪加热状态
uint8_t tempLogCounter = 0;        // 温度日志计数器

// 读取温度，同时保证数据有效
float readValidTemperature() {
  static float lastValid = 25.0;
  float temp = sensors.getTempCByIndex(0);
  if (temp <= -127.0 || temp > 125.0) {
    Serial.println(F("温度传感器异常，使用最后有效值"));
    return lastValid;
  }
  lastValid = temp;
  lastValidTemp = temp;  // 更新全局变量
  return temp;
}

// 判断是否处于工作时间段
bool isWithinWorkingPeriod() {
  bool anyEnabled = false;
  for (int i = 0; i < 4; i++) {
    if (params.periods[i].enabled) {
      anyEnabled = true;
      break;
    }
  }
  if (!anyEnabled)
    return true;

  int currentMinutes = hour(now()) * 60 + minute(now());
  for (int i = 0; i < 4; i++) {
    if (!params.periods[i].enabled)
      continue;
    int start = params.periods[i].start;
    int end = params.periods[i].end;
    if (start <= end) {
      if (currentMinutes >= start && currentMinutes < end)
        return true;
    } else {
      if (currentMinutes >= start || currentMinutes < end)
        return true;
    }
  }
  return false;
}

void setup() {
  ESP.wdtEnable(8000);
  Serial.begin(115200);
  pinSetup();
  initEEPROM();
  loadParamsFromEEPROM();
  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    timeClient.begin();
    syncTime();
  }
  initWebServer();
  initTemperatureSensor();
}

void loop() {
  ESP.wdtFeed();
  server.handleClient();
  updateSystemTime();

  // 异步温度更新
  static unsigned long lastTempUpdate = 0;
  if (millis() - lastTempUpdate >= 200) {  // 每200ms检查
    if (!tempConversionStarted) {
      sensors.requestTemperatures();  // 发起温度请求
      tempConversionStarted = true;
      tempRequestTime = millis();
    } else if (millis() - tempRequestTime >= 100) {  // 等待94ms+余量
      if (sensors.isConversionComplete()) {
        lastValidTemp = readValidTemperature();
        tempConversionStarted = false;
        lastTempUpdate = millis();
        // 每10次（2秒）记录一次温度
        if (++tempLogCounter >= 10) {
          Serial.print(F("Temp: "));
          Serial.println(lastValidTemp, 1);
          tempLogCounter = 0;
        }
      }
    }
  }

  // 自动模式下执行温控逻辑
  if (!manualMode) {
    checkTemperature();
  }

  delay(50);  // 缩短延时，提高响应性
}

// 初始化引脚
void pinSetup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
}

// 初始化温度传感器
void initTemperatureSensor() {
  sensors.begin();
  sensors.setResolution(9); // 9位分辨率，93.75ms
}

void connectWiFi() {
  Serial.println(F("开始连接WiFi..."));
  WiFi.begin(ssid, password);
  unsigned long startTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 30000) {
    delay(10);
    ESP.wdtFeed();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("WiFi连接成功，IP: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("WiFi连接失败"));
  }
}

// Web服务器初始化
void initWebServer() {
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/set", HTTP_POST, handleSet);
  server.on("/toggle", HTTP_POST, handleToggle);
  server.begin();
}

// 时间同步函数
bool syncTime() {
  Serial.println(F("同步时间: 192.168.2.1"));
  timeClient.setPoolServerName("192.168.2.1");
  if (timeClient.forceUpdate()) {
    setTime(timeClient.getEpochTime());
    Serial.print(F("同步成功，时间: "));
    Serial.println(formatTime(now()));
    return true;
  }
  Serial.println(F("路由器NTP同步失败"));
  return false;
}

// 自动温控逻辑（3秒运行，7秒停止）
void checkTemperature() {
  if (!isWithinWorkingPeriod()) {
    setRelay(false);
    inCycleRunning = false;
    inWaitPeriod = false;
    relayStartTime = 0;
    lastCycleTime = 0;
    protectionEndTime = "";
    heatingEnabled = false;
    Serial.println(F("非工作时间，继电器关闭"));
    return;
  }

  float currentTemp = lastValidTemp;

  // 更新加热状态
  if (currentTemp >= params.targetTemp) {
    heatingEnabled = false;
  } else if (currentTemp <= params.targetTemp - params.hysteresis) {
    heatingEnabled = true;
  }

  if (!heatingEnabled) {
    setRelay(false);
    inCycleRunning = false;
    inWaitPeriod = false;
    relayStartTime = 0;
    lastCycleTime = 0;
    protectionEndTime = "";
    Serial.println(F("温度高于阈值或等待滞后，继电器关闭"));
    return;
  }

  // 加热启用，执行脉冲控制
  if (!inWaitPeriod) {
    if (relayStartTime == 0) {
      relayStartTime = millis();
    }

    // 检查是否超过最大运行时间
    if (millis() - relayStartTime > params.maxRunDurationMinutes * 60000UL) {
      setRelay(false);
      inWaitPeriod = true;
      inCycleRunning = false;
      lastCycleTime = 0;
      relayStartTime = millis();
      time_t endTime = now() + (params.restDurationMinutes * 60);
      protectionEndTime = formatTime(endTime);
      Serial.println(F("超过最大运行时间，进入保护"));
      return;
    }

    // 3秒运行、7秒停止循环
    unsigned long currentTime = millis();
    if (!inCycleRunning) {
      setRelay(true);
      inCycleRunning = true;
      lastCycleTime = currentTime;
      Serial.print(F("循环开启，温度: "));
      Serial.println(currentTemp, 1);
    } else {
      unsigned long elapsed = currentTime - lastCycleTime;
      if (elapsed >= 11000) {  // 完整周期（3s+7s=10s）
        lastCycleTime = currentTime;
        setRelay(true);
        Serial.print(F("循环开启，温度: "));
        Serial.println(currentTemp, 1);
      } else if (elapsed >= 4000) {  // 3秒运行后停止
        setRelay(false);
      }
    }
  }

  // 检查休息时间是否结束
  if (inWaitPeriod && (millis() - relayStartTime > params.restDurationMinutes * 60000UL)) {
    inWaitPeriod = false;
    relayStartTime = 0;
    protectionEndTime = "";
    lastCycleTime = 0;
    inCycleRunning = false;
    Serial.println(F("休息期结束，恢复运行"));
  }
}

// 控制继电器
void setRelay(bool state) {
  digitalWrite(RELAY_PIN, state);
  params.relayState = state;
}

// Web页面
void handleRoot() {
  String html = R"rawliteral(
  <html>
    <head>
      <meta charset="UTF-8">
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <title>水泵控制器</title>
      <style>
        body {
          font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
          background: #f2f2f2;
          margin: 0;
          padding: 20px;
          color: #333;
        }
        .container {
          max-width: 800px;
          margin: 0 auto;
          background: #fff;
          padding: 20px;
          border-radius: 8px;
          box-shadow: 0 0 10px rgba(0,0,0,0.1);
        }
        h2, h3 {
          color: #444;
        }
        .status-card {
          padding: 15px;
          border: 1px solid #ddd;
          border-radius: 5px;
          margin-bottom: 20px;
          background: #fafafa;
        }
        .btn {
          display: inline-block;
          padding: 10px 20px;
          margin: 10px 0;
          font-size: 16px;
          color: #fff;
          background: #4CAF50;
          border: none;
          border-radius: 5px;
          cursor: pointer;
        }
        .btn:hover {
          background: #45a045;
        }
        .manual-btn {
          background: #2196F3;
        }
        .manual-btn:hover {
          background: #1976d2;
        }
        .time-period {
          margin-bottom: 20px;
          border: 1px solid #ccc;
          border-radius: 5px;
          padding: 15px;
          background: #f9f9f9;
        }
        .time-row {
          display: flex;
          align-items: center;
          gap: 10px;
          margin-top: 10px;
        }
        label {
          font-weight: bold;
        }
        select, input[type=number] {
          padding: 5px;
          border: 1px solid #ccc;
          border-radius: 4px;
          background: #fff;
        }
      </style>
      <script>
        function updateData() {
          fetch('/data').then(r => r.json()).then(data => {
            document.getElementById('mode').innerText = data.mode;
            document.getElementById('temp').innerText = data.temp;
            const stateElem = document.getElementById('state');
            stateElem.innerHTML = data.state + 
              (data.nextRun !== "-" ? "<br>恢复时间: " + data.nextRun : "");
            if (data.state === "工作中") {
              stateElem.style.color = "#4CAF50";
              stateElem.style.backgroundColor = "#e8f5e9";
            } else if (data.state === "已停止") {
              stateElem.style.color = "#FF9800";
              stateElem.style.backgroundColor = "#fff3e0";
            } else if (data.state === "保护中") {
              stateElem.style.color = "#F44336";
              stateElem.style.backgroundColor = "#ffebee";
            } else {
              stateElem.style.color = "#333";
              stateElem.style.backgroundColor = "#fafafa";
            }
            stateElem.setAttribute('data-state', data.state);
            document.getElementById('time').innerText = data.time;
            document.getElementById('workStatus').innerText = data.working;
          });
        }
        setInterval(updateData, 2000);
        function toggleMode() {
          fetch('/toggle', { method: 'POST' })
            .then(r => r.text())
            .then(msg => document.getElementById('modeBtn').innerText = msg);
        }
      </script>
    </head>
    <body>
      <div class="container">
        <div class="status-card">
          <h2>系统状态</h2>
          <p><label>工作模式:</label> <span id="mode">自动温控模式</span></p>
          <p><label>当前温度:</label> <span id="temp">-</span>℃</p>
          <p><label>水泵状态:</label> <span id="state" data-state="-">-</span></p>
          <p><label>当前时间:</label> <span id="time">-</span></p>
          <p><label>工作时间状态:</label> <span id="workStatus">-</span></p>
        </div>
        <button id="modeBtn" class="btn manual-btn" onclick="toggleMode()">切换手动运行</button>
        <hr>
        <form onsubmit="event.preventDefault(); fetch('/set', {
          method: 'POST',
          body: new FormData(this)
        }).then(r => r.text()).then(alert('设置已保存'))">
          <h3>温控设置</h3>
          <p>
            <label>目标温度:</label>
            <input type="number" step="0.1" name="target" value=")rawliteral"
                + String(params.targetTemp) + R"rawliteral(">
          </p>
          <p>
            <label>启动温差:</label>
            <input type="number" step="0.1" name="hysteresis" value=")rawliteral"
                + String(params.hysteresis) + R"rawliteral(">
          </p>
          <p>
            <label>水泵最大运行时长:</label>
            <input type="number" name="maxRun" value=")rawliteral"
                + String(params.maxRunDurationMinutes) + R"rawliteral(">
          </p>
          <p>
            <label>水泵休息时长:</label>
            <input type="number" name="restTime" value=")rawliteral"
                + String(params.restDurationMinutes) + R"rawliteral(">
          </p>
          <h3>工作时间段 (最多4个)</h3>
  )rawliteral";

  for (int i = 0; i < 4; i++) {
    int startHour = params.periods[i].start / 60;
    int startMin = params.periods[i].start % 60;
    int endHour = params.periods[i].end / 60;
    int endMin = params.periods[i].end % 60;
    html += "<div class='time-period'>";
    html += "<label>时间段 " + String(i + 1) + ":</label> ";
    html += "<input type='checkbox' name='period" + String(i) + "_enabled' " + (params.periods[i].enabled ? "checked" : "") + "> 启用<br>";
    html += "<div class='time-row'>";
    html += "<label>开始时间:</label>";
    html += "<select name='period" + String(i) + "_start_hour'>";
    for (int h = 0; h < 24; h++) {
      html += "<option value='" + String(h) + "'" + (h == startHour ? " selected" : "") + ">" + String(h) + "时</option>";
    }
    html += "</select>";
    html += "<select name='period" + String(i) + "_start_min'>";
    for (int m = 0; m < 60; m++) {
      html += "<option value='" + String(m) + "'" + (m == startMin ? " selected" : "") + ">" + (m < 10 ? "0" : "") + String(m) + "分</option>";
    }
    html += "</select>";
    html += "</div>";
    html += "<div class='time-row'>";
    html += "<label>结束时间:</label>";
    html += "<select name='period" + String(i) + "_end_hour'>";
    for (int h = 0; h < 24; h++) {
      html += "<option value='" + String(h) + "'" + (h == endHour ? " selected" : "") + ">" + String(h) + "时</option>";
    }
    html += "</select>";
    html += "<select name='period" + String(i) + "_end_min'>";
    for (int m = 0; m < 60; m++) {
      html += "<option value='" + String(m) + "'" + (m == endMin ? " selected" : "") + ">" + (m < 10 ? "0" : "") + String(m) + "分</option>";
    }
    html += "</select>";
    html += "</div>";
    html += "</div>";
  }

  html += R"rawliteral(
          <button type="submit" class="btn">保存设置</button>
        </form>
      </div>
    </body>
  </html>
  )rawliteral";

  server.send(200, "text/html; charset=UTF-8", html);
}

// 数据接口
void handleData() {
  String json;
  json.reserve(300);
  json = "{\"temp\":";
  json += String(readValidTemperature(), 1);
  json += ",\"state\":\"";
  if (!manualMode && inWaitPeriod) {
    json += "保护中";
    json += "\",\"nextRun\":\"";
    json += protectionEndTime;
  } else {
    json += digitalRead(RELAY_PIN) ? "工作中" : "已停止";
    json += "\",\"nextRun\":\"-";
  }
  json += "\",\"mode\":\"";
  json += manualMode ? "手动运行" : "自动温控模式";
  json += "\",\"modeBtn\":\"";
  json += manualMode ? "切换自动温控模式" : "切换手动运行";
  json += "\",\"time\":\"";
  time_t localTime = now();
  json += formatTime(localTime);
  json += "\",\"working\":\"";
  json += isWithinWorkingPeriod() ? "在工作时间" : "非工作时间";
  json += "\"}";
  server.send(200, "application/json", json);
}

// 时间格式化
String formatTime(time_t timestamp) {
  String t = String(hour(timestamp)) + ":" + (minute(timestamp) < 10 ? "0" : "") + String(minute(timestamp)) + ":" + (second(timestamp) < 10 ? "0" : "") + String(second(timestamp));
  return t;
}

// 处理设置请求
void handleSet() {
  if (server.hasArg("target")) {
    float newTemp = server.arg("target").toFloat();
    if (newTemp < 10 || newTemp > 90) {
      server.send(400, "text/plain", "温度范围10-90℃");
      return;
    }
    params.targetTemp = newTemp;
    params.modified = true;
  }
  if (server.hasArg("hysteresis")) {
    float hyst = server.arg("hysteresis").toFloat();
    if (hyst < 1 || hyst > 20) {
      server.send(400, "text/plain", "滞后值范围1-20℃");
      return;
    }
    params.hysteresis = hyst;
    params.modified = true;
  }
  if (server.hasArg("maxRun")) {
    int runTime = server.arg("maxRun").toInt();
    if (runTime < 1 || runTime > 1440) {
      server.send(400, "text/plain", "运行时长1-1440分钟");
      return;
    }
    params.maxRunDurationMinutes = runTime;
    params.modified = true;
  }
  if (server.hasArg("restTime")) {
    int restTime = server.arg("restTime").toInt();
    if (restTime < 1 || restTime > 1440) {
      server.send(400, "text/plain", "休息时长1-1440分钟");
      return;
    }
    params.restDurationMinutes = restTime;
    params.modified = true;
  }
  for (int i = 0; i < 4; i++) {
    String enabledArg = "period" + String(i) + "_enabled";
    params.periods[i].enabled = server.hasArg(enabledArg);
    String startHourArg = "period" + String(i) + "_start_hour";
    String startMinArg = "period" + String(i) + "_start_min";
    String endHourArg = "period" + String(i) + "_end_hour";
    String endMinArg = "period" + String(i) + "_end_min";
    if (server.hasArg(startHourArg) && server.hasArg(startMinArg) && server.hasArg(endHourArg) && server.hasArg(endMinArg)) {
      int startHour = server.arg(startHourArg).toInt();
      int startMin = server.arg(startMinArg).toInt();
      int endHour = server.arg(endHourArg).toInt();
      int endMin = server.arg(endMinArg).toInt();
      if (startHour < 0 || startHour > 23 || startMin < 0 || startMin > 59 || endHour < 0 || endHour > 23 || endMin < 0 || endMin > 59) {
        server.send(400, "text/plain", "时间格式错误");
        return;
      }
      params.periods[i].start = startHour * 60 + startMin;
      params.periods[i].end = endHour * 60 + endMin;
    }
  }
  saveParamsToEEPROM();
  server.send(200, "text/plain", "设置已保存");
}

// EEPROM操作
void initEEPROM() {
  EEPROM.begin(sizeof(SystemParams));
}

void saveParamsToEEPROM() {
  params.crc = calculateCRC((uint8_t *)&params, sizeof(SystemParams) - sizeof(uint32_t));
  EEPROM.put(0, params);
  EEPROM.commit();
  Serial.println(F("参数已保存至EEPROM"));
}

bool loadParamsFromEEPROM() {
  EEPROM.get(0, params);
  uint32_t crc = calculateCRC((uint8_t *)&params, sizeof(SystemParams) - sizeof(uint32_t));
  if (crc == params.crc) {
    Serial.println(F("EEPROM参数加载成功"));
    return true;
  } else {
    Serial.println(F("EEPROM校验失败，使用默认参数"));
    params = SystemParams();
    return false;
  }
}

uint32_t calculateCRC(uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1)
        crc = (crc >> 1) ^ 0xEDB88320;
      else
        crc >>= 1;
    }
  }
  return ~crc;
}

// 定时更新时间同步
void updateSystemTime() {
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 3600000 && WiFi.status() == WL_CONNECTED) {
    lastUpdate = millis();
    if (syncTime()) {
      Serial.println(F("时间同步成功"));
    } else {
      Serial.println(F("时间同步失败"));
    }
  }
}

// 切换运行模式
void handleToggle() {
  manualMode = !manualMode;
  if (manualMode) {
    inWaitPeriod = false;
    protectionEndTime = "";
    relayStartTime = 0;
    lastCycleTime = 0;
    inCycleRunning = false;
    heatingEnabled = false;
    setRelay(true);
    server.send(200, "text/plain", "手动运行切换中……");
  } else {
    relayStartTime = 0;
    lastCycleTime = 0;
    inCycleRunning = false;
    heatingEnabled = false;
    setRelay(false);
    server.send(200, "text/plain", "自动温控模式切换中……");
  }
}
