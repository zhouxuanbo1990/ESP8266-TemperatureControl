#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <FS.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

// 硬件配置
#define RELAY_PIN 5
#define logFile "/log.txt"
#define EEPROM_LOG_ENABLED_ADDR 0  // 日志开关状态存储地址

// 网络配置
const char *ssid = "****";
const char *password = "******";
const char *mqtt_server = "*****";
const int mqtt_port = ****;
const char *mqtt_user = "****";
const char *mqtt_password = "****";
const char *ntp_server = "****";

// MQTT 主题
const char *deviceA_status_topic = "deviceA/status";
const char *deviceB_status_topic = "deviceB/status";

// 全局变量
WiFiClient espClient;
PubSubClient client(espClient);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntp_server, 28800);
ESP8266WebServer server(80);

bool relayState = false;
bool deviceAOnline = false;
bool deviceARelayState = false;
bool loggingEnabled = true;  // 默认开启日志
unsigned long lastReconnectAttempt = 0;
unsigned long lastWiFiCheckTime = 0;
unsigned long lastWiFiRetryTime = 0;
unsigned long lastUpdate = 0;
unsigned long lastDeviceAStatusTime = 0;
unsigned long lastMqttCheckTime = 0;
unsigned long lastStatusUpdate = 0;
unsigned long lastLogFlush = 0;
unsigned long lastYieldTime = 0;        // 新增：控制 yield() 频率
unsigned long lastDeviceCheckTime = 0;  // 新增：设备 A 检查时间
unsigned long bootTime = 0;             // 启动时间（毫秒）
time_t lastValidTime = 0;               // 上次有效时间
bool lastMqttConnectedState = false;
bool lastDeviceAOnlineState = false;
bool serverStarted = false;                       // Web服务器启动状态标记
bool timeSynced = false;                          // 时间是否同步
const unsigned long reconnectInterval = 15000;    // 与设备 A 一致
const unsigned long wifiCheckInterval = 120000;   // 调整为与设备 A 一致（2 分钟）
const unsigned long wifiRetryInterval = 180000;   // 与设备 A 一致
const unsigned long mqttCheckInterval = 10000;    // 调整为与设备 A 一致（10 秒）
const unsigned long statusUpdateInterval = 2000;  // 与设备 A 一致
const unsigned long logFlushInterval = 30000;     // 与设备 A 一致
const unsigned long deviceCheckInterval = 60000;  // 新增：与设备 A 一致
String logBuffer = "";

// WiFi 非阻塞重连相关变量
unsigned long lastWiFiAttemptTime = 0;
const unsigned long wifiAttemptInterval = 5000;
int wifiConnectionAttempts = 0;
const int maxWiFiConnectionAttempts = 10;
bool wifiInitialConnectionDone = false;

// MQTT 非阻塞重连相关变量
unsigned long lastMqttAttemptTime = 0;
const unsigned long mqttAttemptInterval = 5000;
int mqttConnectionAttempts = 0;
const int maxMqttConnectionAttempts = 5;
bool mqttInitialConnectionDone = false;

// 函数声明
void pinSetup();
void connectWiFi();
void tryConnectWiFi(bool forceAttempt = false);
void setupMQTT();
void callback(char *topic, byte *payload, unsigned int length);
void reconnect();
void tryConnectMQTT(bool forceAttempt = false);
void setRelay(bool state);
void checkDeviceAStatus();
void updateSystemTime();
String formatTime(time_t t);
void checkWiFiStatus();
void checkMqttStatus();
void initLog();
void logEvent(String message);
void flushLogBuffer();
void initWebServer();
void saveLogConfig();

