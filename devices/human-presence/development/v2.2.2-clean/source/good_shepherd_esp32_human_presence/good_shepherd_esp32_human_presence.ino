/*
  Good Shepherd ESP32 Human Presence Firmware
  Cleanup Workbench — Aggressive Pass 2
  Date: 2026-08-27

  Dedicated LD2410 human-presence firmware.
  Production PIR/motion firmware is intentionally outside this branch.

  Aggressive Pass 2:
    - BLE-only commissioning/service.
    - NimBLE-Arduino instead of classic ESP32 BLE/Bluedroid.
    - MQTT V2 is the normal runtime transport.
    - WebServer/SoftAP/HTML setup removed.
    - Routine HTTPS heartbeat/command polling removed.
    - HTTPS retained only for inventory registration bootstrap and OTA.
    - Existing BLE UUIDs, MQTT topics, Preferences keys, command names,
      presence event names, and OTA-by-URL behavior retained.

  Required Arduino libraries:
    - PubSubClient
    - NimBLE-Arduino (h2zero)

  Board:
    ESP32 Dev Module

  Wiring:
    LD2410 VCC -> VIN / 5V
    LD2410 GND -> GND
    LD2410 OUT -> GPIO21
*/

#include <WiFi.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Update.h>
#include <NimBLEDevice.h>
#include <esp_system.h>

static constexpr uint8_t PRESENCE_PIN = 21;
static constexpr uint8_t LED_PIN = 2;
static constexpr uint8_t PRESENCE_ACTIVE_STATE = HIGH;

static const char* SOFTWARE_VERSION =
  "esp32-good-shepherd-human-presence-v2.2.2-clean";

static const char* REGISTER_URL =
  "https://good-shepherd-server-j06f.onrender.com/nodes/register";

static const char* MQTT_HOST =
  "c3f9bcc09adc4e7db6a3d29b63a24819.s1.eu.hivemq.cloud";
static constexpr uint16_t MQTT_PORT = 8883;
static const char* MQTT_USERNAME = "good-shepherd-pilot";
static const char* MQTT_PASSWORD = "Goodshepherd1!";

static const char* FACTORY_WIFI_SSID = "PRESTIGE HOMECARE OFFICE";
static const char* FACTORY_WIFI_PASSWORD = "Delaware109%";
static const char* FACTORY_REGISTRATION_MARKER_KEY = "invRegV1";

static const char* BLE_SERVICE_UUID =
  "7d9f0001-2f4f-4c3a-8b2a-0b5f7f2a0001";
static const char* BLE_STATUS_UUID =
  "7d9f0002-2f4f-4c3a-8b2a-0b5f7f2a0001";
static const char* BLE_COMMAND_UUID =
  "7d9f0003-2f4f-4c3a-8b2a-0b5f7f2a0001";
static const char* BLE_RESULT_UUID =
  "7d9f0004-2f4f-4c3a-8b2a-0b5f7f2a0001";

static constexpr unsigned long BLE_BOOT_SETUP_WINDOW_MS = 60000UL;
static constexpr unsigned long BLE_STATUS_INTERVAL_MS = 10000UL;
static constexpr unsigned long BLE_MAX_CONNECTED_SESSION_MS = 120000UL;
static constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 10000UL;
static constexpr unsigned long MQTT_RECONNECT_INTERVAL_MS = 10000UL;
static constexpr unsigned long MQTT_STATUS_INTERVAL_MS = 60000UL;
static constexpr unsigned long MQTT_WIFI_SETTLE_MS = 2500UL;
static constexpr unsigned long FACTORY_WIFI_TIMEOUT_MS = 15000UL;
static constexpr unsigned long PRESENCE_CLEAR_DELAY_MS = 10000UL;
static constexpr unsigned long PRESENCE_EVENT_RETRY_MS = 15000UL;
static constexpr unsigned long PRESENCE_HELD_ACTIVE_REARM_MS = 300000UL;
static constexpr uint16_t MQTT_KEEPALIVE_SECONDS = 30;
static constexpr uint16_t MQTT_BUFFER_SIZE = 2048;

Preferences prefs;

String wifiName;
String wifiPassword;
String locationName;
String residentName;
String roomName;
String deviceName = "Human Presence Sensor";

String nodeId;
String sourceKey;
String setupId;

WiFiClientSecure mqttTls;
PubSubClient mqtt(mqttTls);

NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* bleStatus = nullptr;
NimBLECharacteristic* bleCommand = nullptr;
NimBLECharacteristic* bleResult = nullptr;

