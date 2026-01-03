#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <TimeLib.h>
#include <FS.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <type_traits>

// 方法探测工具
template<typename HelperT>
struct KeepAliveHelper {
  template<typename ClientT>
  static auto call(ClientT& c, uint16_t s, int) -> decltype(c.setKeepAlive(s), void()) { c.setKeepAlive(s); }
  template<typename ClientT>
  static void call(ClientT&, uint16_t, long) {}
};
template<typename FuncT>
void setKeepAliveIfSupported(FuncT &c, uint16_t seconds) { KeepAliveHelper<FuncT>::call(c, seconds, 0); }

template<typename HelperT>
struct SocketTimeoutHelper {
  template<typename ClientT>
  static auto call(ClientT& c, uint16_t s, int) -> decltype(c.setSocketTimeout(s), void()) { c.setSocketTimeout(s); }
  template<typename ClientT>
  static void call(ClientT&, uint16_t, long) {}
};
template<typename FuncT>
void setSocketTimeoutIfSupported(FuncT &c, uint16_t seconds) { SocketTimeoutHelper<FuncT>::call(c, seconds, 0); }

#define EEPROM_PARAMS_ADDR 128

// 硬件配置
#define RELAY_PIN 5
#define PIN_LED 16
#define logFile "/log.txt"
#define maxLogSize 20000
#define maxBufferSize 1024

// 网络配置
const char *ssid = "Zhou";
const char *password = "18601982023";
const char *mqtt_server = "192.168.2.1";
const int mqtt_port = 1883;
const char *mqtt_user = "admin";
const char *mqtt_password = "public";

// 静态 IP 配置
const IPAddress local_ip(192, 168, 2, 4);
const IPAddress gateway_ip(192, 168, 2, 1);
const IPAddress subnet_mask(255, 255, 255, 0);
const IPAddress dns_ip(192, 168, 2, 1);
const char *ntp_server = "192.168.2.1";

// MQTT 主题
const char *deviceA_status_topic = "deviceA/status";
const char *deviceB_status_topic = "deviceB/status";

// 系统参数
struct SystemParams {
  uint8_t marker = 0xA5;
  uint8_t version = 1;
  bool relayState = false;
  bool enableLogging = true;
  uint32_t crc = 0;
};
SystemParams params;

// 全局变量
WiFiClient espClient;
PubSubClient client(espClient);
ESP8266WebServer server(80);

unsigned long lastUpdate = 0;
unsigned long lastPeerStatusTime = 0; // 统一命名，原 lastDeviceAStatusTime
unsigned long lastMqttCheckTime = 0;
unsigned long lastWiFiCheckTime = 0;
unsigned long lastStatusUpdate = 0;
unsigned long lastLogFlush = 0;
unsigned long lastYieldTime = 0;
unsigned long lastDeviceCheckTime = 0;
bool lastMqttConnectedState = false;
bool lastPeerOnlineState = false; // 统一命名，原 lastDeviceAOnlineState
bool serverStarted = false;
bool timeSynced = false;
String logBuffer = "";
time_t lastValidTime = 0;
const unsigned long reconnectInterval = 15000;
const unsigned long wifiCheckInterval = 120000;
const unsigned long wifiRetryInterval = 180000;
const unsigned long mqttCheckInterval = 10000;
const unsigned long statusUpdateInterval = 2000;
const unsigned long logFlushInterval = 30000;
const unsigned long deviceCheckInterval = 60000;
unsigned long lastWiFiAttemptTime = 0;
const unsigned long wifiAttemptInterval = 5000;
int wifiConnectionAttempts = 0;
const int maxWiFiConnectionAttempts = 10;
bool wifiInitialConnectionDone = false;
unsigned long lastMqttAttemptTime = 0;
const unsigned long mqttAttemptInterval = 5000;
int mqttConnectionAttempts = 0;
const int maxMqttConnectionAttempts = 5;
bool mqttInitialConnectionDone = false;
bool peerOnline = false;
bool peerRelayState = false; 