// 日志初始化
void initLog() {
  if (!SPIFFS.begin()) {
    Serial.println(F("[SPIFFS初始化失败"));
    logEvent(F("SPIFFS初始化失败"));
    return;
  }
  Serial.println(F("[SPIFFS初始化成功"));

  // 初始化 EEPROM 并加载日志开关状态
  EEPROM.begin(4);
  byte storedValue = EEPROM.read(EEPROM_LOG_ENABLED_ADDR);
  if (storedValue == 0xFF) {  // EEPROM 未初始化
    loggingEnabled = true;
    EEPROM.write(EEPROM_LOG_ENABLED_ADDR, 1);
    EEPROM.commit();
    Serial.println(F("EEPROM 未初始化"));
  } else {
    loggingEnabled = (storedValue == 1);
    Serial.println(F("EEPROM 加载成功"));
  }

  Serial.println(F("系统启动"));
}

// 记录日志
void logEvent(String message) {
  String timestamp = formatTime(now()) + " " + message;
  Serial.println(timestamp);
  if (loggingEnabled) {
    logBuffer += timestamp + "\n";
    if (logBuffer.length() > 1024 || millis() - lastLogFlush >= logFlushInterval)
      flushLogBuffer();
  }
}

// 刷新日志缓冲区
void flushLogBuffer() {
  if (logBuffer.length() == 0 || !loggingEnabled)
    return;
  File file = SPIFFS.open(logFile, "a");
  if (!file) {
    Serial.println(F("无法打开日志文件"));
    logEvent(F("无法打开日志文件"));
    return;
  }
  file.print(logBuffer);
  yield();  // 在 SPIFFS 写入后让步
  file.close();
  Serial.println(F("日志写入成功"));
  logBuffer = "";
  if (file.size() > 20000) {
    SPIFFS.remove(logFile);
    Serial.println(F("日志文件已清空，重新开始记录"));
    logEvent(F("日志文件已清空，重新开始记录"));
  }
}

// 保存日志开关状态
void saveLogConfig() {
  EEPROM.write(EEPROM_LOG_ENABLED_ADDR, loggingEnabled ? 1 : 0);
  EEPROM.commit();
  Serial.println(F("日志开关状态已保存: ") + String(loggingEnabled ? "开启" : "关闭"));
  logEvent(F("日志开关状态已保存: ") + String(loggingEnabled ? "开启" : "关闭"));
}