bool bleStarted = false;
bool bleClientConnected = false;
bool bleBootWindowActive = false;
bool bleReleasedForRuntime = false;
unsigned long bleBootWindowStartedAt = 0;
unsigned long bleConnectedAt = 0;
unsigned long lastBleStatusAt = 0;

bool pendingBleCommand = false;
String pendingBleCommandPayload;

bool pendingMqttCommand = false;
String pendingMqttCommandPayload;

bool wifiConnectInProgress = false;
unsigned long lastWifiAttemptAt = 0;
unsigned long wifiConnectedAt = 0;
unsigned long lastMqttAttemptAt = 0;
unsigned long lastStatusPublishAt = 0;

int lastPresenceState = -1;
bool presenceReported = false;
bool pendingPresenceDetected = false;
bool pendingPresenceCleared = false;
unsigned long noPresenceStartedAt = 0;
unsigned long presenceHeldStartedAt = 0;
unsigned long lastPresenceAttemptAt = 0;

String jsonEscape(String value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  value.replace("\n", "\\n");
  value.replace("\r", "");
  return value;
}

String extractJsonString(const String& json, const String& key) {
  String needle = "\"" + key + "\"";
  int keyPos = json.indexOf(needle);
  if (keyPos < 0) return "";
  int colon = json.indexOf(':', keyPos + needle.length());
  if (colon < 0) return "";
  int quote = json.indexOf('"', colon + 1);
  if (quote < 0) return "";

  String out;
  bool escaped = false;
  for (int i = quote + 1; i < (int)json.length(); ++i) {
    char c = json[i];
    if (escaped) {
      if (c == 'n') out += '\n';
      else if (c == 'r') out += '\r';
      else if (c == 't') out += '\t';
      else out += c;
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') return out;
    out += c;
  }
  return "";
}

bool extractJsonBool(const String& json, const String& key, bool fallback) {
  String needle = "\"" + key + "\"";
  int keyPos = json.indexOf(needle);
  if (keyPos < 0) return fallback;
  int colon = json.indexOf(':', keyPos + needle.length());
  if (colon < 0) return fallback;

  String tail = json.substring(colon + 1);
  tail.trim();
  if (tail.startsWith("true")) return true;
  if (tail.startsWith("false")) return false;
  return fallback;
}

String chipId() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[17];
  snprintf(buf, sizeof(buf), "%04X%08X",
           (uint16_t)(mac >> 32), (uint32_t)mac);
  String result(buf);
  result.toLowerCase();
  return result;
}

void ensureIdentity() {
  String chip = chipId();
  nodeId = "esp32-" + chip;
  sourceKey = "presence-" + chip;

  prefs.begin("gs-device", false);
  setupId = prefs.getString("setupId", "");
  if (setupId.length() == 0) {
    setupId = chip.substring(chip.length() >= 6 ? chip.length() - 6 : 0);
    setupId.toUpperCase();
    prefs.putString("setupId", setupId);
  }
  prefs.end();
}

String assignmentState() {
  return (locationName.length() && residentName.length() && roomName.length())
    ? "Assigned" : "Unassigned";
}

bool isHumanPresenceMode(const String& mode) {
  return mode == "human_presence" ||
         mode == "presence" ||
         mode == "human presence";
}

#define logLine(message) do { } while (0)

