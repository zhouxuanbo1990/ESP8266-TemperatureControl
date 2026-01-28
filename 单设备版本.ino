#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <FS.h>
#include <ArduinoJson.h>

// 硬件配置
#define RELAY_PIN 5
#define ONE_WIRE_BUS 4
#define PIN_LED 16 // 新增，D0 作为WiFi状态灯

// 网络配置
const char *ssid = "Zhou";
const char *password = "18601982023";
const char *custom_ntp_servers[] = {
  "192.168.2.1",
  "ntp.aliyun.com",
  "ntp.tencent.com",
  "ntp.tuna.tsinghua.edu.cn",
  "cn.ntp.org.cn"
};
const int custom_ntp_server_count = sizeof(custom_ntp_servers) / sizeof(custom_ntp_servers[0]);

// 静态 IP 配置
const IPAddress local_ip(192, 168, 2, 5);
const IPAddress gateway_ip(192, 168, 2, 1);
const IPAddress subnet_mask(255, 255, 255, 0);
const IPAddress dns_ip(192, 168, 2, 1);

// 日志相关
#define logFile "/log.txt"
#define maxLogSize 20000
#define maxBufferSize 1024
#define EEPROM_PARAMS_ADDR 128
unsigned long lastLogFlush = 0;
const unsigned long logFlushInterval = 30000; // 30秒
String logBuffer = "";

// 状态变量
bool serverStarted = false;
bool timeSynced = false;
bool wifiInitialConnectionDone = false; // 新增声明并初始化

// 系统参数
struct TimePeriod
{
  bool enabled = false;
  uint8_t startHour = 0;
  uint8_t startMinute = 0;
  uint8_t endHour = 23;
  uint8_t endMinute = 59;
};

struct SystemParams
{
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
  bool enableLogging = true;
  TimePeriod periods[4] = {
      {false, 0, 0, 23, 59},
      {false, 0, 0, 23, 59},
      {false, 0, 0, 23, 59},
      {false, 0, 0, 23, 59}};
  uint32_t crc = 0;
};

// HTML 内容（未修改，与原代码A一致）
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
        const sensorStatus = document.getElementById('sensor-status');
        sensorStatus.textContent = data.sensorStatus;
        sensorStatus.className = '';
        if(data.sensorStatus === '在线') sensorStatus.classList.add('sensor-online');
        else if(data.sensorStatus === '离线/故障') sensorStatus.classList.add('sensor-offline');
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
    function fetchWithTimeout(resource, options = {}) {
      const { timeout = 5000 } = options;
      return Promise.race([
        fetch(resource, options),
        new Promise((_, reject) => setTimeout(() => reject(new Error('请求超时')), timeout))
      ]);
    }
    function saveSettings() {
      const form = document.getElementById('settings-form');
      const formData = new FormData(form);
      const submitBtn = form.querySelector('button[type="submit"]');
      if (submitBtn) submitBtn.disabled = true;
      fetchWithTimeout('/set', { method: 'POST', body: formData, timeout: 8000 })
        .then(r => {
          if (!r.ok) throw new Error('保存设置失败');
          return r.text();
        })
        .then(() => {
          alert('设置已保存');
          updateData();
        })
        .catch(error => {
          if (error.message === '请求超时') {
            alert('保存设置失败: 设备响应超时，请检查网络或设备状态');
          } else {
            alert('保存设置失败: ' + error.message);
          }
        })
        .finally(() => {
          if (submitBtn) submitBtn.disabled = false;
        });
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
    <div class="status-item"><span class="status-label">传感器状态:</span><span id="sensor-status">-</span></div>
    <button id="mode-btn" class="btn btn-manual" onclick="toggleMode()">切换手动运行</button>
    <h2>参数设置</h2>
    <form id="settings-form" onsubmit="if(!validateForm()) return false; saveSettings(); return false;">
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
ESP8266WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, custom_ntp_servers[0], 28800);
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
String protectionEndTime = "";
unsigned long lastUpdate = 0;
unsigned long lastYieldTime = 0;
unsigned long lastValidTime = 0;
unsigned long lastWiFiCheckTime = 0;
bool tempSensorFailed = false;
uint8_t tempErrorCount = 0;
const uint8_t maxTempErrorCount = 5;
unsigned long lastWiFiAttemptTime = 0;
const unsigned long wifiAttemptInterval = 5000;
int wifiConnectionAttempts = 0;
const int maxWiFiConnectionAttempts = 10;
unsigned long lastLogFlushTime = 0;
const unsigned long logFlushIntervalMs = 30000;
const unsigned long wifiCheckInterval = 120000;
const unsigned long wifiRetryInterval = 180000;
const unsigned long statusUpdateInterval = 2000;