// Web 服务器初始化
void initWebServer() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-cache");
    String html = R"rawliteral(
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>设备B控制器</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 0; padding: 20px; color: #333; background: #f5f5f5; }
    .container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; box-shadow: 0 0 5px rgba(0,0,0,0.1); }
    h2 { color: #444; border-bottom: 1px solid #eee; padding-bottom: 10px; }
    .status-item { margin: 10px 0; display: flex; align-items: center; }
    .status-label { display: inline-block; width: 150px; font-weight: bold; }
    .btn { padding: 8px 15px; margin: 5px 0; font-size: 14px; color: white; background: #4CAF50; border: none; border-radius: 3px; cursor: pointer; display: inline-block; }
    .btn:hover { background: #45a049; }
    .state { padding: 5px 10px; border-radius: 3px; font-weight: bold; }
    .state-on { background: #dff0d8; color: #3c763d; }
    .state-off { background: #f2dede; color: #a94442; }
    .device-a-online { color: #28a745; font-weight: bold; }
    .device-a-offline { color: #dc3545; font-weight: bold; }
    .status-item > * { margin-right: 10px; }
  </style>
  <script>
    function updateData() {
      fetch('/data').then(r => r.json()).then(data => {
        document.getElementById('state').textContent = data.state;
        document.getElementById('state').className = 'state ' + (data.state === 'ON' ? 'state-on' : 'state-off');
        document.getElementById('device-a-status').textContent = data.deviceAStatus;
        document.getElementById('device-a-status').className = data.deviceAStatus === '在线' ? 'device-a-online' : 'device-a-offline';
        document.getElementById('time').textContent = data.time;
        document.getElementById('log-checkbox').checked = data.loggingEnabled;
      }).catch(error => console.error('更新状态失败:', error));
    }
    function saveLogging() {
      const enabled = document.getElementById('log-checkbox').checked;
      fetch('/set_log', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ enabled: enabled })
      })
      .then(r => r.json().then(data => ({ status: r.status, body: data })))
      .then(({ status, body }) => {
        if (status === 200) {
          alert('保存成功');
        } else {
          alert('保存失败: ' + body.error);
        }
      })
      .catch(error => alert('保存失败: 网络错误'));
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
    setInterval(updateData, 2000);
    updateData();
  </script>
</head>
<body>
  <div class="container">
    <h2>系统状态</h2>
    <div class="status-item"><span class="status-label">继电器状态:</span><span id="state">-</span></div>
    <div class="status-item"><span class="status-label">设备A状态:</span><span id="device-a-status">-</span></div>
    <div class="status-item"><span class="status-label">当前时间:</span><span id="time">-</span></div>
    <h2>日志管理</h2>
    <div class="status-item">
      <span class="status-label">日志记录:</span>
      <input type="checkbox" id="log-checkbox">
    </div>
    <div class="status-item">
      <button class="btn" onclick="saveLogging()">保存</button>
    </div>
    <div class="status-item">
      <button class="btn" onclick="downloadLog()">下载日志</button>
      <button class="btn" onclick="clearLog()">清除日志</button>
    </div>
  </div>
</body>
</html>
    )rawliteral";

    server.send(200, "text/html", html);
  });

  server.on("/data", HTTP_GET, []() {
    char webTimeBuffer[10];
    snprintf(webTimeBuffer, sizeof(webTimeBuffer), "%02d:%02d:%02d",
             hour(now()), minute(now()), second(now()));
    String json = "{";
    json += "\"state\":\"" + String(relayState ? "ON" : "OFF") + "\",";
    json += "\"deviceAStatus\":\"" + String(deviceAOnline ? "在线" : "离线") + "\",";
    json += "\"time\":\"" + String(webTimeBuffer) + "\",";
    json += "\"loggingEnabled\":" + String(loggingEnabled ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/set_log", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      String json = "{\"error\":\"缺少请求体\"}";
      server.send(400, "application/json", json);
      Serial.println(F("收到POST /set_log 请求，缺少请求体"));
      logEvent(F("收到POST /set_log 请求，缺少请求体"));
      return;
    }

    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, body);
    if (error || !doc.containsKey("enabled")) {
      String json = "{\"error\":\"无效请求体\"}";
      server.send(400, "application/json", json);
      Serial.println(F("收到无效的POST /set_log 请求"));
      logEvent(F("收到无效的POST /set_log 请求"));
      return;
    }

    loggingEnabled = doc["enabled"].as<bool>();
    saveLogConfig();
    byte storedValue = EEPROM.read(EEPROM_LOG_ENABLED_ADDR);
    if (storedValue == (loggingEnabled ? 1 : 0)) {
      String json = "{\"message\":\"日志开关已设置\"}";
      server.send(200, "application/json", json);
      Serial.println(F("EEPROM 写入成功"));
      logEvent(F("EEPROM 写入成功"));
    } else {
      String json = "{\"error\":\"EEPROM 写入失败\"}";
      server.send(500, "application/json", json);
      Serial.println(F("EEPROM 写入失败"));
      logEvent(F("EEPROM 写入失败"));
    }
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
    logEvent(F("收到GET /download_log 请求并响应"));
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
    logEvent(F("收到POST /clear_log 请求"));
  });

  server.onNotFound([]() {
    Serial.println(F("收到未知请求"));
    logEvent(F("收到未知请求"));
    server.send(404, "text/plain", "Not Found");
  });

  server.begin();
  serverStarted = true;
  Serial.println(F("Web服务器已启动"));
  logEvent(F("Web服务器已启动"));
}

// 硬件初始化
void pinSetup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
}