void setLed(bool on) {
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void blinkIdentify(unsigned long durationMs = 12000UL) {
  unsigned long started = millis();
  bool state = false;
  while (millis() - started < durationMs) {
    state = !state;
    setLed(state);
    delay(150);
  }
  setLed(false);
}

void loadSettings() {
  prefs.begin("gs-device", true);
  wifiName = prefs.getString("wifiName", "");
  wifiPassword = prefs.getString("wifiPass", "");
  locationName = prefs.getString("location", "");
  residentName = prefs.getString("resident", "");
  roomName = prefs.getString("room", "");
  deviceName = prefs.getString("deviceName", "Human Presence Sensor");
  prefs.end();

  if (deviceName.length() == 0) deviceName = "Human Presence Sensor";
}

void saveSettings() {
  prefs.begin("gs-device", false);
  prefs.putString("wifiName", wifiName);
  prefs.putString("wifiPass", wifiPassword);
  prefs.putString("location", locationName);
  prefs.putString("resident", residentName);
  prefs.putString("room", roomName);
  prefs.putString("deviceName", deviceName);
  prefs.putString("sensorMode", "human_presence");
  prefs.remove("nodeId");
  prefs.remove("sourceKey");
  prefs.end();
}

void clearAssignmentSettingsOnly() {
  prefs.begin("gs-device", false);
  prefs.remove("wifiName");
  prefs.remove("wifiPass");
  prefs.remove("location");
  prefs.remove("resident");
  prefs.remove("room");
  prefs.remove("deviceName");
  prefs.remove("sensorMode");
  prefs.remove("nodeId");
  prefs.remove("sourceKey");
  prefs.end();
}

void factoryClearEverything() {
  prefs.begin("gs-device", false);
  prefs.clear();
  prefs.end();
}

String statusTopic()   { return "good-shepherd/v2/nodes/" + nodeId + "/status"; }
String eventsTopic()   { return "good-shepherd/v2/nodes/" + nodeId + "/events"; }
String commandsTopic() { return "good-shepherd/v2/nodes/" + nodeId + "/commands"; }
String resultsTopic()  { return "good-shepherd/v2/nodes/" + nodeId + "/results"; }

String mqttClientId() {
  String chip = nodeId;
  chip.replace("esp32-", "");
  if (chip.length() > 12) chip = chip.substring(chip.length() - 12);
  return "gsv2-" + chip;
}

String statusPayload(bool online) {
  String p;
  p.reserve(700);
  p += "{";
  p += "\"protocolVersion\":\"2.0\",";
  p += "\"nodeId\":\"" + jsonEscape(nodeId) + "\",";
  p += "\"nodeName\":\"" + jsonEscape(deviceName + " - " + roomName) + "\",";
  p += "\"deviceName\":\"" + jsonEscape(deviceName) + "\",";
  p += "\"locationName\":\"" + jsonEscape(locationName) + "\",";
  p += "\"residentName\":\"" + jsonEscape(residentName) + "\",";
  p += "\"roomName\":\"" + jsonEscape(roomName) + "\",";
  p += "\"setupId\":\"" + jsonEscape(setupId) + "\",";
  p += "\"assignmentState\":\"" + assignmentState() + "\",";
  p += "\"softwareVersion\":\"" + String(SOFTWARE_VERSION) + "\",";
  p += "\"sensorMode\":\"human_presence\",";
  p += "\"sourceKey\":\"" + jsonEscape(sourceKey) + "\",";
  p += "\"localIp\":\"" + WiFi.localIP().toString() + "\",";
  p += "\"wifiSsid\":\"" + jsonEscape(WiFi.SSID()) + "\",";
  p += "\"wifiRssi\":" + String(WiFi.RSSI()) + ",";
  p += "\"uptimeSeconds\":" + String(millis() / 1000UL) + ",";
  p += "\"online\":" + String(online ? "true" : "false");
  p += "}";
  return p;
}

String lastWillPayload() {
  return "{\"protocolVersion\":\"2.0\",\"nodeId\":\"" +
         jsonEscape(nodeId) + "\",\"online\":false}";
}

bool publishStatus(bool online) {
  if (!mqtt.connected()) return false;
  String p = statusPayload(online);
  bool ok = mqtt.publish(statusTopic().c_str(), p.c_str(), true);
  if (ok && online) lastStatusPublishAt = millis();
  return ok;
}

bool publishCommandResult(
  const String& commandId,
  const String& commandType,
  const String& status,
  const String& message
) {
  if (!mqtt.connected()) return false;

  String p;
  p.reserve(450);
  p += "{";
  p += "\"protocolVersion\":\"2.0\",";
  p += "\"nodeId\":\"" + jsonEscape(nodeId) + "\",";
  p += "\"commandId\":\"" + jsonEscape(commandId) + "\",";
  p += "\"commandType\":\"" + jsonEscape(commandType) + "\",";
  p += "\"status\":\"" + jsonEscape(status) + "\",";
  p += "\"message\":\"" + jsonEscape(message) + "\"";
  p += "}";

  bool ok = mqtt.publish(resultsTopic().c_str(), p.c_str(), false);
  mqtt.loop();
  return ok;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (pendingMqttCommand) return;

  String body;
  body.reserve(length + 1);
  for (unsigned int i = 0; i < length; ++i) body += (char)payload[i];

  if (extractJsonString(body, "commandId").length() == 0 ||
      extractJsonString(body, "commandType").length() == 0) {
    return;
  }

  pendingMqttCommandPayload = body;
  pendingMqttCommand = true;
}

void configureMqtt() {
  static bool configured = false;
  if (configured) return;

  mqttTls.setInsecure();
  mqttTls.setTimeout(12000);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(MQTT_KEEPALIVE_SECONDS);
  mqtt.setSocketTimeout(8);
  mqtt.setBufferSize(MQTT_BUFFER_SIZE);
  mqtt.setCallback(mqttCallback);

  configured = true;
}

bool connectMqtt() {
  if (WiFi.status() != WL_CONNECTED) return false;

  if (wifiConnectedAt == 0) {
    wifiConnectedAt = millis();
    return false;
  }

  if (millis() - wifiConnectedAt < MQTT_WIFI_SETTLE_MS) return false;
  if (mqtt.connected()) return true;

  unsigned long now = millis();
  if (lastMqttAttemptAt &&
      now - lastMqttAttemptAt < MQTT_RECONNECT_INTERVAL_MS) {
    return false;
  }
  lastMqttAttemptAt = now;

  configureMqtt();

  mqttTls.stop();
  mqttTls.setInsecure();
  mqttTls.setTimeout(12000);

  String willTopic = statusTopic();
  String willPayload = lastWillPayload();

  bool ok = mqtt.connect(
    mqttClientId().c_str(),
    MQTT_USERNAME,
    MQTT_PASSWORD,
    willTopic.c_str(),
    1,
    true,
    willPayload.c_str()
  );

  if (!ok) {
    logLine("MQTT connect failed state=" + String(mqtt.state()));
    return false;
  }

  mqtt.subscribe(commandsTopic().c_str(), 1);
  publishStatus(true);
  logLine("MQTT connected.");
  return true;
}

void serviceMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqtt.connected()) {
    connectMqtt();
    return;
  }

  if (!mqtt.loop()) return;

  if (lastStatusPublishAt == 0 ||
      millis() - lastStatusPublishAt >= MQTT_STATUS_INTERVAL_MS) {
    publishStatus(true);
  }
}