// CRC32 计算
uint32_t calcCrc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1)
        crc = (crc >> 1) ^ 0xEDB88320;
      else
        crc >>= 1;
    }
  }
  return crc ^ 0xFFFFFFFF;
}

// HTML 内容
static const char html_part1[] PROGMEM = R"rawliteral(
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
    // 标记本地是否有未保存的checkbox变更
    window.loggingDirty = false;
    function updateData() {
      fetch('/data').then(r => r.json()).then(data => {
        document.getElementById('state').textContent = data.state;
        document.getElementById('state').className = 'state ' + (data.state === 'ON' ? 'state-on' : 'state-off');
        document.getElementById('device-a-status').textContent = data.deviceAStatus;
        document.getElementById('device-a-status').className = data.deviceAStatus === '在线' ? 'device-a-online' : 'device-a-offline';
        document.getElementById('time').textContent = data.time;
        // 只有未本地操作过checkbox时才自动刷新
        if (!window.loggingDirty) {
          document.getElementById('log-checkbox').checked = data.loggingEnabled;
        }
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
          window.loggingDirty = false; // 保存后允许自动刷新
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
    // 监听checkbox本地变更
    window.addEventListener('DOMContentLoaded', function() {
      var logCheckbox = document.getElementById('log-checkbox');
      logCheckbox.addEventListener('change', function() {
        window.loggingDirty = true;
      });
    });
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
)rawliteral";

static const char html_part2[] PROGMEM = R"rawliteral(
  </div>
</body>
</html>
)rawliteral";

// 函数声明
void pinSetup();
void tryConnectWiFi(bool forceAttempt = false);
void setupMQTT();
void callback(char *topic, byte *payload, unsigned int length);
void tryConnectMQTT(bool forceAttempt = false);
void setRelay(bool state);
void checkPeerStatus();
void updateSystemTime();
String formatUptime();
void checkWiFiStatus();
void checkMqttStatus();
void initLog();
void logEvent(String message);
void flushLogBuffer();
void initWebServer();

// 日志初始化
void initLog() {
  if (!SPIFFS.begin()) {
    Serial.println(F("SPIFFS初始化失败"));
    return;
  }
  Serial.println(F("SPIFFS初始化成功"));
  File file = SPIFFS.open(logFile, "a");
  if (!file) {
    Serial.println(F("无法打开日志文件，创建新文件"));
    file = SPIFFS.open(logFile, "w");
    if (file) {
      Serial.println(F("日志文件已创建"));
    } else {
      Serial.println(F("日志文件创建失败"));
    }
  }
  file.close();
}

// EEPROM初始化
void saveParamsToEEPROM() {
  params.marker = 0xA5;
  params.version = 1;
  params.crc = calcCrc32((const uint8_t *)&params, sizeof(SystemParams) - sizeof(uint32_t));
  EEPROM.begin(sizeof(SystemParams) + EEPROM_PARAMS_ADDR);
  uint8_t *ptr = (uint8_t *)&params;
  for (size_t i = 0; i < sizeof(SystemParams); i++) {
    EEPROM.write(EEPROM_PARAMS_ADDR + i, ptr[i]);
  }
  EEPROM.commit();
}

void loadParamsFromEEPROM() {
  EEPROM.begin(sizeof(SystemParams) + EEPROM_PARAMS_ADDR);
  uint8_t *ptr = (uint8_t *)&params;
  for (size_t i = 0; i < sizeof(SystemParams); i++) {
    ptr[i] = EEPROM.read(EEPROM_PARAMS_ADDR + i);
  }
  if (params.marker != 0xA5 || params.version != 1) {
    params = SystemParams();
    Serial.println(F("EEPROM 加载失败，使用默认参数"));
    saveParamsToEEPROM();
  } else {
    uint32_t crc = params.crc;
    params.crc = 0;
    uint32_t calcCrc = calcCrc32((const uint8_t *)&params, sizeof(SystemParams) - sizeof(uint32_t));
    if (crc == calcCrc) {
      Serial.println(F("EEPROM 加载成功"));
    } else {
      params = SystemParams();
      Serial.println(F("EEPROM 加载失败，使用默认参数"));
      saveParamsToEEPROM();
    }
  }
}

// 记录日志
void logEvent(String message) {
  String timestamp = formatUptime();
  String logLine = timestamp + " " + message;
  if (params.enableLogging) {
    logBuffer += logLine + "\n";
    if (logBuffer.length() > maxBufferSize || millis() - lastLogFlush >= logFlushInterval)
      flushLogBuffer();
  } else {
    Serial.println(logLine); // 仅在未启用logEvent时输出到串口
  }
}

// 刷新日志缓冲区
void flushLogBuffer() {
  if (logBuffer.length() == 0 || !params.enableLogging)
    return;

  // 限制文件操作频率
  static unsigned long lastFileWrite = 0;
  if (millis() - lastFileWrite < 1000) // 每秒最多写入一次
    return;

  File file = SPIFFS.open(logFile, "a");
  if (!file) {
    Serial.println(F("无法打开日志文件"));
    logEvent(F("无法打开日志文件")); // 避免递归调用
    return;
  }

  file.print(logBuffer);
  file.close(); // 立即关闭文件
  lastFileWrite = millis();

  if (file.size() > maxLogSize) {
    SPIFFS.remove(logFile);
    Serial.println(F("日志文件已清空，重新开始记录"));
    logEvent(F("日志文件已清空，重新开始记录"));
  }

  logBuffer = ""; // 清空缓冲区
}

// Web 服务器初始化
void initWebServer() {
  server.on("/", HTTP_GET, []() {
    String html = String(FPSTR(html_part1));
    html += String(FPSTR(html_part2));
    server.sendHeader("Cache-Control", "no-cache");
    server.send(200, "text/html; charset=utf-8", html);
  });

  server.on("/data", HTTP_GET, []() {
    String uptimeStr = formatUptime();
    String json = "{";
    json += "\"state\":\"" + String(params.relayState ? "ON" : "OFF") + "\",";
    json += "\"deviceAStatus\":\"" + String(peerOnline ? "在线" : "离线") + "\",";
    json += "\"time\":\"" + uptimeStr + "\",";
    json += "\"loggingEnabled\":" + String(params.enableLogging ? "true" : "false");
    json += "}";
    server.send(200, "application/json; charset=utf-8", json);
  });

  server.on("/set_log", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      String json = "{\"error\":\"缺少请求体\"}";
      server.send(400, "application/json; charset=utf-8", json);
      Serial.println(F("收到POST /set_log 请求，缺少请求体"));
      return;
    }
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, body);
    if (error || !doc.containsKey("enabled")) {
      String json = "{\"error\":\"无效请求体\"}";
      server.send(400, "application/json; charset=utf-8", json);
      Serial.println(F("收到无效的POST /set_log 请求"));
      return;
    }
    params.enableLogging = doc["enabled"].as<bool>();
    saveParamsToEEPROM();
    String json = "{\"message\":\"日志开关已设置\"}";
    server.send(200, "application/json; charset=utf-8", json);
    Serial.println(F("EEPROM 写入成功"));
    logEvent(F("EEPROM 写入成功"));
  });

  server.on("/download_log", HTTP_GET, []() {
    flushLogBuffer();
    File file = SPIFFS.open(logFile, "r");
    if (!file) {
      Serial.println(F("无法打开日志文件"));
      logEvent(F("无法打开日志文件"));
      server.send(500, "text/plain; charset=utf-8", "无法打开日志文件");
      return;
    }
    if (file.size() == 0) {
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
  });

  server.on("/clear_log", HTTP_POST, []() {
    flushLogBuffer();
    if (SPIFFS.remove(logFile)) {
      Serial.println(F("日志文件已清除"));
      logEvent(F("日志文件已清除"));
      server.send(200, "text/plain; charset=utf-8", "日志已清除");
    } else {
      Serial.println(F("清除日志文件失败"));
      logEvent(F("清除日志文件失败"));
      server.send(500, "text/plain; charset=utf-8", "清除日志文件失败");
    }
  });

  server.onNotFound([]() {
    server.send(404, "text/plain; charset=utf-8", "Not Found");
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
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH); // LED默认关闭（高电平灭）
}

// WiFi 连接
void tryConnectWiFi(bool forceAttempt) {
  if (WiFi.status() == WL_CONNECTED) return;

  if (!forceAttempt && (millis() - lastWiFiAttemptTime < wifiAttemptInterval)) return;
  lastWiFiAttemptTime = millis();

  WiFi.begin(ssid, password);
}

// MQTT 初始化
void setupMQTT() {
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  // 保活设置
  setKeepAliveIfSupported(client, 15);
  setSocketTimeoutIfSupported(client, 5);
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
      peerOnline = true;
      lastPeerStatusTime = millis(); // 统一命名
      peerRelayState = (message == "ON");
      if (peerOnline != lastPeerOnlineState) { // 统一命名
        lastPeerOnlineState = peerOnline;
        Serial.println(F("对端设备上线"));
        logEvent(F("对端设备上线"));
      }
    }
  }
}