// WiFi 连接
void tryConnectWiFi(bool forceAttempt) {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectionAttempts = 0;
    if (!wifiInitialConnectionDone) {
      String logMessage = F("WiFi首次连接成功，IP: ") + WiFi.localIP().toString();
      Serial.print(F("WiFi首次连接成功，IP: "));
      Serial.println(WiFi.localIP());
      Serial.print(F("设备名称: DeviceB-"));
      Serial.println(ESP.getChipId());
      logMessage += F("，设备名称: DeviceB-") + String(ESP.getChipId());
      logEvent(logMessage);
      wifiInitialConnectionDone = true;
    }
    return;
  }

  if (!forceAttempt && millis() - lastWiFiAttemptTime < wifiAttemptInterval) {
    return;
  }
  lastWiFiAttemptTime = millis();

  if (wifiConnectionAttempts >= maxWiFiConnectionAttempts) {
    if (millis() - lastWiFiAttemptTime < wifiRetryInterval) {
      return;
    }
    Serial.println(F("WiFi连接尝试已达最大次数，将等待较长时间后重试"));
    logEvent(F("WiFi连接尝试已达最大次数，将等待较长时间后重试"));
    wifiConnectionAttempts = 0;
  }

  Serial.print(F("尝试连接WiFi (第 "));
  Serial.print(wifiConnectionAttempts + 1);
  Serial.println(F(" 次)..."));
  logEvent(F("尝试连接WiFi (第 ") + String(wifiConnectionAttempts + 1) + F(" 次)..."));

  WiFi.begin(ssid, password);
  wifiConnectionAttempts++;
}

void connectWiFi() {
  Serial.println(F("等待WiFi连接..."));
  WiFi.begin(ssid, password);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 30000) {
    delay(100);
    ESP.wdtFeed();
  }
  if (WiFi.status() == WL_CONNECTED) {
    String wifiMsg = String(F("WiFi首次连接成功，IP: ")) + WiFi.localIP().toString() + F("，设备名称: DeviceB-") + String(ESP.getChipId());
    Serial.println(wifiMsg);
    logEvent(wifiMsg);
    wifiInitialConnectionDone = true;
  } else {
    Serial.println(F("WiFi连接失败，系统将重启"));
    logEvent(F("WiFi连接失败，系统将重启"));
    delay(2000);
    ESP.restart();
  }
}

// MQTT 初始化
void setupMQTT() {
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  Serial.println(F("MQTT服务器配置完成"));
  logEvent(F("MQTT服务器配置完成"));
}

// MQTT 回调
void callback(char *topic, byte *payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  if (strcmp(topic, deviceA_status_topic) == 0) {
    if (message == "ON" || message == "OFF") {
      deviceAOnline = true;
      lastDeviceAStatusTime = millis();
      deviceARelayState = (message == "ON");
      Serial.print(F("设备A状态更新: "));
      Serial.println(message);
      if (deviceAOnline != lastDeviceAOnlineState) {
        lastDeviceAOnlineState = deviceAOnline;
        Serial.println(F("设备A上线"));
        logEvent(F("设备A上线"));
      }
    }
  }
}