bool publishPresenceEvent(bool active) {
  if (!mqtt.connected()) return false;

  String fullSourceName = deviceName + " - " + roomName;
  String eventType = active ? "presence_detected" : "presence_cleared";
  String message = active
    ? "Human presence detected from " + fullSourceName
    : "Human presence cleared from " + fullSourceName;
  String timeText = active
    ? "ESP32 Human Presence Detected Event"
    : "ESP32 Human Presence Cleared Event";

  String p;
  p.reserve(700);
  p += "{";
  p += "\"protocolVersion\":\"2.0\",";
  p += "\"nodeId\":\"" + jsonEscape(nodeId) + "\",";
  p += "\"locationName\":\"" + jsonEscape(locationName) + "\",";
  p += "\"sourceKey\":\"" + jsonEscape(sourceKey) + "\",";
  p += "\"sourceName\":\"" + jsonEscape(fullSourceName) + "\",";
  p += "\"residentName\":\"" + jsonEscape(residentName) + "\",";
  p += "\"message\":\"" + jsonEscape(message) + "\",";
  p += "\"alertLevel\":\"Normal\",";
  p += "\"timeText\":\"" + jsonEscape(timeText) + "\",";
  p += "\"sensorMode\":\"human_presence\",";
  p += "\"sensorType\":\"human_presence\",";
  p += "\"source\":\"ld2410\",";
  p += "\"eventType\":\"" + eventType + "\",";
  p += "\"presence\":" + String(active ? "true" : "false");
  p += "}";

  bool ok = mqtt.publish(eventsTopic().c_str(), p.c_str(), false);
  if (ok) mqtt.loop();
  return ok;
}

void servicePresence() {
  int presence = digitalRead(PRESENCE_PIN);

  if (presence != lastPresenceState) {
    lastPresenceState = presence;
    logLine("LD2410 OUT=" + String(presence));
  }

  if (presence == PRESENCE_ACTIVE_STATE) {
    noPresenceStartedAt = 0;
    pendingPresenceCleared = false;

    if (presenceHeldStartedAt == 0) {
      presenceHeldStartedAt = millis();
    } else if (presenceReported &&
               millis() - presenceHeldStartedAt >=
               PRESENCE_HELD_ACTIVE_REARM_MS) {
      presenceReported = false;
      presenceHeldStartedAt = millis();
    }

    if (!presenceReported) pendingPresenceDetected = true;

    if (pendingPresenceDetected &&
        (!lastPresenceAttemptAt ||
         millis() - lastPresenceAttemptAt >= PRESENCE_EVENT_RETRY_MS)) {
      lastPresenceAttemptAt = millis();
      if (publishPresenceEvent(true)) {
        pendingPresenceDetected = false;
        presenceReported = true;
      }
    }
    return;
  }

  presenceHeldStartedAt = 0;

  if (!(presenceReported || pendingPresenceDetected ||
        pendingPresenceCleared)) {
    return;
  }

  if (noPresenceStartedAt == 0) noPresenceStartedAt = millis();
  if (millis() - noPresenceStartedAt < PRESENCE_CLEAR_DELAY_MS) return;

  pendingPresenceCleared = true;

  if (pendingPresenceDetected) {
    if (!lastPresenceAttemptAt ||
        millis() - lastPresenceAttemptAt >= PRESENCE_EVENT_RETRY_MS) {
      lastPresenceAttemptAt = millis();
      if (publishPresenceEvent(true)) {
        pendingPresenceDetected = false;
        presenceReported = true;
      }
    }
    return;
  }

  if (!lastPresenceAttemptAt ||
      millis() - lastPresenceAttemptAt >= PRESENCE_EVENT_RETRY_MS) {
    lastPresenceAttemptAt = millis();
    if (publishPresenceEvent(false)) {
      pendingPresenceCleared = false;
      presenceReported = false;
      noPresenceStartedAt = 0;
    }
  }
}