// CRC32 计算
uint32_t calcCrc32(const uint8_t *data, size_t len)
{
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; ++i)
  {
    crc ^= data[i];
    for (int j = 0; j < 8; j++)
    {
      if (crc & 1)
        crc = (crc >> 1) ^ 0xEDB88320;
      else
        crc >>= 1;
    }
  }
  return crc ^ 0xFFFFFFFF;
}

void pinSetup()
{
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(PIN_LED, OUTPUT);    // 新增
  digitalWrite(PIN_LED, HIGH); // 默认关闭（高电平熄灭）
}

void tryConnectWiFi(bool forceAttempt = false)
{
  if (WiFi.status() == WL_CONNECTED)
    return;

  if (!forceAttempt && (millis() - lastWiFiAttemptTime < wifiAttemptInterval))
    return;
  lastWiFiAttemptTime = millis();

  WiFi.begin(ssid, password);
}

void connectWiFi()
{
  tryConnectWiFi(true);
}

void initWebServer()
{
  server.on("/", HTTP_GET, []()
            {
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

              for (int i = 0; i < 4; i++)
              {
                html += "<div class='time-period'>";
                html += "<div class='time-control'><span class='time-label'>时间段 " + String(i + 1) + ":</span><input type='checkbox' name='period" + String(i) + "_enabled' " + (params.periods[i].enabled ? "checked" : "") + "></div>";
                html += "<div class='time-control'><span class='time-label'>开始时间:</span>";
                html += "<select name='period" + String(i) + "_start_hour'>";
                for (int h = 0; h < 24; h++)
                {
                  html += "<option value='" + String(h) + "'" + (h == params.periods[i].startHour ? " selected" : "") + ">" + String(h) + "时</option>";
                }
                html += "</select>";
                html += "<select name='period" + String(i) + "_start_min'>";
                for (int m = 0; m < 60; m += 5)
                {
                  html += "<option value='" + String(m) + "'" + (m == params.periods[i].startMinute ? " selected" : "") + ">" + (m < 10 ? "0" : "") + String(m) + "分</option>";
                }
                html += "</select></div>";
                html += "<div class='time-control'><span class='time-label'>结束时间:</span>";
                html += "<select name='period" + String(i) + "_end_hour'>";
                for (int h = 0; h < 24; h++)
                {
                  html += "<option value='" + String(h) + "'" + (h == params.periods[i].endHour ? " selected" : "") + ">" + String(h) + "时</option>";
                }
                html += "</select>";
                html += "<select name='period" + String(i) + "_end_min'>";
                for (int m = 0; m < 60; m += 5)
                {
                  html += "<option value='" + String(m) + "'" + (m == params.periods[i].endMinute ? " selected" : "") + ">" + (m < 10 ? "0" : "") + String(m) + "分</option>";
                }
                html += "</select></div>";
                html += "</div>";
              }

              html += String(FPSTR(html_part2));
              server.sendHeader("Cache-Control", "no-cache");
              server.send(200, "text/html; charset=utf-8", html);
              // Serial.println(F("收到GET / 请求并响应"));
              // logEvent(F("收到GET / 请求并响应"));
            });

  server.on("/data", HTTP_GET, []()
            {
              DynamicJsonDocument doc(512);
              char tempBuf[10];
              snprintf(tempBuf, sizeof(tempBuf), "%.1f", lastValidTemp);
              doc["mode"] = manualMode ? "手动运行" : "自动温控模式";
              doc["temp"] = tempBuf;
              doc["state"] = digitalRead(RELAY_PIN) ? "工作中" : (inWaitPeriod ? "保护中" : "已停止");
              doc["nextRun"] = inWaitPeriod ? protectionEndTime : "-";
              doc["time"] = formatTime(now());
              doc["working"] = isWithinAutoModeWorkingPeriod() ? "在自动模式工作时间" : "非自动模式工作时间";
              doc["sensorStatus"] = tempSensorFailed ? "离线/故障" : "在线";
              doc["modeBtn"] = manualMode ? "切换自动温控模式" : "切换手动运行";
              String jsonOutput;
              serializeJson(doc, jsonOutput);
              server.send(200, "application/json; charset=utf-8", jsonOutput);
              // Serial.println(F("收到GET /data 请求并响应"));
              // logEvent(F("收到GET /data 请求并响应"));
            });

  server.on("/set", HTTP_POST, []()
            {
              if (!server.hasArg("targetTemp") || server.arg("targetTemp").isEmpty())
              {
                Serial.println(F("保存设置失败：缺少或无效的目标温度参数"));
                server.send(400, "text/plain; charset=utf-8", "缺少或无效的目标温度参数");
                return;
              }
              params.targetTemp = server.arg("targetTemp").toFloat();
              params.targetTemp = constrain(params.targetTemp, 20.0, 50.0);

              if (!server.hasArg("hysteresis") || server.arg("hysteresis").isEmpty())
              {
                Serial.println(F("保存设置失败：缺少或无效的启动温差参数"));
                server.send(400, "text/plain; charset=utf-8", "缺少或无效的启动温差参数");
                return;
              }
              params.hysteresis = server.arg("hysteresis").toFloat();
              params.hysteresis = constrain(params.hysteresis, 1.0, 10.0);

              if (!server.hasArg("maxRunDurationMinutes") || server.arg("maxRunDurationMinutes").isEmpty())
              {
                Serial.println(F("保存设置失败：缺少或无效的最大运行时长参数"));
                server.send(400, "text/plain; charset=utf-8", "缺少或无效的最大运行时长参数");
                return;
              }
              params.maxRunDurationMinutes = server.arg("maxRunDurationMinutes").toInt();

              if (!server.hasArg("restDurationMinutes") || server.arg("restDurationMinutes").isEmpty())
              {
                Serial.println(F("保存设置失败：缺少或无效的休息时长参数"));
                server.send(400, "text/plain; charset=utf-8", "缺少或无效的休息时长参数");
                return;
              }
              params.restDurationMinutes = server.arg("restDurationMinutes").toInt();

              if (!server.hasArg("pulseOnMs") || server.arg("pulseOnMs").isEmpty())
              {
                Serial.println(F("保存设置失败：缺少或无效的脉冲开启时间参数"));
                server.send(400, "text/plain; charset=utf-8", "缺少或无效的脉冲开启时间参数");
                return;
              }
              params.pulseOnMs = server.arg("pulseOnMs").toInt();

              if (!server.hasArg("pulseOffMs") || server.arg("pulseOffMs").isEmpty())
              {
                Serial.println(F("保存设置失败：缺少或无效的脉冲关闭时间参数"));
                server.send(400, "text/plain; charset=utf-8", "缺少或无效的脉冲关闭时间参数");
                return;
              }
              params.pulseOffMs = server.arg("pulseOffMs").toInt();

              params.pulseEnabled = server.hasArg("pulseEnabled") && (server.arg("pulseEnabled") == "on");
              params.enableLogging = server.hasArg("enableLogging") && (server.arg("enableLogging") == "on");

              for (int i = 0; i < 4; i++)
              {
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

              inWaitPeriod = false;
              relayStartTime = 0;
              lastCycleTime = 0;
              protectionEndTime = "";
              heatingEnabled = false;
              setRelay(false);

              saveParamsToEEPROM();
              server.send(200, "text/plain; charset=utf-8", "设置已保存");
              Serial.println(F("收到POST /set 请求并保存设置"));
              // logEvent(F("收到POST /set 请求并保存设置"));
            });

  server.on("/toggle", HTTP_POST, []()
            {
    manualMode = !manualMode;
    if (manualMode) {
      inWaitPeriod = false;
      protectionEndTime = "";
      relayStartTime = 0;
      lastCycleTime = 0;
      setRelay(true);
    } else {
      relayStartTime = 0;
      lastCycleTime = 0;
      heatingEnabled = false;
      setRelay(false);
    }
    server.send(200, "text/plain; charset=utf-8", manualMode ? "切换自动温控模式" : "切换手动运行");
    Serial.println(F("收到POST /toggle 请求并切换模式"));
    logEvent(F("切换模式至: ") + String(manualMode ? "手动运行" : "自动温控模式")); });

  server.on("/download_log", HTTP_GET, []()
            {
              flushLogBuffer();
              File file = SPIFFS.open(logFile, "r");
              if (!file)
              {
                Serial.println(F("无法打开日志文件"));
                logEvent(F("无法打开日志文件"));
                server.send(500, "text/plain; charset=utf-8", "无法打开日志文件");
                return;
              }
              if (file.size() == 0)
              {
                file.close();
                Serial.println(F("日志文件为空"));
                logEvent(F("日志文件为空"));
                server.send(200, "text/plain; charset=utf-8", "日志文件为空");
                return;
              }
              server.sendHeader("Content-Type", "application/octet-stream");
              server.sendHeader("Content-Disposition", "attachment; filename=log.txt");
              server.sendHeader("Content-Length", String(file.size()));
              server.sendHeader("Connection", "close");
              server.streamFile(file, "application/octet-stream");
              file.close();
              Serial.println(F("收到GET /download_log 请求并响应"));
              // logEvent(F("收到GET /download_log 请求并响应"));
            });

  server.on("/clear_log", HTTP_POST, []()
            {
              flushLogBuffer();
              if (SPIFFS.remove(logFile))
              {
                Serial.println(F("日志文件已清除"));
                logEvent(F("日志文件已清除"));
                server.send(200, "text/plain; charset=utf-8", "日志已清除");
              }
              else
              {
                Serial.println(F("清除日志文件失败"));
                logEvent(F("清除日志文件失败"));
                server.send(500, "text/plain; charset=utf-8", "清除日志文件失败");
              }
              Serial.println(F("收到POST /clear_log 请求"));
              // logEvent(F("收到POST /clear_log 请求"));
            });

  server.onNotFound([]()
                    {
    Serial.println(F("收到未知请求"));
    //logEvent(F("收到未知请求"));
    server.send(404, "text/plain; charset=utf-8", "Not Found"); });

  server.begin();
  serverStarted = true;
  Serial.println(F("Web服务器已启动"));
  logEvent(F("Web服务器已启动"));
}

void saveParamsToEEPROM()
{
  if (sizeof(SystemParams) > 4096)
  {
    Serial.println(F("EEPROM尺寸超出限制"));
    logEvent(F("EEPROM尺寸超出限制"));
    return;
  }
  static SystemParams lastSavedParams;
  if (memcmp(&params, &lastSavedParams, sizeof(SystemParams)) == 0)
  {
    return;
  }
  params.marker = 0xA5;
  params.version = 1;
  params.crc = calcCrc32((const uint8_t *)&params, sizeof(SystemParams) - sizeof(uint32_t));
  EEPROM.begin(sizeof(SystemParams) + EEPROM_PARAMS_ADDR);
  uint8_t *ptr = (uint8_t *)&params;
  for (size_t i = 0; i < sizeof(SystemParams); i++)
  {
    EEPROM.write(EEPROM_PARAMS_ADDR + i, ptr[i]);
  }
  EEPROM.commit();
  memcpy(&lastSavedParams, &params, sizeof(SystemParams));
}

void loadParamsFromEEPROM()
{
  SystemParams tempParams;
  EEPROM.begin(sizeof(SystemParams) + EEPROM_PARAMS_ADDR);
  uint8_t *ptr = (uint8_t *)&tempParams;
  for (size_t i = 0; i < sizeof(SystemParams); i++)
  {
    ptr[i] = EEPROM.read(EEPROM_PARAMS_ADDR + i);
  }
  uint32_t storedCRC = tempParams.crc;
  tempParams.crc = 0;
  uint32_t calculatedCRC = calcCrc32((const uint8_t *)&tempParams, sizeof(SystemParams) - sizeof(uint32_t));
  if (tempParams.marker == 0xA5 && calculatedCRC == storedCRC)
  {
    params = tempParams;
    params.targetTemp = constrain(params.targetTemp, 20.0, 50.0);
    params.hysteresis = constrain(params.hysteresis, 1.0, 10.0);
    params.maxRunDurationMinutes = constrain(params.maxRunDurationMinutes, 1, 1440);
    params.restDurationMinutes = constrain(params.restDurationMinutes, 1, 1440);
    params.pulseOnMs = constrain(params.pulseOnMs, 100, 60000);
    params.pulseOffMs = constrain(params.pulseOffMs, 100, 60000);
    for (int i = 0; i < 4; i++)
    {
      params.periods[i].startHour = constrain(params.periods[i].startHour, 0, 23);
      params.periods[i].endHour = constrain(params.periods[i].endHour, 0, 23);
      params.periods[i].startMinute = constrain(params.periods[i].startMinute, 0, 59);
      params.periods[i].endMinute = constrain(params.periods[i].endMinute, 0, 59);
      params.periods[i].enabled = params.periods[i].enabled ? true : false;
    }
    Serial.println(F("EEPROM 加载成功"));
    // logEvent(F("EEPROM 加载成功"));
  }
  else
  {
    params = SystemParams();
    saveParamsToEEPROM();
    Serial.println(F("EEPROM 加载失败，使用默认参数"));
    // logEvent(F("EEPROM 加载失败，使用默认参数"));
  }
}

float readValidTemperature()
{
  float temp = sensors.getTempCByIndex(0);
  static int sensorRecoverCount = 0;
  
  if (temp <= -127.0 || temp > 125.0)
  {
    tempErrorCount++;
    sensorRecoverCount = 0;
    if (!tempSensorFailed)
    {
      Serial.println(F("温度传感器偶发异常，读取值: ") + String(temp, 1));
      logEvent(F("温度传感器偶发异常，读取值: ") + String(temp, 1));
    }
    if (tempErrorCount >= maxTempErrorCount && !tempSensorFailed)
    {
      tempSensorFailed = true;
      Serial.println(F("温度传感器故障，已进入保护状态"));
      logEvent(F("温度传感器故障，已进入保护状态"));
    }
    return lastValidTemp;
  }
  if (tempSensorFailed)
  {
    sensorRecoverCount++;
    if (sensorRecoverCount >= 5)
    {
      tempSensorFailed = false;
      tempErrorCount = 0;
      Serial.println(F("温度传感器自动恢复，退出保护状态"));
      logEvent(F("温度传感器自动恢复，退出保护状态"));
    }
  }
  else
  {
    sensorRecoverCount = 0;
    tempErrorCount = 0;
  }
  if (abs(temp - lastPrintedTemp) >= 0.5 || lastPrintedTemp == -127.0)
  {
    lastPrintedTemp = temp;
    Serial.println(F("温度更新: ") + String(temp, 1) + F("°C"));
  }
  lastValidTemp = temp;
  return temp;
}

void setRelay(bool state)
{
  if (params.relayState != state)
  {
    digitalWrite(RELAY_PIN, state);
    params.relayState = state;
    Serial.print(F("继电器状态已设置为: "));
    Serial.println(state ? "ON" : "OFF");
    logEvent(F("继电器状态已设置为: ") + String(state ? "ON" : "OFF"));
    delay(50);
  }
}

void checkTemperature()
{
  static unsigned long lastRelayChange = 0;
  static unsigned long pulseCycleStartTime = 0;
  const unsigned long relayDebounceTime = 500;

  if (tempSensorFailed)
  {
    if (millis() - lastRelayChange >= relayDebounceTime)
    {
      setRelay(false);
      inWaitPeriod = false;
      relayStartTime = 0;
      lastCycleTime = 0;
      pulseCycleStartTime = 0;
      protectionEndTime = "";
      heatingEnabled = false;
      lastRelayChange = millis();
      logEvent(F("温度传感器故障，已停止水泵"));
    }
    return;
  }

  if (manualMode)
  {
    if (millis() - lastRelayChange >= relayDebounceTime)
    {
      setRelay(true);
      lastRelayChange = millis();
    }
    return;
  }

  if (!isWithinAutoModeWorkingPeriod())
  {
    if (millis() - lastRelayChange >= relayDebounceTime)
    {
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

  float currentTemp = lastValidTemp;

  if (currentTemp >= params.targetTemp)
  {
    heatingEnabled = false;
  }
  else if (currentTemp <= params.targetTemp - params.hysteresis)
  {
    heatingEnabled = true;
  }

  if (!heatingEnabled)
  {
    if (millis() - lastRelayChange >= relayDebounceTime)
    {
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

  if (!inWaitPeriod && params.pulseEnabled)
  {
    if (relayStartTime == 0)
    {
      relayStartTime = millis();
      pulseCycleStartTime = millis();
    }

    unsigned long elapsedTime = millis() - relayStartTime;
    if (elapsedTime > params.maxRunDurationMinutes * 60000UL)
    {
      if (millis() - lastRelayChange >= relayDebounceTime)
      {
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

    if (pulseElapsed >= pulseCycleDuration)
    {
      pulseCycleStartTime = millis();
      pulseElapsed = 0;
    }

    if (pulseElapsed < params.pulseOnMs)
    {
      if (millis() - lastRelayChange >= relayDebounceTime && !digitalRead(RELAY_PIN))
      {
        setRelay(true);
        lastRelayChange = millis();
      }
    }
    else
    {
      if (millis() - lastRelayChange >= relayDebounceTime && digitalRead(RELAY_PIN))
      {
        setRelay(false);
        lastRelayChange = millis();
      }
    }
  }
  else if (!inWaitPeriod && !params.pulseEnabled)
  {
    if (relayStartTime == 0 && !digitalRead(RELAY_PIN))
    {
      relayStartTime = millis();
    }

    unsigned long elapsedTime = millis() - relayStartTime;
    if (elapsedTime > params.maxRunDurationMinutes * 60000UL)
    {
      if (millis() - lastRelayChange >= relayDebounceTime)
      {
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

    if (millis() - lastRelayChange >= relayDebounceTime)
    {
      setRelay(true);
      lastRelayChange = millis();
    }
  }

  if (inWaitPeriod && (millis() - relayStartTime > params.restDurationMinutes * 60000UL))
  {
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
      bool success = false;
      for (int i = 0; i < custom_ntp_server_count; i++) {
        timeClient.setPoolServerName(custom_ntp_servers[i]);
        if (timeClient.update()) {
          lastValidTime = timeClient.getEpochTime();
          setTime(lastValidTime);
          timeSynced = true;
          Serial.printf("时间同步成功 (%s): %s\n", custom_ntp_servers[i], formatTime(now()).c_str());
          logEvent(F("时间同步成功"));
          success = true;
          break;
        }
        Serial.printf("时间同步失败 (%s)\n", custom_ntp_servers[i]);
      }
      
      if (!success) {
        lastValidTime = millis() / 1000;
        setTime(lastValidTime);
        timeSynced = false;
        Serial.println(F("所有时间服务器同步失败，使用系统计数器时间"));
        logEvent(F("所有时间服务器同步失败"));
      }
    } else {
      Serial.println(F("WiFi未连接，跳过时间同步"));
      if (lastValidTime == 0) {
        lastValidTime = millis() / 1000;
      } else {
        lastValidTime += (millis() - lastUpdate) / 1000;
      }
      setTime(lastValidTime);
      timeSynced = false;
    }
    lastUpdate = millis();
  }
}

bool isWithinAutoModeWorkingPeriod()
{
  time_t nowTime = now();
  int currentHour = hour(nowTime);
  int currentMinute = minute(nowTime);
  int currentMinutes = currentHour * 60 + currentMinute;

  bool isInAnyActivePeriod = false;
  bool anyPeriodConfiguredAndEnabled = false;
  for (int i = 0; i < 4; i++)
  {
    if (params.periods[i].enabled)
    {
      anyPeriodConfiguredAndEnabled = true;
      int startMinutes = params.periods[i].startHour * 60 + params.periods[i].startMinute;
      int endMinutes = params.periods[i].endHour * 60 + params.periods[i].endMinute;
      if (startMinutes <= endMinutes)
      {
        if (currentMinutes >= startMinutes && currentMinutes <= endMinutes)
        {
          isInAnyActivePeriod = true;
          break;
        }
      }
      else
      {
        if (currentMinutes >= startMinutes || currentMinutes <= endMinutes)
        {
          isInAnyActivePeriod = true;
          break;
        }
      }
    }
  }
  if (!anyPeriodConfiguredAndEnabled)
  {
    return true;
  }
  return isInAnyActivePeriod;
}

String formatTime(time_t t)
{
  char buffer[30];
  snprintf(buffer, sizeof(buffer), "[%04d-%02d-%02d %02d:%02d:%02d]", year(t), month(t), day(t), hour(t), minute(t), second(t));
  return String(buffer);
}

void checkWiFiStatus()
{
  if (millis() - lastWiFiCheckTime < wifiCheckInterval)
  {
    return;
  }
  tryConnectWiFi();
  lastWiFiCheckTime = millis();
}

void logEvent(String message)
{
  String timestamp = formatTime(now());
  String logLine = timestamp + " " + message;
  if (params.enableLogging)
  {
    logBuffer += logLine + "\n";
    if (logBuffer.length() > maxBufferSize || millis() - lastLogFlush >= logFlushInterval)
      flushLogBuffer();
  }
  else
  {
    Serial.println(logLine); // 仅在未启用logEvent时输出到串口
  }
}

void flushLogBuffer()
{
  if (logBuffer.length() == 0 || !params.enableLogging)
    return;

  // 限制文件操作频率
  static unsigned long lastFileWrite = 0;
  if (millis() - lastFileWrite < 1000) // 每秒最多写入一次
    return;

  File file = SPIFFS.open(logFile, "a");
  if (!file)
  {
    Serial.println(F("无法打开日志文件"));
    // logEvent(F("无法打开日志文件")); // 避免递归
    return;
  }

  file.print(logBuffer);
  size_t fileSize = file.size(); // 关闭前获取文件大小
  file.close(); // 立即关闭文件
  lastFileWrite = millis();
  lastLogFlush = millis(); // 新增：日志刷新时间

  if (fileSize > maxLogSize)
  {
    SPIFFS.remove(logFile);
    File newFile = SPIFFS.open(logFile, "w"); // 立即新建空文件
    if (newFile) newFile.close();
    Serial.println(F("日志文件已清空，重新开始记录"));
    // logEvent(F("日志文件已清空，重新开始记录"));
  }

  logBuffer = ""; // 清空缓冲区
  // Serial.println(F("日志写入成功"));
}

void initLog()
{
  if (!SPIFFS.begin())
  {
    Serial.println(F("SPIFFS初始化失败"));
    // logEvent(F("SPIFFS初始化失败"));
    return;
  }
  Serial.println(F("SPIFFS初始化成功"));
  // logEvent(F("SPIFFS初始化成功"));
  File file = SPIFFS.open(logFile, "a");
  if (!file)
  {
    Serial.println(F("无法打开日志文件，创建新文件"));
    // logEvent(F("无法打开日志文件，创建新文件"));
    file = SPIFFS.open(logFile, "w");
    if (file)
    {
      Serial.println(F("日志文件已创建"));
      // logEvent(F("日志文件已创建"));
    }
    else
    {
      Serial.println(F("日志文件创建失败"));
      // logEvent(F("日志文件创建失败"));
    }
  }
  file.close();
}

void initEEPROM()
{
  EEPROM.begin(sizeof(SystemParams) + EEPROM_PARAMS_ADDR);
  Serial.println(F("EEPROM初始化完成"));
  // logEvent(F("EEPROM初始化完成"));
  loadParamsFromEEPROM();
}

void setup()
{
  ESP.wdtEnable(8000);
  Serial.begin(115200);
  delay(3000);
  pinSetup();
  initLog();
  initEEPROM();
  Serial.println(F("系统启动"));
  // logEvent(F("系统启动"));
  sensors.begin();
  sensors.setWaitForConversion(false);
  sensors.setResolution(9);
  if (sensors.getDeviceCount() == 0)
  {
    tempSensorFailed = true;
    Serial.println(F("未找到DS18B20传感器"));
    logEvent(F("未找到DS18B20传感器"));
  }
  else
  {
    oneWire.reset();
    sensors.requestTemperatures();
    tempConversionStarted = true;
    tempRequestTime = millis();
  }
  Serial.println(F("等待WiFi连接..."));
  // 增强 WiFi 稳定性配置
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.config(local_ip, gateway_ip, subnet_mask, dns_ip); // 设置静态 IP
  WiFi.setAutoReconnect(true);           // 开启系统底层自动重连
  WiFi.setSleepMode(WIFI_NONE_SLEEP);     // 禁用休眠模式

  WiFi.begin(ssid, password);
  unsigned long wifiStart = millis();
  static unsigned long ledTimer = 0;
  static bool ledState = false;
  // 尝试连接30秒
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 30000) {
    // 启动阶段WiFi未连上时LED闪烁
    if (millis() - ledTimer > 500) {
      ledTimer = millis();
      ledState = !ledState;
      digitalWrite(PIN_LED, ledState ? LOW : HIGH);  // 闪烁
    }
    delay(100);
    ESP.wdtFeed();
  }

  // WiFi连接后处理
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(PIN_LED, LOW);
    String wifiMsg = String(F("WiFi首次连接成功，IP: ")) + WiFi.localIP().toString();
    Serial.println(wifiMsg);
    logEvent(wifiMsg);
    
    timeClient.begin();
    bool success = false;
    for (int i = 0; i < custom_ntp_server_count; i++) {
      timeClient.setPoolServerName(custom_ntp_servers[i]);
      if (timeClient.update()) {
        lastValidTime = timeClient.getEpochTime();
        setTime(lastValidTime);
        timeSynced = true;
        Serial.printf("启动时间同步成功 (%s): %s\n", custom_ntp_servers[i], formatTime(now()).c_str());
        logEvent(F("启动时间同步成功"));
        success = true;
        break;
      }
      Serial.printf("启动时间同步失败 (%s)\n", custom_ntp_servers[i]);
      delay(100);
    }
    if (!success) {
      lastValidTime = millis() / 1000;
      setTime(lastValidTime);
      timeSynced = false;
      Serial.println(F("启动时间同步失败，使用系统计数器"));
    }
    
    initWebServer();
    checkTemperature();
  } else {
    Serial.println(F("WiFi未能在启动阶段连接，将在后台持续尝试..."));
    logEvent(F("WiFi启动连接超时，进入后台重连模式"));
    lastValidTime = millis() / 1000;
    setTime(lastValidTime);
    timeSynced = false;
    
    // 即使WiFi失败也初始化Web服务，以便后续重连后可用
    initWebServer();
    // 确保传感器逻辑运行
    checkTemperature();
  }
}

void loop()
{
  ESP.wdtFeed();
  // WiFi/传感器状态灯逻辑 优先级：传感器故障 > WiFi未连 > WiFi已连
  static unsigned long lastLedToggle = 0;
  static bool ledState = false;
  if (tempSensorFailed) {
    // 传感器故障，LED快速闪烁
    if (millis() - lastLedToggle > 200) {
      ledState = !ledState;
      digitalWrite(PIN_LED, ledState ? LOW : HIGH);
      lastLedToggle = millis();
    }
  } else if (WiFi.status() != WL_CONNECTED) {
    // WiFi未连，LED慢速闪烁
    if (millis() - lastLedToggle > 500) {
      ledState = !ledState;
      digitalWrite(PIN_LED, ledState ? LOW : HIGH);
      lastLedToggle = millis();
    }
  } else {
    // 正常，常亮
    digitalWrite(PIN_LED, LOW);
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    server.handleClient();
  }
  checkWiFiStatus();

  if (millis() - lastLogFlush >= logFlushInterval)
  {
    flushLogBuffer();
  }

  if (millis() - lastYieldTime >= 10)
  {
    yield();
    lastYieldTime = millis();
  }

  // 定期更新时间
  updateSystemTime();

  // --- DS18B20非阻塞温度采集与温控 (优化版) ---
  if (!tempConversionStarted) {
    oneWire.reset();
    sensors.requestTemperatures();
    tempConversionStarted = true;
    tempRequestTime = millis();
  } else if (sensors.isConversionComplete()) {
    lastValidTemp = readValidTemperature();
    tempConversionStarted = false;
    tempRequestTime = 0;
    // 读取到新温度后执行温控
    checkTemperature();
  } else if (millis() - tempRequestTime >= 300) {
    Serial.println(F("温度转换超时，重置并重试"));
    oneWire.reset();
    tempConversionStarted = false;
    tempRequestTime = 0;
  }
}