// MQTT 重连
void tryConnectMQTT(bool forceAttempt) {
  if (WiFi.status() != WL_CONNECTED) {
    if (mqttConnectionAttempts > 0) {
      Serial.println(F("WiFi未连接，MQTT连接尝试暂停"));
      logEvent(F("WiFi未连接，MQTT连接尝试暂停"));
    }
    mqttConnectionAttempts = 0;
    lastMqttAttemptTime = 0;
    return;
  }

  if (client.connected()) {
    mqttConnectionAttempts = 0;
    if (!mqttInitialConnectionDone) {
      Serial.println(F("MQTT首次连接成功"));
      logEvent(F("MQTT首次连接成功"));
      mqttInitialConnectionDone = true;
    }
    return;
  }

  if (!forceAttempt && millis() - lastMqttAttemptTime < mqttAttemptInterval) {
    return;
  }
  lastMqttAttemptTime = millis();

  if (mqttConnectionAttempts >= maxMqttConnectionAttempts) {
    if (millis() - lastMqttAttemptTime < reconnectInterval) {
      return;
    }
    Serial.println(F("MQTT连接尝试已达最大次数，将等待较长时间后重试"));
    logEvent(F("MQTT连接尝试已达最大次数，将等待较长时间后重试"));
    mqttConnectionAttempts = 0;
  }

  Serial.print(F("尝试连接MQTT (第 "));
  Serial.print(mqttConnectionAttempts + 1);
  Serial.println(F(" 次)..."));
  logEvent(F("尝试连接MQTT (第 ") + String(mqttConnectionAttempts + 1) + F(" 次)..."));

  String clientId = "DeviceB-" + String(ESP.getChipId());
  if (client.connect(clientId.c_str(), mqtt_user, mqtt_password, deviceB_status_topic, 0, true, "OFF")) {
    if (!mqttInitialConnectionDone) {
      Serial.println(F("MQTT首次连接成功"));
      logEvent(F("MQTT首次连接成功"));
      mqttInitialConnectionDone = true;
    } else {
      Serial.println(F("MQTT重连成功"));
      logEvent(F("MQTT重连成功"));
    }
    client.subscribe(deviceA_status_topic);
    client.publish(deviceB_status_topic, relayState ? "ON" : "OFF", true);
    Serial.println(F("订阅A状态并发布B状态"));
    logEvent(F("订阅A状态并发布B状态"));
    mqttConnectionAttempts = 0;
  } else {
    Serial.print(F("MQTT连接失败，状态: "));
    Serial.println(client.state());
    logEvent(F("MQTT连接失败，状态: ") + String(client.state()));
    mqttConnectionAttempts++;
  }
}

void reconnect() {
  tryConnectMQTT(true);
}

// 设置继电器
void setRelay(bool state) {
  if (relayState != state) {
    digitalWrite(RELAY_PIN, state);
    relayState = state;
    delay(50);
    if (client.connected()) {
      client.publish(deviceB_status_topic, state ? "ON" : "OFF");
    }
    Serial.printf("继电器状态已设置为: %s\n", state ? "ON" : "OFF");
    logEvent(F("继电器状态已设置为: ") + String(state ? "ON" : "OFF"));
  }
}

// 检查设备 A 状态
void checkDeviceAStatus() {
  static unsigned long lastDeviceACheck = 0;
  const unsigned long deviceACheckInterval = 100;
  if (millis() - lastDeviceACheck >= deviceACheckInterval) {
    unsigned long timeout = relayState ? 3500 : 30000;
    if (millis() - lastDeviceAStatusTime >= timeout && deviceAOnline) {
      deviceAOnline = false;
      lastDeviceAOnlineState = deviceAOnline;
      Serial.println(F("设备A掉线"));
      logEvent(F("设备A掉线"));
    }
    lastDeviceACheck = millis();
  }
}

// 更新系统时间
void updateSystemTime() {
  if (millis() - lastUpdate >= 300000) {
    if (WiFi.status() == WL_CONNECTED) {
      if (timeClient.update()) {
        lastValidTime = timeClient.getEpochTime();
        setTime(lastValidTime);
        timeSynced = true;
        Serial.println("时间同步成功: " + formatTime(now()));
        logEvent(F("时间同步成功"));
      } else {
        Serial.println(F("时间同步失败"));
        logEvent(F("时间同步失败"));
        if (lastValidTime == 0) {
          lastValidTime = (millis() - bootTime) / 1000;
        } else {
          lastValidTime += (millis() - lastUpdate) / 1000;
        }
        setTime(lastValidTime);
        timeSynced = false;
        Serial.println("使用计数器时间: " + formatTime(now()));
        logEvent(F("使用计数器时间"));
      }
    } else {
      Serial.println(F("WiFi未连接，跳过时间同步"));
      logEvent(F("WiFi未连接，跳过时间同步"));
      if (lastValidTime == 0) {
        lastValidTime = (millis() - bootTime) / 1000;
      } else {
        lastValidTime += (millis() - lastUpdate) / 1000;
      }
      setTime(lastValidTime);
      timeSynced = false;
      Serial.println("使用计数器时间: " + formatTime(now()));
      logEvent(F("使用计数器时间"));
    }
    lastUpdate = millis();
  }
}