String bleStatusPayload() {
  String p;
  p.reserve(850);
  p += "{";
  p += "\"setupId\":\"" + jsonEscape(setupId) + "\",";
  p += "\"nodeId\":\"" + jsonEscape(nodeId) + "\",";
  p += "\"sourceKey\":\"" + jsonEscape(sourceKey) + "\",";
  p += "\"sensorMode\":\"human_presence\",";
  p += "\"deviceType\":\"human_presence\",";
  p += "\"deviceName\":\"" + jsonEscape(deviceName) + "\",";
  p += "\"locationName\":\"" + jsonEscape(locationName) + "\",";
  p += "\"residentName\":\"" + jsonEscape(residentName) + "\",";
  p += "\"roomName\":\"" + jsonEscape(roomName) + "\",";
  p += "\"assignmentState\":\"" + assignmentState() + "\",";
  p += "\"softwareVersion\":\"" + String(SOFTWARE_VERSION) + "\",";
  p += "\"wifiConfigured\":" + String(wifiName.length() ? "true" : "false") + ",";
  p += "\"wifiConnected\":" +
       String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  p += "\"wifiSsid\":\"" +
       jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : wifiName) + "\",";
  p += "\"localIp\":\"" +
       String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "") + "\",";
  p += "\"mqttConnected\":" + String(mqtt.connected() ? "true" : "false") + ",";
  p += "\"presence\":" +
       String(digitalRead(PRESENCE_PIN) == PRESENCE_ACTIVE_STATE ? "true" : "false");
  p += "}";
  return p;
}

void updateBleStatus() {
  if (!bleStatus) return;
  String p = bleStatusPayload();
  bleStatus->setValue(p.c_str());
  if (bleClientConnected) bleStatus->notify();
  lastBleStatusAt = millis();
}

void publishBleResult(const String& status, const String& message) {
  if (!bleResult) return;
  String p = "{\"status\":\"" + jsonEscape(status) +
             "\",\"message\":\"" + jsonEscape(message) + "\"}";
  bleResult->setValue(p.c_str());
  if (bleClientConnected) bleResult->notify();
}

void startOperationalWifi() {
  if (wifiName.length() == 0 || wifiPassword.length() == 0) return;
  if (wifiConnectInProgress) return;

  wifiConnectInProgress = true;
  lastWifiAttemptAt = millis();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(wifiName.c_str(), wifiPassword.c_str());
  logLine("Starting saved Wi-Fi connection to " + wifiName);
}

void serviceWifi() {
  if (bleBootWindowActive) return;
  if (wifiName.length() == 0 || wifiPassword.length() == 0) return;

  if (WiFi.status() == WL_CONNECTED) {
    if (wifiConnectInProgress || wifiConnectedAt == 0) {
      wifiConnectInProgress = false;
      wifiConnectedAt = millis();
      logLine("Wi-Fi connected. IP=" + WiFi.localIP().toString());
    }
    return;
  }

  wifiConnectedAt = 0;
  wifiConnectInProgress = false;

  if (!lastWifiAttemptAt ||
      millis() - lastWifiAttemptAt >= WIFI_RECONNECT_INTERVAL_MS) {
    startOperationalWifi();
  }
}