// MQTT 非阻塞重连
void tryConnectMQTT(bool forceAttempt) {
  if (WiFi.status() != WL_CONNECTED || client.connected()) return;

  if (!forceAttempt && (millis() - lastMqttAttemptTime < mqttAttemptInterval)) return;
  lastMqttAttemptTime = millis();

  String clientId = "DeviceB-" + String(ESP.getChipId());
  if (client.connect(clientId.c_str(), mqtt_user, mqtt_password, deviceB_status_topic, 0, false, "OFF")) {
    client.subscribe(deviceA_status_topic);
    client.publish(deviceB_status_topic, params.relayState ? "ON" : "OFF");
    mqttConnectionAttempts = 0;
    lastMqttConnectedState = true; 
    Serial.println(F("MQTT连接成功"));
    logEvent(F("MQTT连接成功"));
  } else {
    mqttConnectionAttempts++;
    if (mqttConnectionAttempts >= 5) {
      Serial.println(F("MQTT失败次数过多，系统重启..."));
      logEvent(F("MQTT失败次数过多，系统重启..."));
      delay(1000);
      ESP.restart();
    }
  }
}

void setRelay(bool state) {
  if (params.relayState != state) {
    digitalWrite(RELAY_PIN, state);
    params.relayState = state;
    delay(50);
    if (client.connected()) {
      client.publish(deviceB_status_topic, state ? "ON" : "OFF");
    }
    Serial.printf("继电器状态已设置为: %s\n", state ? "ON" : "OFF");
    logEvent(F("继电器状态已设置为: ") + String(state ? "ON" : "OFF"));
  }
}

