#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <FS.h>

// 硬件配置
#define RELAY_PIN 5
#define ONE_WIRE_BUS 4

// 网络配置
const char *ssid = "Zhou";
const char *password = "18601982023";
const char *mqtt_server = "192.168.2.1";
const int mqtt_port = 1883;
const char *mqtt_user = "admin";
const char *mqtt_password = "public";
const char *ntp_server = "192.168.2.1";

// MQTT 主题
const char *deviceA_status_topic = "deviceA/status";
const char *deviceB_status_topic = "deviceB/status";

// 日志相关
const char *logFile = "/log.txt";
const size_t maxLogSize = 20480;  // 最大日志大小20KB
String logBuffer = "";
const size_t maxBufferSize = 1024;  // 缓冲区最大1KB
unsigned long lastLogFlush = 0;
const unsigned long logFlushInterval = 30000;  // 每30秒写入一次

struct TimePeriod {
  bool enabled = false;
  uint8_t startHour = 0;
  uint8_t startMinute = 0;
  uint8_t endHour = 23;
  uint8_t endMinute = 59;
};

struct SystemParams {
  uint8_t marker = 0xA5;
  uint8_t version = 1;
  float targetTemp = 34.0;
  float hysteresis = 4.0;
  bool relayState = false;
  unsigned long maxRunDurationMinutes = 5;
  unsigned long restDurationMinutes = 10;
  bool pulseEnabled = false;
  unsigned long pulseOnMs = 5000;
  unsigned long pulseOffMs = 8000;
  bool enableLogging = true;  // 日志开关，仅控制文件日志
  TimePeriod periods[4] = {
    { false, 0, 0, 23, 59 },
    { false, 0, 0, 23, 59 },
    { false, 0, 0, 23, 59 },
    { false, 0, 0, 23, 59 }
  };
  uint32_t crc = 0;
};