void applyBleConfig(const String& payload) {
  String ssid = extractJsonString(payload, "ssid");
  String password = extractJsonString(payload, "password");

  if (ssid.length() == 0) {
    publishBleResult("failed", "SSID required");
    return;
  }

  String requestedMode = extractJsonString(payload, "sensorMode");
  if (requestedMode.length() == 0)
    requestedMode = extractJsonString(payload, "deviceType");

  requestedMode.trim();
  requestedMode.toLowerCase();

  if (requestedMode.length() &&
      !isHumanPresenceMode(requestedMode)) {
    publishBleResult("failed", "human_presence only");
    return;
  }

  wifiName = ssid;
  wifiPassword = password;

  String v = extractJsonString(payload, "locationName");
  if (v.length()) locationName = v;

  v = extractJsonString(payload, "residentName");
  if (v.length()) residentName = v;

  v = extractJsonString(payload, "roomName");
  if (v.length()) roomName = v;

  v = extractJsonString(payload, "deviceName");
  if (v.length()) deviceName = v;
  if (deviceName.length() == 0) deviceName = "Human Presence Sensor";

  saveSettings();
  publishBleResult("success", "saved");
  updateBleStatus();

  bleBootWindowActive = false;
  delay(250);
  startOperationalWifi();

  if (extractJsonBool(payload, "restartAfterSave", false)) {
    delay(750);
    ESP.restart();
  }
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    bleClientConnected = true;
    bleConnectedAt = millis();
    updateBleStatus();
  }

  void onDisconnect(
    NimBLEServer* pServer,
    NimBLEConnInfo& connInfo,
    int reason
  ) override {
    bleClientConnected = false;
    bleConnectedAt = 0;

    if (!bleReleasedForRuntime) {
      NimBLEDevice::startAdvertising();
      if (bleBootWindowActive &&
          wifiName.length() && wifiPassword.length()) {
        bleBootWindowStartedAt = millis();
      }
    }
  }
};

class CommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(
    NimBLECharacteristic* characteristic,
    NimBLEConnInfo& connInfo
  ) override {
    if (pendingBleCommand) return;
    std::string value = characteristic->getValue();
    if (value.empty()) return;

    pendingBleCommandPayload = String(value.c_str());
    pendingBleCommand = true;
  }
};

void startBle() {
  if (bleStarted) return;

  String bleName = "GoodShepherd-" + setupId;
  NimBLEDevice::init(bleName.c_str());
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  NimBLEService* service = bleServer->createService(BLE_SERVICE_UUID);

  bleStatus = service->createCharacteristic(
    BLE_STATUS_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  bleCommand = service->createCharacteristic(
    BLE_COMMAND_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  bleCommand->setCallbacks(new CommandCallbacks());

  bleResult = service->createCharacteristic(
    BLE_RESULT_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->enableScanResponse(true);

  updateBleStatus();
  NimBLEDevice::startAdvertising();

  bleStarted = true;
  bleReleasedForRuntime = false;
  logLine("NimBLE management started as " + bleName);
}

void stopBleForRuntime() {
  if (!bleStarted || bleReleasedForRuntime) return;
  if (bleClientConnected) return;

  logLine("Releasing NimBLE for MQTT runtime.");
  NimBLEDevice::stopAdvertising();
  NimBLEDevice::deinit(true);

  bleServer = nullptr;
  bleStatus = nullptr;
  bleCommand = nullptr;
  bleResult = nullptr;

  bleStarted = false;
  bleReleasedForRuntime = true;
}

void serviceBleBootWindow() {
  if (!bleBootWindowActive) return;
  if (bleClientConnected) return;

  if (wifiName.length() == 0 || wifiPassword.length() == 0) return;

  if (bleBootWindowStartedAt == 0)
    bleBootWindowStartedAt = millis();

  if (millis() - bleBootWindowStartedAt < BLE_BOOT_SETUP_WINDOW_MS)
    return;

  bleBootWindowActive = false;
  logLine("BLE startup window expired; entering runtime.");
  startOperationalWifi();
}

void serviceBle() {
  if (!bleStarted) return;

  if (bleClientConnected &&
      bleConnectedAt &&
      millis() - bleConnectedAt >= BLE_MAX_CONNECTED_SESSION_MS) {
    bleServer->disconnect(0);
    return;
  }

  if (bleClientConnected &&
      (!lastBleStatusAt ||
       millis() - lastBleStatusAt >= BLE_STATUS_INTERVAL_MS)) {
    updateBleStatus();
  }
}

bool inventoryAlreadyRegistered() {
  prefs.begin("gs-device", true);
  bool done = prefs.getBool(FACTORY_REGISTRATION_MARKER_KEY, false);
  prefs.end();
  return done;
}

void markInventoryRegistered() {
  prefs.begin("gs-device", false);
  prefs.putBool(FACTORY_REGISTRATION_MARKER_KEY, true);
  prefs.end();
}

bool postRegistration() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  http.setReuse(false);

  if (!http.begin(client, REGISTER_URL)) return false;
  http.addHeader("Content-Type", "application/json");

  String p;
  p.reserve(650);
  p += "{";
  p += "\"nodeId\":\"" + jsonEscape(nodeId) + "\",";
  p += "\"sourceKey\":\"" + jsonEscape(sourceKey) + "\",";
  p += "\"setupId\":\"" + jsonEscape(setupId) + "\",";
  p += "\"deviceName\":\"" + jsonEscape(deviceName) + "\",";
  p += "\"locationName\":\"" + jsonEscape(locationName) + "\",";
  p += "\"residentName\":\"" + jsonEscape(residentName) + "\",";
  p += "\"roomName\":\"" + jsonEscape(roomName) + "\",";
  p += "\"sensorMode\":\"human_presence\",";
  p += "\"softwareVersion\":\"" + String(SOFTWARE_VERSION) + "\"";
  p += "}";

  int code = http.POST(p);
  http.end();
  client.stop();

  return code >= 200 && code < 300;
}

void runFactoryInventoryBootstrap() {
  if (inventoryAlreadyRegistered()) return;
  if (strlen(FACTORY_WIFI_SSID) == 0 ||
      strlen(FACTORY_WIFI_PASSWORD) == 0) return;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(FACTORY_WIFI_SSID, FACTORY_WIFI_PASSWORD);

  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < FACTORY_WIFI_TIMEOUT_MS) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (postRegistration()) markInventoryRegistered();
  }

  WiFi.disconnect(true, false);
  delay(150);
}