// 检查设备A状态
void checkPeerStatus() {
  static unsigned long lastDeviceACheck = 0;
  const unsigned long deviceACheckInterval = 100;
  if (millis() - lastDeviceACheck >= deviceACheckInterval) {
    unsigned long timeout = params.relayState ? 3500 : 30000;
    if (millis() - lastPeerStatusTime >= timeout && peerOnline) {
      peerOnline = false;
      lastPeerOnlineState = peerOnline;
      Serial.println(F("对端设备掉线"));
      logEvent(F("对端设备掉线"));
    }
    lastDeviceACheck = millis();
  }
}

// 更新系统时间 (仅累加)
void updateSystemTime() {
  // B 端不再同步 NTP，仅使用系统 ticks
}

// 格式化运行时长
String formatUptime() {
  unsigned long seconds = millis() / 1000;
  unsigned long days = seconds / 86400UL;
  unsigned long rem = seconds % 86400UL;
  unsigned long hours = rem / 3600UL;
  rem %= 3600UL;
  unsigned long minutes = rem / 60UL;
  unsigned long secs = rem % 60UL;

  char buffer[40];
  snprintf(buffer, sizeof(buffer), "[运行 %lud %02lu:%02lu:%02lu]", days, hours,
           minutes, secs);
  return String(buffer);
}