// 格式化时间
String formatTime(time_t t) {
  char buffer[30];
  snprintf(buffer, sizeof(buffer), "[%04d-%02d-%02d %02d:%02d:%02d]",
           year(t), month(t), day(t), hour(t), minute(t), second(t));
  return String(buffer);
}

// 检查 WiFi 状态
void checkWiFiStatus() {
  if (millis() - lastWiFiCheckTime < wifiCheckInterval) {
    return;
  }
  tryConnectWiFi();
  lastWiFiCheckTime = millis();
}

// 检查 MQTT 状态
void checkMqttStatus() {
  if (millis() - lastMqttCheckTime < mqttCheckInterval) {
    return;
  }
  tryConnectMQTT();
  lastMqttCheckTime = millis();
}

// 初始化
void setup() {
  ESP.wdtEnable(8000);
  Serial.begin(115200);
  delay(2000);
  bootTime = millis();
  pinSetup();
  initLog();

  connectWiFi();

  timeClient.begin();
  if (timeClient.update()) {
    lastValidTime = timeClient.getEpochTime();
    setTime(lastValidTime);
    timeSynced = true;
    Serial.println(F("初始时间同步成功: ") + formatTime(now()));
    logEvent(F("初始时间同步成功: ") + formatTime(now()));
  } else {
    Serial.println(F("初始时间同步失败"));
    logEvent(F("初始时间同步失败"));
    lastValidTime = (millis() - bootTime) / 1000;
    setTime(lastValidTime);
    timeSynced = false;
    Serial.println(F("使用计数器时间: ") + formatTime(now()));
    logEvent(F("使用计数器时间"));
  }
  lastUpdate = millis();

  initWebServer();
  setupMQTT();
  tryConnectMQTT(true);
}

// 主循环
void loop() {
  ESP.wdtFeed();

  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
  }

  checkWiFiStatus();
  checkMqttStatus();
  checkDeviceAStatus();

  bool currentMqttConnected = client.connected();
  if (currentMqttConnected != lastMqttConnectedState) {
    Serial.println(currentMqttConnected ? F("MQTT连接恢复") : F("MQTT未连接，跳过client.loop()"));
    logEvent(currentMqttConnected ? F("MQTT连接恢复") : F("MQTT断开连接"));
    lastMqttConnectedState = currentMqttConnected;
  }

  if (currentMqttConnected) {
    client.loop();
  }

  updateSystemTime();

  // 继电器检查（与设备 A 的 checkTemperature() 频率对齐）
  if (!deviceAOnline) {
    setRelay(false);
  } else {
    setRelay(deviceARelayState);
  }

  if (millis() - lastStatusUpdate >= statusUpdateInterval) {
    if (client.connected()) {
      client.publish(deviceB_status_topic, relayState ? "ON" : "OFF");
    }
    lastStatusUpdate = millis();
  }

  if (!relayState && millis() - lastDeviceCheckTime >= deviceCheckInterval) {
    // 与设备 A 的 isDeviceBOnline() 类似，检查设备 A 状态
    if (millis() - lastDeviceAStatusTime >= 30000) {
      deviceAOnline = false;
      lastDeviceAOnlineState = deviceAOnline;
      Serial.println(F("设备A掉线（周期检查）"));
      logEvent(F("设备A掉线（周期检查）"));
    }
    lastDeviceCheckTime = millis();
  }

  if (millis() - lastLogFlush >= logFlushInterval) {
    flushLogBuffer();
  }

  if (millis() - lastYieldTime >= 10) {  // 每 10ms yield 一次
    yield();
    lastYieldTime = millis();
  }
}