void stopBleForOta() {
  if (!bleStarted) return;

  NimBLEDevice::stopAdvertising();
  NimBLEDevice::deinit(true);

  bleServer = nullptr;
  bleStatus = nullptr;
  bleCommand = nullptr;
  bleResult = nullptr;
  bleStarted = false;
  bleClientConnected = false;
  bleReleasedForRuntime = true;

  delay(250);
}

void performFirmwareUpdate(
  const String& commandId,
  const String& commandType,
  const String& firmwareUrl
) {
  if (WiFi.status() != WL_CONNECTED) {
    if (commandId.length()) {
      publishCommandResult(
        commandId, commandType, "failed",
        "wifi offline"
      );
    }
    return;
  }

  if (mqtt.connected()) {
    if (commandId.length()) {
      publishCommandResult(
        commandId, commandType, "running",
        "downloading"
      );
    }
    mqtt.loop();
    delay(150);
    mqtt.disconnect();
  }

  mqttTls.stop();
  stopBleForOta();

  // Restore the proven OTA radio transition before opening HTTPS.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  delay(750);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(120000);

  HTTPClient http;
  http.setTimeout(180000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setReuse(false);
  http.setUserAgent("GoodShepherd-ESP32-OTA/1.9.5");

  if (!http.begin(client, firmwareUrl)) {
    logLine("OTA failed: could not open URL.");
    ESP.restart();
    return;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    logLine(String(code));
    http.end();
    client.stop();
    ESP.restart();
    return;
  }

  int contentLength = http.getSize();
  if (contentLength == 0) {
    logLine("OTA failed: empty firmware.");
    http.end();
    client.stop();
    ESP.restart();
    return;
  }

  if (!Update.begin(contentLength > 0
      ? (size_t)contentLength
      : UPDATE_SIZE_UNKNOWN)) {
    logLine("OTA failed: Update.begin error=" + String(Update.getError()));
    http.end();
    client.stop();
    ESP.restart();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);

  if (contentLength > 0 &&
      written != (size_t)contentLength) {
    Update.abort();
    logLine("OTA failed: incomplete write.");
    http.end();
    client.stop();
    ESP.restart();
    return;
  }

  bool ended = Update.end();
  bool finished = Update.isFinished();

  http.end();
  client.stop();

  if (!ended || !finished) {
    logLine("OTA finalization failed error=" + String(Update.getError()));
    ESP.restart();
    return;
  }

  logLine("OTA installed successfully; rebooting.");
  delay(1000);
  ESP.restart();
}

void executeMqttCommand(const String& payload) {
  String commandId = extractJsonString(payload, "commandId");
  String commandType = extractJsonString(payload, "commandType");
  commandType.trim();
  commandType.toLowerCase();

  if (commandId.length() == 0 || commandType.length() == 0) return;

  publishCommandResult(
    commandId, commandType, "running",
    "running"
  );

  if (commandType == "ping") {
    publishCommandResult(
      commandId, commandType, "success",
      "pong"
    );
    return;
  }

  if (commandType == "identify" || commandType == "locate") {
    blinkIdentify();
    publishCommandResult(
      commandId, commandType, "success",
      "identified"
    );
    return;
  }

  if (commandType == "reboot") {
    publishCommandResult(
      commandId, commandType, "success",
      "restarting"
    );
    mqtt.loop();
    delay(500);
    ESP.restart();
    return;
  }

  if (commandType == "reconfigure") {
    publishCommandResult(
      commandId, commandType, "success",
      "reconfiguring"
    );
    mqtt.loop();
    delay(300);
    clearAssignmentSettingsOnly();
    ESP.restart();
    return;
  }

  if (commandType == "factory_reset") {
    publishCommandResult(
      commandId, commandType, "success",
      "resetting"
    );
    mqtt.loop();
    delay(300);
    factoryClearEverything();
    ESP.restart();
    return;
  }

  if (commandType == "update_firmware") {
    String url = extractJsonString(payload, "firmwareUrl");
    if (url.length() == 0) {
      publishCommandResult(
        commandId, commandType, "failed",
        "missing firmwareUrl"
      );
      return;
    }
    performFirmwareUpdate(commandId, commandType, url);
    return;
  }

  publishCommandResult(
    commandId, commandType, "failed",
    "unsupported"
  );
}

void executeBleCommand(const String& payload) {
  String command = extractJsonString(payload, "command");
  if (command.length() == 0)
    command = extractJsonString(payload, "action");
  command.trim();
  command.toLowerCase();

  if (command.length() == 0) {
    publishBleResult("failed", "missing command");
    return;
  }

  if (command == "get_status" || command == "status") {
    updateBleStatus();
    publishBleResult("success", "ok");
    return;
  }

  if (command == "identify" || command == "locate") {
    publishBleResult("running", "running");
    blinkIdentify();
    publishBleResult("success", "identified");
    return;
  }

  if (command == "set_wifi" ||
      command == "sync_wifi" ||
      command == "sync_config") {
    applyBleConfig(payload);
    return;
  }

  if (command == "set_sensor_mode") {
    String mode = extractJsonString(payload, "sensorMode");
    mode.trim();
    mode.toLowerCase();

    if (mode.length() &&
        !isHumanPresenceMode(mode)) {
      publishBleResult(
        "failed",
        "human_presence only"
      );
      return;
    }

    String newName = extractJsonString(payload, "deviceName");
    if (newName.length()) deviceName = newName;
    saveSettings();

    publishBleResult(
      "success",
      "saved"
    );
    updateBleStatus();
    return;
  }

  if (command == "check_firmware") {
    publishBleResult(
      "success",
      String(SOFTWARE_VERSION)
    );
    return;
  }

  if (command == "force_update" ||
      command == "update_firmware") {
    String url = extractJsonString(payload, "firmwareUrl");
    if (url.length() == 0) {
      publishBleResult(
        "failed",
        "missing firmwareUrl"
      );
      return;
    }

    publishBleResult("running", "updating");
    delay(250);
    performFirmwareUpdate("", "update_firmware", url);
    return;
  }

  if (command == "restart" || command == "reboot") {
    publishBleResult("success", "restarting");
    delay(500);
    ESP.restart();
    return;
  }

  if (command == "reconfigure") {
    publishBleResult(
      "success",
      "reconfiguring"
    );
    delay(300);
    clearAssignmentSettingsOnly();
    ESP.restart();
    return;
  }

  if (command == "factory_reset") {
    publishBleResult("success", "resetting");
    delay(300);
    factoryClearEverything();
    ESP.restart();
    return;
  }

  if (command == "ping") {
    publishBleResult("success", "pong");
    return;
  }

  publishBleResult("failed", "unknown command");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PRESENCE_PIN, INPUT_PULLDOWN);
  pinMode(LED_PIN, OUTPUT);
  setLed(false);

  ensureIdentity();
  loadSettings();


  runFactoryInventoryBootstrap();
  startBle();

  bleBootWindowActive = true;

  if (wifiName.length() && wifiPassword.length()) {
    bleBootWindowStartedAt = millis();
    logLine("BLE setup window active for 60 seconds.");
  } else {
    bleBootWindowStartedAt = 0;
    logLine("No saved operational Wi-Fi; BLE remains available.");
  }
}

void loop() {
  if (pendingBleCommand) {
    String payload = pendingBleCommandPayload;
    pendingBleCommandPayload = "";
    pendingBleCommand = false;
    executeBleCommand(payload);
  }

  serviceBleBootWindow();
  serviceBle();

  if (!bleBootWindowActive) {
    serviceWifi();

    if (WiFi.status() == WL_CONNECTED) {
      if (!bleClientConnected &&
          bleStarted &&
          wifiConnectedAt &&
          millis() - wifiConnectedAt > 3000UL) {
        stopBleForRuntime();
      }

      serviceMqtt();

      if (pendingMqttCommand) {
        String payload = pendingMqttCommandPayload;
        pendingMqttCommandPayload = "";
        pendingMqttCommand = false;
        executeMqttCommand(payload);
      }

      if (mqtt.connected()) {
        servicePresence();
      }
    }
  }

  delay(10);
}