// 检查WiFi状态
void checkWiFiStatus() {
  if (millis() - lastWiFiCheckTime < wifiCheckInterval) {
    return;
  }
  tryConnectWiFi();
  lastWiFiCheckTime = millis();
}

// 检查MQTT状态
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
  delay(3000);
  pinSetup();
  initLog();
  loadParamsFromEEPROM();
  Serial.println(F("系统启动"));

  // 稳定配置
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.config(local_ip, gateway_ip, subnet_mask, dns_ip); // 固定 IP
  WiFi.setAutoReconnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  
  Serial.println(F("等待WiFi连接..."));
  WiFi.begin(ssid, password);
  unsigned long wifiStart = millis();
  static unsigned long ledTimer = 0;
  static bool ledState = false;
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 30000) {
    // 启动阶段WiFi未连上时LED闪烁
    if (millis() - ledTimer > 500) {
      ledTimer = millis();
      ledState = !ledState;
      digitalWrite(PIN_LED, ledState ? LOW : HIGH); // 闪烁
    }
    delay(100);
    ESP.wdtFeed();
  }
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(PIN_LED, LOW);
  } else {
    Serial.println(F("WiFi未能在启动阶段连接，将在后台持续尝试..."));
  }

  // 无论初始WiFi是否成功，都初始化以便后续自动工作
  initWebServer();
  setupMQTT();
  if (WiFi.status() == WL_CONNECTED) {
    tryConnectMQTT(true);
  }
}

// 主循环
void loop() {
  ESP.wdtFeed();
  // 16脚LED作为WiFi指示灯：连网常亮，断网闪烁，连接中常灭
  static unsigned long ledTimer = 0;
  static bool ledState = false;
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(PIN_LED, LOW); // 常亮（低电平亮）
  } else if (WiFi.SSID() && WiFi.status() == WL_DISCONNECTED) {
    // 断网时闪烁
    if (millis() - ledTimer > 500) {
      ledTimer = millis();
      ledState = !ledState;
      digitalWrite(PIN_LED, ledState ? LOW : HIGH); // 闪烁
    }
  } else {
    // 连接过程中常灭（高电平）
    digitalWrite(PIN_LED, HIGH);
  }
  static bool lastWiFiConnected = false;
  if (WiFi.status() == WL_CONNECTED) {
    if (!lastWiFiConnected) {
      Serial.print(F("WiFi已连接，IP地址: "));
      Serial.println(WiFi.localIP());
      lastWiFiConnected = true;
    }
    server.handleClient();
  } else {
    lastWiFiConnected = false;
  }
  checkWiFiStatus();
  checkMqttStatus();
  checkPeerStatus();

  bool currentMqttConnected = client.connected();
  if (currentMqttConnected != lastMqttConnectedState) {
    Serial.println(currentMqttConnected ? F("MQTT连接恢复") : F("MQTT断开连接，跳过client.loop()"));
    logEvent(currentMqttConnected ? F("MQTT连接恢复") : F("MQTT断开连接"));
    lastMqttConnectedState = currentMqttConnected;
  }
  if (currentMqttConnected) {
    client.loop();
  }
  updateSystemTime();
  if (!peerOnline) {
    setRelay(false);
  } else {
    setRelay(peerRelayState);
  }
  if (millis() - lastStatusUpdate >= statusUpdateInterval) {
    if (client.connected()) {
      client.publish(deviceB_status_topic, params.relayState ? "ON" : "OFF");
    }
    lastStatusUpdate = millis();
  }
  // 移除每60秒一次的冗余周期检查
  if (millis() - lastLogFlush >= logFlushInterval) {
    flushLogBuffer();
  }
  if (millis() - lastYieldTime >= 10) {
    yield();
    lastYieldTime = millis();
  }
}