// 全局 PROGMEM 变量
static const char html_part1[] PROGMEM = R"rawliteral(
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>水泵控制器</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 0; padding: 20px; color: #333; background: #f5f5f5; }
    .container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; box-shadow: 0 0 5px rgba(0,0,0,0.1); }
    h2 { color: #444; border-bottom: 1px solid #eee; padding-bottom: 10px; }
    .status-item { margin: 10px 0; display: flex; align-items: center; }
    .status-label { display: inline-block; width: 150px; font-weight: bold; }
    .btn { padding: 8px 15px; margin: 5px 0; font-size: 14px; color: white; background: #4CAF50; border: none; border-radius: 3px; cursor: pointer; display: inline-block; }
    .btn:hover { background: #45a049; }
    .btn-manual { background: #2196F3; }
    .btn-manual:hover { background: #0b7dda; }
    .time-period { margin: 15px 0; padding: 10px; border: 1px solid #ddd; background: #f9f9f9; }
    .time-control { margin: 5px 0; display: flex; align-items: center; }
    .time-label { display: inline-block; width: 150px; }
    select, input[type="number"], input[type="checkbox"] { padding: 5px; border: 1px solid #ddd; border-radius: 3px; box-sizing: border-box; }
    .state { padding: 5px 10px; border-radius: 3px; font-weight: bold; }
    .state-on { background: #dff0d8; color: #3c763d; }
    .state-off { background: #f2dede; color: #a94442; }
    .state-protect { background: #fcf8e3; color: #8a6d3b; }
    .device-b-online { color: #28a745; font-weight: bold; }
    .device-b-offline { color: #dc3545; font-weight: bold; }
    .sensor-online { color: #28a745; font-weight: bold; }
    .sensor-offline { color: #dc3545; font-weight: bold; }
    .next-run { color: #007bff; font-weight: bold; }
    .status-item > *, .time-control > * { margin-right: 10px; }
    .status-item:last-child, .time-control:last-child { margin-right: 0; }
    input[type="number"] { width: 100px; }
    form { margin-top: 20px; }
  </style>
  <script>
    function updateData() {
      fetch('/data').then(r => r.json()).then(data => {
        document.getElementById('mode').textContent = data.mode;
        document.getElementById('temp').textContent = data.temp;
        const stateElem = document.getElementById('state');
        stateElem.textContent = data.state;
        stateElem.className = 'state ';
        if(data.state === '工作中') stateElem.classList.add('state-on');
        else if(data.state === '已停止') stateElem.classList.add('state-off');
        else if(data.state === '保护中') stateElem.classList.add('state-protect');
        const deviceBStatus = document.getElementById('device-b-status');
        deviceBStatus.textContent = data.deviceBStatus;
        deviceBStatus.className = '';
        if(data.deviceBStatus === '在线') deviceBStatus.classList.add('device-b-online');
        else if(data.deviceBStatus === '离线') deviceBStatus.classList.add('device-b-offline');
        const sensorStatus = document.getElementById('sensor-status');
        sensorStatus.textContent = data.sensorStatus;
        sensorStatus.className = '';
        if(data.sensorStatus === '在线') sensorStatus.classList.add('sensor-online');
        else if(data.sensorStatus === '离线') sensorStatus.classList.add('sensor-offline');
        const nextRun = document.getElementById('next-run');
        nextRun.textContent = data.nextRun !== '-' ? '恢复时间: ' + data.nextRun : '';
        nextRun.className = data.nextRun !== '-' ? 'next-run' : '';
        document.getElementById('time').textContent = data.time;
        document.getElementById('auto-mode-work-status').textContent = data.working;
        document.getElementById('mode-btn').textContent = data.modeBtn;
      }).catch(error => console.error('更新状态失败:', error));
    }
    function toggleMode() {
      fetch('/toggle', { method: 'POST' })
        .then(r => { if (!r.ok) throw new Error('切换模式失败'); return r.text(); })
        .then(msg => { document.getElementById('mode-btn').textContent = msg; updateData(); })
        .catch(error => { console.error('切换模式失败:', error); alert('切换模式失败: ' + error.message); });
    }
    function clearLog() {
      if (confirm('确定清除日志吗？')) {
        fetch('/clear_log', { method: 'POST' })
          .then(r => { if (!r.ok) throw new Error('清除日志失败'); return r.text(); })
          .then(() => alert('日志已清除'))
          .catch(error => alert('清除日志失败: ' + error.message));
      }
    }
    function downloadLog() {
      fetch('/download_log')
        .then(r => { if (!r.ok) throw new Error('下载失败'); return r.blob(); })
        .then(blob => {
          const url = window.URL.createObjectURL(blob);
          const a = document.createElement('a');
          a.href = url;
          a.download = 'log.txt';
          document.body.appendChild(a);
          a.click();
          a.remove();
          window.URL.revokeObjectURL(url);
        })
        .catch(error => alert('下载日志失败: ' + error.message));
    }
    function validateForm() {
      const form = document.getElementById('settings-form');
      const inputs = form.querySelectorAll('input, select');
      for (let input of inputs) {
        if (input.required && !input.value) {
          alert('请填写所有必填字段');
          return false;
        }
      }
      return true;
    }
    setInterval(updateData, 2000);
    updateData();
  </script>
</head>
<body>
  <div class="container">
    <h2>系统状态</h2>
    <div class="status-item"><span class="status-label">工作模式:</span><span id="mode">自动温控模式</span></div>
    <div class="status-item"><span class="status-label">当前温度:</span><span id="temp">-</span>℃</div>
    <div class="status-item"><span class="status-label">水泵状态:</span><span id="state" class="state">-</span><div id="next-run"></div></div>
    <div class="status-item"><span class="status-label">当前时间:</span><span id="time">-</span></div>
    <div class="status-item"><span class="status-label">自动模式工作时间:</span><span id="auto-mode-work-status">-</span></div>
    <div class="status-item"><span class="status-label">设备B状态:</span><span id="device-b-status">-</span></div>
    <div class="status-item"><span class="status-label">传感器状态:</span><span id="sensor-status">-</span></div>
    <button id="mode-btn" class="btn btn-manual" onclick="toggleMode()">切换手动运行</button>
    <h2>参数设置</h2>
    <form id="settings-form" onsubmit="if(!validateForm()) return false; fetch('/set', { method: 'POST', body: new FormData(this) }).then(r => { if (!r.ok) throw new Error('保存设置失败'); return r.text(); }).then(() => alert('设置已保存')).then(updateData).catch(error => alert('保存设置失败: ' + error.message));">
      <div class="status-item"><span class="status-label">目标温度:</span><input type="number" step="0.1" min="20" max="50" name="targetTemp" value="%TARGET_TEMP%" required> ℃</div>
      <div class="status-item"><span class="status-label">启动温差:</span><input type="number" step="0.1" min="1" max="10" name="hysteresis" value="%HYSTERESIS%" required> ℃</div>
      <div class="status-item"><span class="status-label">最大运行时长:</span><input type="number" min="1" max="1440" name="maxRunDurationMinutes" value="%MAX_RUN_DURATION%" required> 分钟</div>
      <div class="status-item"><span class="status-label">休息时长:</span><input type="number" min="1" max="1440" name="restDurationMinutes" value="%REST_DURATION%" required> 分钟</div>
      <div class="status-item"><span class="status-label">脉冲功能:</span><input type="checkbox" name="pulseEnabled" %PULSE_ENABLED%></div>
      <div class="status-item"><span class="status-label">脉冲开启时间:</span><input type="number" min="100" max="60000" name="pulseOnMs" value="%PULSE_ON_MS%" required> 毫秒</div>
      <div class="status-item"><span class="status-label">脉冲关闭时间:</span><input type="number" min="100" max="60000" name="pulseOffMs" value="%PULSE_OFF_MS%" required> 毫秒</div>
      <div class="status-item"><span class="status-label">日志记录:</span><input type="checkbox" name="enableLogging" %ENABLE_LOGGING%></div>
      <h3>自动模式工作时间段</h3>
)rawliteral";

static const char html_part2[] PROGMEM = R"rawliteral(
      <button type="submit" class="btn">保存设置</button>
    </form>
    <h2>日志管理</h2>
    <div class="status-item">
      <button class="btn" onclick="downloadLog()">下载日志</button>
      <button class="btn" onclick="clearLog()">清除日志</button>
    </div>
  </div>
</body>
</html>
)rawliteral";

// 全局变量
WiFiClient espClient;
PubSubClient client(espClient);
ESP8266WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntp_server, 28800);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
SystemParams params;

float lastValidTemp = 25.0;
float lastPrintedTemp = -127.0;
bool tempConversionStarted = false;
unsigned long tempRequestTime = 0;
uint8_t tempLogCounter = 0;
bool manualMode = false;
bool heatingEnabled = false;
bool inWaitPeriod = false;
unsigned long relayStartTime = 0;
unsigned long lastCycleTime = 0;
unsigned long lastLogTime = 0;
String protectionEndTime = "";
bool deviceBOnline = false;
bool deviceBRelayState = false;
unsigned long lastReconnectAttempt = 0;
unsigned long lastWiFiCheckTime = 0;
unsigned long lastWiFiRetryTime = 0;
unsigned long lastUpdate = 0;
unsigned long lastDeviceBStatusTime = 0;
unsigned long lastStatusUpdateTime = 0;
unsigned long lastMqttCheckTime = 0;
unsigned long lastDeviceCheckTime = 0;
bool lastMqttConnectedState = false;
bool lastDeviceBOnlineState = false;
bool isFirstWiFiConnect = true;
bool isFirstMqttConnect = true;
const unsigned long reconnectInterval = 15000;
const unsigned long wifiCheckInterval = 120000;
const unsigned long wifiRetryInterval = 180000;
const unsigned long mqttCheckInterval = 10000;
const unsigned long statusUpdateInterval = 2000;
const unsigned long deviceCheckInterval = 60000;
static unsigned long lastTempCheck = 0;
const unsigned long tempCheckInterval = 100;
bool tempSensorFailed = false;        // DS18B20 是否故障
uint8_t tempErrorCount = 0;           // 连续异常计数
const uint8_t maxTempErrorCount = 5;  // 连续5次异常判为故障

// 函数声明
void pinSetup();
void connectWiFi();
void setupMQTT();
void callback(char *topic, byte *payload, unsigned int length);
void reconnect();
void initWebServer();
void saveParamsToEEPROM();
void loadParamsFromEEPROM();
uint32_t calculateCRC(uint8_t *data, size_t len);
float readValidTemperature();
void setRelay(bool state);
void checkTemperature();
void updateSystemTime();
bool isWithinAutoModeWorkingPeriod();
String formatTime(time_t t);
void checkWiFiStatus();
void checkMqttStatus();
bool isDeviceBOnline(unsigned long timeout);
void checkDeviceBStatus();
String getLogTimestamp();
void logEvent(const String &event);
void flushLogBuffer();
void initLog();
void debugParams();

void pinSetup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
}

void connectWiFi() {
  Serial.println(F("开始连接WiFi..."));
  logEvent(F("开始连接WiFi..."));
  WiFi.begin(ssid, password);
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 30000) {
    delay(10);
    ESP.wdtFeed();
  }
  if (WiFi.status() == WL_CONNECTED) {
    if (isFirstWiFiConnect) {
      Serial.print(F("WiFi连接成功，IP: "));
      Serial.println(WiFi.localIP());
      Serial.print(F("设备名称: DeviceA-"));
      Serial.println(ESP.getChipId());
      logEvent(F("WiFi连接成功，IP: ") + WiFi.localIP().toString());
      isFirstWiFiConnect = false;
    }
    Serial.print(F("当前IP: "));
    Serial.println(WiFi.localIP());
    logEvent(F("WiFi连接成功，IP: ") + WiFi.localIP().toString());
  } else {
    Serial.println(F("WiFi连接失败"));
    logEvent(F("WiFi连接失败"));
  }
}

void setupMQTT() {
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  Serial.println(F("MQTT服务器配置完成"));
  logEvent(F("MQTT服务器配置完成"));
}

void callback(char *topic, byte *payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  if (strcmp(topic, deviceB_status_topic) == 0) {
    if (message == "ON" || message == "OFF") {
      deviceBOnline = true;
      lastDeviceBStatusTime = millis();
      deviceBRelayState = (message == "ON");
      if (deviceBOnline != lastDeviceBOnlineState) {
        lastDeviceBOnlineState = deviceBOnline;
        Serial.println(F("设备B上线"));
        logEvent(F("设备B上线"));
      }
    }
  }
}

void reconnect() {
  if (millis() - lastReconnectAttempt < reconnectInterval) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi未连接，跳过MQTT连接"));
    logEvent(F("WiFi未连接，跳过MQTT连接"));
    lastReconnectAttempt = millis();
    return;
  }
  Serial.println(F("尝试连接MQTT..."));
  logEvent(F("尝试连接MQTT..."));
  String clientId = "DeviceA-" + String(ESP.getChipId());
  int attempt = 0;
  for (int i = 0; i < 5; i++) {
    attempt++;
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_password, deviceA_status_topic, 0, true, "OFF")) {
      if (isFirstMqttConnect) {
        Serial.println(F("MQTT连接正常"));
        logEvent(F("MQTT连接正常"));
        isFirstMqttConnect = false;
      }
      client.subscribe(deviceB_status_topic);
      client.publish(deviceA_status_topic, params.relayState ? "ON" : "OFF", true);
      Serial.println(F("订阅B状态并发布A状态"));
      lastReconnectAttempt = 0;
      return;
    }
    Serial.print(F("第"));
    Serial.print(attempt);
    Serial.print(F("次失败，状态: "));
    Serial.println(client.state());
    logEvent(F("MQTT连接失败，第") + String(attempt) + F("次，状态: ") + String(client.state()));
    delay(2000);
  }
  Serial.println(F("MQTT重连失败"));
  logEvent(F("MQTT重连失败"));
  lastReconnectAttempt = millis();
}

void debugParams() {
  for (int i = 0; i < 4; i++) {
    Serial.print(F("时间段 "));
    Serial.print(i + 1);
    Serial.print(F(": enabled="));
    Serial.print(params.periods[i].enabled);
    Serial.print(F(", start="));
    Serial.print(params.periods[i].startHour);
    Serial.print(F(":"));
    Serial.print(params.periods[i].startMinute);
    Serial.print(F(", end="));
    Serial.print(params.periods[i].endHour);
    Serial.print(F(":"));
    Serial.println(params.periods[i].endMinute);
  }
}

void initWebServer() {
  server.on("/", HTTP_GET, []() {
    debugParams();
    String html = String(FPSTR(html_part1));
    char tempBuf[10];
    snprintf(tempBuf, sizeof(tempBuf), "%.1f", params.targetTemp);
    html.replace("%TARGET_TEMP%", tempBuf);
    snprintf(tempBuf, sizeof(tempBuf), "%.1f", params.hysteresis);
    html.replace("%HYSTERESIS%", tempBuf);
    html.replace("%MAX_RUN_DURATION%", String(params.maxRunDurationMinutes));
    html.replace("%REST_DURATION%", String(params.restDurationMinutes));
    html.replace("%PULSE_ON_MS%", String(params.pulseOnMs));
    html.replace("%PULSE_OFF_MS%", String(params.pulseOffMs));
    html.replace("%PULSE_ENABLED%", params.pulseEnabled ? "checked" : "");
    html.replace("%ENABLE_LOGGING%", params.enableLogging ? "checked" : "");

    for (int i = 0; i < 4; i++) {
      html += "<div class='time-period'>";
      html += "<div class='time-control'><span class='time-label'>时间段 " + String(i + 1) + ":</span><input type='checkbox' name='period" + String(i) + "_enabled' " + (params.periods[i].enabled ? "checked" : "") + "></div>";
      html += "<div class='time-control'><span class='time-label'>开始时间:</span>";
      html += "<select name='period" + String(i) + "_start_hour'>";
      for (int h = 0; h < 24; h++) {
        html += "<option value='" + String(h) + "'" + (h == params.periods[i].startHour ? " selected" : "") + ">" + String(h) + "时</option>";
      }
      html += "</select>";
      html += "<select name='period" + String(i) + "_start_min'>";
      for (int m = 0; m < 60; m += 5) {
        html += "<option value='" + String(m) + "'" + (m == params.periods[i].startMinute ? " selected" : "") + ">" + (m < 10 ? "0" : "") + String(m) + "分</option>";
      }
      html += "</select></div>";
      html += "<div class='time-control'><span class='time-label'>结束时间:</span>";
      html += "<select name='period" + String(i) + "_end_hour'>";
      for (int h = 0; h < 24; h++) {
        html += "<option value='" + String(h) + "'" + (h == params.periods[i].endHour ? " selected" : "") + ">" + String(h) + "时</option>";
      }
      html += "</select>";
      html += "<select name='period" + String(i) + "_end_min'>";
      for (int m = 0; m < 60; m += 5) {
        html += "<option value='" + String(m) + "'" + (m == params.periods[i].endMinute ? " selected" : "") + ">" + (m < 10 ? "0" : "") + String(m) + "分</option>";
      }
      html += "</select></div>";
      html += "</div>";
    }

    html += String(FPSTR(html_part2));
    server.sendHeader("Cache-Control", "no-cache");
    server.send(200, "text/html", html);
    Serial.println(F("收到GET / 请求并响应"));
  });

  server.on("/data", HTTP_GET, []() {
    char tempBuf[10];
    snprintf(tempBuf, sizeof(tempBuf), "%.1f", lastValidTemp);
    String json = "{";
    json += "\"mode\":\"" + String(manualMode ? "手动运行" : "自动温控模式") + "\",";
    json += "\"temp\":\"" + String(tempBuf) + "\",";
    json += "\"state\":\"" + String(digitalRead(RELAY_PIN) ? "工作中" : (inWaitPeriod ? "保护中" : "已停止")) + "\",";
    json += "\"nextRun\":\"" + String(inWaitPeriod ? protectionEndTime : "-") + "\",";
    json += "\"time\":\"" + formatTime(now()) + "\",";
    json += "\"working\":\"" + String(isWithinAutoModeWorkingPeriod() ? "在自动模式工作时间" : "非自动模式工作时间") + "\",";
    json += "\"deviceBStatus\":\"" + String(deviceBOnline ? "在线" : "离线") + "\",";
    json += "\"sensorStatus\":\"" + String(tempSensorFailed ? "离线" : "在线") + "\",";
    json += "\"modeBtn\":\"" + String(manualMode ? "切换自动温控模式" : "切换手动运行") + "\"";
    json += "}";
    server.send(200, "application/json", json);
    Serial.println(F("收到GET /data 请求并响应"));
  });

  server.on("/set", HTTP_POST, []() {
    if (!server.hasArg("targetTemp") || server.arg("targetTemp").isEmpty()) {
      Serial.println(F("保存设置失败：缺少或无效的目标温度参数"));
      server.send(400, "text/plain", "缺少或无效的目标温度参数");
      return;
    }
    params.targetTemp = server.arg("targetTemp").toFloat();
    params.targetTemp = constrain(params.targetTemp, 20.0, 50.0);

    if (!server.hasArg("hysteresis") || server.arg("hysteresis").isEmpty()) {
      Serial.println(F("保存设置失败：缺少或无效的启动温差参数"));
      server.send(400, "text/plain", "缺少或无效的启动温差参数");
      return;
    }
    params.hysteresis = server.arg("hysteresis").toFloat();
    params.hysteresis = constrain(params.hysteresis, 1.0, 10.0);

    if (!server.hasArg("maxRunDurationMinutes") || server.arg("maxRunDurationMinutes").isEmpty()) {
      Serial.println(F("保存设置失败：缺少或无效的最大运行时长参数"));
      server.send(400, "text/plain", "缺少或无效的最大运行时长参数");
      return;
    }
    params.maxRunDurationMinutes = server.arg("maxRunDurationMinutes").toInt();

    if (!server.hasArg("restDurationMinutes") || server.arg("restDurationMinutes").isEmpty()) {
      Serial.println(F("保存设置失败：缺少或无效的休息时长参数"));
      server.send(400, "text/plain", "缺少或无效的休息时长参数");
      return;
    }
    params.restDurationMinutes = server.arg("restDurationMinutes").toInt();

    if (!server.hasArg("pulseOnMs") || server.arg("pulseOnMs").isEmpty()) {
      Serial.println(F("保存设置失败：缺少或无效的脉冲开启时间参数"));
      server.send(400, "text/plain", "缺少或无效的脉冲开启时间参数");
      return;
    }
    params.pulseOnMs = server.arg("pulseOnMs").toInt();

    if (!server.hasArg("pulseOffMs") || server.arg("pulseOffMs").isEmpty()) {
      Serial.println(F("保存设置失败：缺少或无效的脉冲关闭时间参数"));
      server.send(400, "text/plain", "缺少或无效的脉冲关闭时间参数");
      return;
    }
    params.pulseOffMs = server.arg("pulseOffMs").toInt();

    params.pulseEnabled = server.hasArg("pulseEnabled") && (server.arg("pulseEnabled") == "on");
    params.enableLogging = server.hasArg("enableLogging") && (server.arg("enableLogging") == "on");

    for (int i = 0; i < 4; i++) {
      String enabledArg = String("period") + String(i) + "_enabled";
      String startHourArg = String("period") + String(i) + "_start_hour";
      String startMinArg = String("period") + String(i) + "_start_min";
      String endHourArg = String("period") + String(i) + "_end_hour";
      String endMinArg = String("period") + String(i) + "_end_min";

      params.periods[i].enabled = server.hasArg(enabledArg) && (server.arg(enabledArg) == "on");
      params.periods[i].startHour = server.hasArg(startHourArg) && !server.arg(startHourArg).isEmpty() ? server.arg(startHourArg).toInt() : params.periods[i].startHour;
      params.periods[i].startMinute = server.hasArg(startMinArg) && !server.arg(startMinArg).isEmpty() ? server.arg(startMinArg).toInt() : params.periods[i].startMinute;
      params.periods[i].endHour = server.hasArg(endHourArg) && !server.arg(endHourArg).isEmpty() ? server.arg(endHourArg).toInt() : params.periods[i].endHour;
      params.periods[i].endMinute = server.hasArg(endMinArg) && !server.arg(endMinArg).isEmpty() ? server.arg(endMinArg).toInt() : params.periods[i].endMinute;

      params.periods[i].startHour = constrain(params.periods[i].startHour, 0, 23);
      params.periods[i].startMinute = constrain(params.periods[i].startMinute, 0, 59);
      params.periods[i].endHour = constrain(params.periods[i].endHour, 0, 23);
      params.periods[i].endMinute = constrain(params.periods[i].endMinute, 0, 59);
    }

    debugParams();
    inWaitPeriod = false;
    relayStartTime = 0;
    lastCycleTime = 0;
    protectionEndTime = "";
    heatingEnabled = false;
    setRelay(false);

    saveParamsToEEPROM();
    server.send(200, "text/plain", "设置已保存");
    Serial.println(F("收到POST /set 请求并保存设置"));
  });

  server.on("/toggle", HTTP_POST, []() {
    manualMode = !manualMode;
    if (manualMode) {
      inWaitPeriod = false;
      protectionEndTime = "";
      relayStartTime = 0;
      lastCycleTime = 0;
      if (isDeviceBOnline(30000)) {
        setRelay(true);
      } else {
        setRelay(false);
      }
    } else {
      relayStartTime = 0;
      lastCycleTime = 0;
      heatingEnabled = false;
      setRelay(false);
    }
    server.send(200, "text/plain", manualMode ? "切换自动温控模式" : "切换手动运行");
    Serial.println(F("收到POST /toggle 请求并切换模式"));
    logEvent(F("切换模式至: ") + String(manualMode ? "手动运行" : "自动温控模式"));
  });

  server.on("/download_log", HTTP_GET, []() {
    flushLogBuffer();
    File file = SPIFFS.open(logFile, "r");
    if (!file) {
      Serial.println(F("无法打开日志文件"));
      logEvent(F("无法打开日志文件"));
      server.send(500, "text/plain", "无法打开日志文件");
      return;
    }
    if (file.size() == 0) {
      file.close();
      Serial.println(F("日志文件为空"));
      logEvent(F("日志文件为空"));
      server.send(200, "text/plain", "日志文件为空");
      return;
    }
    server.sendHeader("Content-Type", "application/octet-stream");
    server.sendHeader("Content-Disposition", "attachment; filename=log.txt");
    server.sendHeader("Content-Length", String(file.size()));
    server.sendHeader("Connection", "close");
    server.streamFile(file, "application/octet-stream");
    file.close();
    Serial.println(F("收到GET /download_log 请求并响应"));
    logEvent(F("日志文件已下载"));
  });

  server.on("/clear_log", HTTP_POST, []() {
    flushLogBuffer();
    if (SPIFFS.remove(logFile)) {
      Serial.println(F("日志文件已清除"));
      logEvent(F("日志文件已清除"));
      server.send(200, "text/plain", "日志已清除");
    } else {
      Serial.println(F("清除日志文件失败"));
      logEvent(F("清除日志文件失败"));
      server.send(500, "text/plain", "清除日志文件失败");
    }
    Serial.println(F("收到POST /clear_log 请求"));
  });

  server.onNotFound([]() {
    Serial.println(F("收到未知请求"));
    logEvent(F("收到未知请求"));
    server.send(404, "text/plain", "Not Found");
  });

  server.begin();
  Serial.println(F("Web服务器已启动"));
  logEvent(F("Web服务器已启动"));
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

void saveParamsToEEPROM() {
  if (sizeof(SystemParams) > 4096) {
    Serial.println(F("EEPROM尺寸超出限制"));
    logEvent(F("EEPROM尺寸超出限制，保存设置失败"));
    return;
  }

  static SystemParams lastSavedParams;
  if (memcmp(&params, &lastSavedParams, sizeof(SystemParams)) == 0) {
    Serial.println(F("参数未更改，跳过EEPROM写入"));
    logEvent(F("参数未更改，跳过EEPROM写入"));
    return;
  }

  params.crc = calculateCRC((uint8_t *)&params, sizeof(SystemParams) - sizeof(uint32_t));
  EEPROM.put(0, params);
  if (EEPROM.commit()) {
    Serial.println(F("参数成功保存至EEPROM"));
    memcpy(&lastSavedParams, &params, sizeof(SystemParams));
  } else {
    Serial.println(F("EEPROM写入失败"));
    logEvent(F("EEPROM写入失败，保存设置失败"));
    return;
  }

  SystemParams verifyParams;
  EEPROM.get(0, verifyParams);
  if (memcmp(&params, &verifyParams, sizeof(SystemParams)) == 0) {
    Serial.println(F("EEPROM写入验证成功"));
    logEvent(F("EEPROM写入验证成功"));
  } else {
    Serial.println(F("EEPROM写入验证失败"));
    logEvent(F("EEPROM写入验证失败，保存设置失败"));
  }
}

void loadParamsFromEEPROM() {
  SystemParams tempParams;
  EEPROM.get(0, tempParams);
  uint32_t storedCRC = tempParams.crc;
  tempParams.crc = 0;
  uint32_t calculatedCRC = calculateCRC((uint8_t *)&tempParams, sizeof(SystemParams) - sizeof(uint32_t));
  if (tempParams.marker == 0xA5 && calculatedCRC == storedCRC) {
    params = tempParams;
    params.targetTemp = constrain(params.targetTemp, 20.0, 50.0);
    params.hysteresis = constrain(params.hysteresis, 1.0, 10.0);
    params.maxRunDurationMinutes = constrain(params.maxRunDurationMinutes, 1, 1440);
    params.restDurationMinutes = constrain(params.restDurationMinutes, 1, 1440);
    params.pulseOnMs = constrain(params.pulseOnMs, 100, 60000);
    params.pulseOffMs = constrain(params.pulseOffMs, 100, 60000);
    params.enableLogging = params.enableLogging ? true : false;
    for (int i = 0; i < 4; i++) {
      params.periods[i].startHour = constrain(params.periods[i].startHour, 0, 23);
      params.periods[i].endHour = constrain(params.periods[i].endHour, 0, 23);
      params.periods[i].startMinute = constrain(params.periods[i].startMinute, 0, 59);
      params.periods[i].endMinute = constrain(params.periods[i].endMinute, 0, 59);
      params.periods[i].enabled = params.periods[i].enabled ? true : false;
    }
    Serial.println(F("从EEPROM加载参数成功"));
    logEvent(F("从EEPROM加载参数成功"));
    debugParams();
  } else {
    Serial.println(F("EEPROM校验失败，初始化默认参数"));
    logEvent(F("EEPROM校验失败，初始化默认参数"));
    params = SystemParams();
    saveParamsToEEPROM();
  }
}

float readValidTemperature() {
  float temp = sensors.getTempCByIndex(0);
  if (temp <= -127.0 || temp > 125.0) {
    tempErrorCount++;
    if (!tempSensorFailed) {
      Serial.println(F("温度传感器偶发异常，读取值: ") + String(temp, 1));
      logEvent(F("温度传感器偶发异常，读取值: ") + String(temp, 1));
    }
    if (tempErrorCount >= maxTempErrorCount && !tempSensorFailed) {
      tempSensorFailed = true;
    }
    return lastValidTemp;
  }
  if (tempErrorCount > 0 || tempSensorFailed) {
    tempErrorCount = 0;
    if (tempSensorFailed) {
      tempSensorFailed = false;
    }
  }
  lastValidTemp = temp;
  if (abs(temp - lastPrintedTemp) >= 0.5 || lastPrintedTemp == -127.0) {
    lastPrintedTemp = temp;
  }
  return temp;
}

void setRelay(bool state) {
  if (params.relayState != state) {
    digitalWrite(RELAY_PIN, state);
    params.relayState = state;
    Serial.print(F("继电器状态已设置为: "));
    Serial.println(state ? "ON" : "OFF");
    logEvent(F("继电器状态已设置为: ") + String(state ? "ON" : "OFF"));
    delay(50);
    if (client.connected()) {
      client.publish(deviceA_status_topic, state ? "ON" : "OFF");
      Serial.print(F("发布设备A状态: "));
      Serial.println(state ? "ON" : "OFF");
    }
  }
}

bool isDeviceBOnline(unsigned long timeout) {
  if (deviceBOnline && (millis() - lastDeviceBStatusTime < timeout)) {
    return true;
  }
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
  return deviceBOnline;
}

void checkDeviceBStatus() {
  static unsigned long lastDeviceBCheck = 0;
  const unsigned long deviceBCheckInterval = 100;

  if (millis() - lastDeviceBCheck >= deviceBCheckInterval) {
    unsigned long timeout = params.relayState ? 3500 : 30000;
    if (millis() - lastDeviceBStatusTime >= timeout && deviceBOnline) {
      deviceBOnline = false;
      if (deviceBOnline != lastDeviceBOnlineState) {
        lastDeviceBOnlineState = deviceBOnline;
        Serial.println(F("设备B掉线"));
        logEvent(F("设备B掉线"));
      }
    }
    lastDeviceBCheck = millis();
  }
}

void checkTemperature() {
  static unsigned long lastRelayChange = 0;
  static unsigned long pulseCycleStartTime = 0;
  const unsigned long relayDebounceTime = 500;

  if (tempSensorFailed) {
    if (millis() - lastRelayChange >= relayDebounceTime) {
      setRelay(false);
      inWaitPeriod = false;
      relayStartTime = 0;
      lastCycleTime = 0;
      pulseCycleStartTime = 0;
      protectionEndTime = "";
      heatingEnabled = false;
      lastRelayChange = millis();
    }
    return;
  }

  if (!deviceBOnline) {
    if (millis() - lastRelayChange >= relayDebounceTime) {
      setRelay(false);
      inWaitPeriod = false;
      relayStartTime = 0;
      lastCycleTime = 0;
      pulseCycleStartTime = 0;
      protectionEndTime = "";
      heatingEnabled = false;
      lastRelayChange = millis();
    }
    return;
  }

  if (manualMode) {
    if (millis() - lastRelayChange >= relayDebounceTime) {
      setRelay(true);
      lastRelayChange = millis();
    }
    return;
  }

  if (!isWithinAutoModeWorkingPeriod()) {
    if (millis() - lastRelayChange >= relayDebounceTime) {
      setRelay(false);
      inWaitPeriod = false;
      relayStartTime = 0;
      lastCycleTime = 0;
      pulseCycleStartTime = 0;
      protectionEndTime = "";
      heatingEnabled = false;
      lastRelayChange = millis();
    }
    return;
  }

  float currentTemp = readValidTemperature();

  if (currentTemp >= params.targetTemp) {
    heatingEnabled = false;
  } else if (currentTemp <= params.targetTemp - params.hysteresis) {
    heatingEnabled = true;
  }

  if (!heatingEnabled) {
    if (millis() - lastRelayChange >= relayDebounceTime) {
      setRelay(false);
      inWaitPeriod = false;
      relayStartTime = 0;
      lastCycleTime = 0;
      pulseCycleStartTime = 0;
      protectionEndTime = "";
      lastRelayChange = millis();
    }
    return;
  }

  if (!inWaitPeriod && params.pulseEnabled) {
    if (relayStartTime == 0) {
      relayStartTime = millis();
      pulseCycleStartTime = millis();
    }

    unsigned long elapsedTime = millis() - relayStartTime;
    if (elapsedTime > params.maxRunDurationMinutes * 60000UL) {
      if (millis() - lastRelayChange >= relayDebounceTime) {
        setRelay(false);
        inWaitPeriod = true;
        lastCycleTime = 0;
        pulseCycleStartTime = 0;
        relayStartTime = millis();
        time_t endTime = now() + (params.restDurationMinutes * 60);
        protectionEndTime = formatTime(endTime);
        Serial.print(F("保护结束时间: "));
        Serial.println(protectionEndTime);
        logEvent(F("保护结束时间: ") + protectionEndTime);
        lastRelayChange = millis();
      }
      return;
    }

    unsigned long pulseElapsed = millis() - pulseCycleStartTime;
    unsigned long pulseCycleDuration = params.pulseOnMs + params.pulseOffMs;

    if (pulseElapsed >= pulseCycleDuration) {
      pulseCycleStartTime = millis();
      pulseElapsed = 0;
    }

    if (pulseElapsed < params.pulseOnMs) {
      if (millis() - lastRelayChange >= relayDebounceTime && !digitalRead(RELAY_PIN)) {
        setRelay(true);
        lastRelayChange = millis();
      }
    } else {
      if (millis() - lastRelayChange >= relayDebounceTime && digitalRead(RELAY_PIN)) {
        setRelay(false);
        lastRelayChange = millis();
      }
    }
  } else if (!inWaitPeriod && !params.pulseEnabled) {
    if (relayStartTime == 0 && !digitalRead(RELAY_PIN)) {
      relayStartTime = millis();
    }

    unsigned long elapsedTime = millis() - relayStartTime;
    if (elapsedTime > params.maxRunDurationMinutes * 60000UL) {
      if (millis() - lastRelayChange >= relayDebounceTime) {
        setRelay(false);
        inWaitPeriod = true;
        lastCycleTime = 0;
        relayStartTime = millis();
        time_t endTime = now() + (params.restDurationMinutes * 60);
        protectionEndTime = formatTime(endTime);
        Serial.print(F("保护结束时间: "));
        Serial.println(protectionEndTime);
        logEvent(F("保护结束时间: ") + protectionEndTime);
        lastRelayChange = millis();
      }
      return;
    }

    if (millis() - lastRelayChange >= relayDebounceTime) {
      setRelay(true);
      lastRelayChange = millis();
    }
  }

  if (inWaitPeriod && (millis() - relayStartTime > params.restDurationMinutes * 60000UL)) {
    inWaitPeriod = false;
    relayStartTime = 0;
    pulseCycleStartTime = 0;
    protectionEndTime = "";
    lastCycleTime = 0;
  }
}

void updateSystemTime() {
  if (millis() - lastUpdate >= 300000) {
    if (WiFi.status() == WL_CONNECTED) {
      if (timeClient.update()) {
        setTime(timeClient.getEpochTime());
        Serial.print(F("时间同步成功: "));
        Serial.println(formatTime(now()));
        logEvent(F("时间同步成功: ") + formatTime(now()));
      } else {
        Serial.println(F("时间同步失败"));
        logEvent(F("时间同步失败"));
      }
    } else {
      Serial.println(F("WiFi未连接，跳过时间同步"));
      logEvent(F("WiFi未连接，跳过时间同步"));
    }
    lastUpdate = millis();
  }
}

bool isWithinAutoModeWorkingPeriod() {
  time_t nowTime = now();
  int currentHour = hour(nowTime);
  int currentMinute = minute(nowTime);
  int currentMinutes = currentHour * 60 + currentMinute;

  bool anyPeriodEnabled = false;
  for (int i = 0; i < 4; i++) {
    if (params.periods[i].enabled) {
      anyPeriodEnabled = true;
      int startMinutes = params.periods[i].startHour * 60 + params.periods[i].startMinute;
      int endMinutes = params.periods[i].endHour * 60 + params.periods[i].endMinute;
      if (startMinutes <= endMinutes) {
        if (currentMinutes >= startMinutes && currentMinutes <= endMinutes) {
          return true;
        }
      } else {
        if (currentMinutes >= startMinutes || currentMinutes <= endMinutes) {
          return true;
        }
      }
    }
  }
  return !anyPeriodEnabled;
}

String formatTime(time_t t) {
  char buffer[20];
  snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", hour(t), minute(t), second(t));
  return String(buffer);
}

String getLogTimestamp() {
  time_t t = now();
  char buffer[20];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", year(t), month(t), day(t), hour(t), minute(t), second(t));
  return String(buffer);
}

void logEvent(const String &event) {
  String logEntry = "[" + getLogTimestamp() + "] " + event + "\n";
  Serial.print(F("日志: "));
  Serial.print(logEntry);
  if (params.enableLogging) {
    logBuffer += logEntry;
    if (logBuffer.length() >= maxBufferSize || millis() - lastLogFlush >= logFlushInterval) {
      flushLogBuffer();
    }
  }
}

void flushLogBuffer() {
  if (!params.enableLogging || logBuffer.length() == 0) return;

  File file = SPIFFS.open(logFile, "a");
  if (!file) {
    Serial.println(F("无法打开日志文件"));
    logEvent(F("无法打开日志文件"));
    return;
  }

  if (file.size() + logBuffer.length() > maxLogSize) {
    file.close();
    SPIFFS.remove(logFile);
    file = SPIFFS.open(logFile, "w");
    if (!file) {
      Serial.println(F("无法创建新日志文件"));
      logEvent(F("无法创建新日志文件"));
      return;
    }
    logEvent(F("日志文件已清空，重新开始记录"));
  }

  if (file.print(logBuffer)) {
    Serial.println(F("日志写入成功"));
  } else {
    Serial.println(F("日志写入失败"));
    logEvent(F("日志写入失败"));
  }
  file.close();
  logBuffer = "";
  lastLogFlush = millis();
}

void initLog() {
  if (!SPIFFS.begin()) {
    Serial.println(F("SPIFFS初始化失败"));
    logEvent(F("SPIFFS初始化失败"));
    return;
  }
  Serial.println(F("SPIFFS初始化成功"));
  logEvent(F("SPIFFS初始化成功"));
  File file = SPIFFS.open(logFile, "a");
  if (!file) {
    Serial.println(F("无法打开日志文件，创建新文件"));
    logEvent(F("无法打开日志文件，创建新文件"));
    file = SPIFFS.open(logFile, "w");
  }
  file.close();
  logEvent(F("系统启动"));
}

void checkWiFiStatus() {
  if (millis() - lastWiFiCheckTime < wifiCheckInterval) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi断开，尝试重连"));
    logEvent(F("WiFi断开，尝试重连"));
    WiFi.reconnect();
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 30000) {
      delay(10);
      ESP.wdtFeed();
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(F("WiFi重连成功"));
      Serial.print(F("当前IP: "));
      Serial.println(WiFi.localIP());
      logEvent(F("WiFi重连成功，IP: ") + WiFi.localIP().toString());
    } else {
      Serial.println(F("WiFi重连失败"));
      logEvent(F("WiFi重连失败"));
    }
  }
  lastWiFiCheckTime = millis();
}

void checkMqttStatus() {
  if (millis() - lastMqttCheckTime < mqttCheckInterval) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED && !client.connected()) {
    reconnect();
  }
  lastMqttCheckTime = millis();
}

void setup() {
  ESP.wdtEnable(8000);
  Serial.begin(115200);
  delay(3000);
  pinSetup();
  Serial.println(F("初始化 EEPROM..."));
  logEvent(F("EEPROM 初始化完成"));
  EEPROM.begin(sizeof(SystemParams));
  Serial.println(F("EEPROM 初始化完成"));
  loadParamsFromEEPROM();
  initLog();
  sensors.begin();
  sensors.setWaitForConversion(false);
  sensors.setResolution(9);
  if (sensors.getDeviceCount() == 0) {
    tempSensorFailed = true;
  }
  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    timeClient.begin();
    if (timeClient.update()) {
      setTime(timeClient.getEpochTime());
      Serial.print(F("初始时间同步成功: "));
      Serial.println(formatTime(now()));
      logEvent(F("初始时间同步成功: ") + formatTime(now()));
    } else {
      Serial.println(F("时间同步失败"));
      logEvent(F("时间同步失败"));
    }
  } else {
    Serial.println(F("WiFi未连接，跳过时间同步"));
    logEvent(F("WiFi未连接，跳过时间同步"));
  }
  setupMQTT();
  reconnect();
  initWebServer();
  checkTemperature();
}

void loop() {
  ESP.wdtFeed();
  server.handleClient();

  checkWiFiStatus();
  checkMqttStatus();
  checkDeviceBStatus();

  bool currentMqttConnected = client.connected();
  if (currentMqttConnected != lastMqttConnectedState) {
    lastMqttConnectedState = currentMqttConnected;
    Serial.println(currentMqttConnected ? F("MQTT连接恢复") : F("MQTT断开连接"));
    logEvent(currentMqttConnected ? F("MQTT连接恢复") : F("MQTT断开连接"));
    if (!currentMqttConnected) {
      Serial.println(F("跳过client.loop()"));
    }
  }

  if (currentMqttConnected) {
    client.loop();
  }

  updateSystemTime();

  if (millis() - lastTempCheck >= tempCheckInterval) {
    if (!tempConversionStarted) {
      sensors.requestTemperatures();
      tempConversionStarted = true;
    } else if (sensors.isConversionComplete()) {
      lastValidTemp = readValidTemperature();
      tempConversionStarted = false;
    }
    lastTempCheck = millis();
  }

  checkTemperature();

  static unsigned long lastStatusUpdate = 0;
  if (millis() - lastStatusUpdate >= statusUpdateInterval) {
    if (client.connected()) {
      client.publish(deviceA_status_topic, params.relayState ? "ON" : "OFF");
    }
    lastStatusUpdate = millis();
  }

  if (!params.relayState && millis() - lastDeviceCheckTime >= deviceCheckInterval) {
    isDeviceBOnline(30000);
    lastDeviceCheckTime = millis();
  }

  if (lastWiFiRetryTime != 0 && millis() - lastWiFiRetryTime >= wifiRetryInterval) {
    Serial.println(F("WiFi断开，尝试重连"));
    logEvent(F("WiFi断开，尝试重连"));
    WiFi.reconnect();
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 30000) {
      delay(10);
      ESP.wdtFeed();
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(F("WiFi重连成功"));
      Serial.print(F("当前IP: "));
      Serial.println(WiFi.localIP());
      logEvent(F("WiFi重连成功，IP: ") + WiFi.localIP().toString());
    } else {
      Serial.println(F("WiFi重连失败"));
      logEvent(F("WiFi重连失败"));
    }
    lastWiFiRetryTime = millis();
  }

  if (millis() - lastLogFlush >= logFlushInterval) {
    flushLogBuffer();
  }

  delay(10);
}