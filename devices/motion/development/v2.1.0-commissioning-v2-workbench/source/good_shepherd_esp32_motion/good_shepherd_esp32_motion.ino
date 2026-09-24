/*
  Good Shepherd ESP32 Multi-Sensor Firmware
  ≈
  Date: 2026-08-06

  Supported sensor modes:
    motion            = PIR motion sensor on GPIO27

  Wiring:
    PIR VCC      -> ESP32 3V3
    PIR GND      -> ESP32 GND
    PIR OUT      -> ESP32 GPIO27


  Notes:
    - v1.8.8 makes command polling noncritical so optional command checks do not drive self-healing.
    - v1.8.9 makes BLE status read-only/set-value instead of repeated notify to prevent BLE/Wi-Fi stack aborts.
    - v1.9.1 isolated BLE service sessions from background cloud traffic.
    - v1.9.3 adds heartbeat-lite retry when full /node-health HTTPS fails.
    - v1.9.4 restores BLE status notifications for the Nearby Sensors screen.
    - v1.9.5 makes heartbeat-lite actually lean and uses it as the primary heartbeat path.
    - v1.9.6 separates heartbeat attempt/success state, classifies heartbeat failures,
      keeps healthy Wi-Fi associated during server failures, and avoids unconditional
      Wi-Fi refresh after BLE disconnect.
    - v1.9.8 makes the primary /node-health heartbeat genuinely minimal. Motion and
    - v1.9.9 schedules an immediate normal heartbeat after BLE disconnect recovery
      confirms Wi-Fi is still healthy, without changing BLE, Wi-Fi, or event semantics.
    - v1.9.12 restores a dedicated /node-health heartbeat schedule so command polls
      and other successful server traffic cannot suppress device liveness updates.
    - v1.9.13 adds diagnostic-only Wi-Fi event/reason logging and 30-second
      connectivity snapshots. It intentionally does not change reconnect,
      heartbeat, BLE, OTA, sensing, or self-healing behavior.
    - v1.9.14 makes terminal server-command acknowledgements durable and
      retryable. Success/failed results are retained in a separate Preferences
      namespace until Render acknowledges them.
    - v1.9.4 restores BLE status notifications during active BLE service sessions.
    - This keeps the Nearby Sensors screen live without allowing background cloud traffic to fight BLE.
    - BLE, Wi-Fi, registration, heartbeat, OTA, motion, and AI event behavior are preserved.
*/

#include <WiFi.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Update.h>
#include <NimBLEDevice.h>
#include <esp_system.h>

#define PIR_PIN 27
#define MOTION_ACTIVE_STATE HIGH



#define LED_PIN 2
#define LED_ACTIVE_STATE HIGH
#define LED_INACTIVE_STATE LOW

Preferences prefs;
// V2.0.5 uses the exact concrete-client pattern proven by the standalone
// HiveMQ test on this ESP32/core/network combination.
WiFiClientSecure v2MqttTlsClient;
PubSubClient v2MqttClient(v2MqttTlsClient);
unsigned long v2LastMqttConnectAttempt = 0;
unsigned long v2LastStatusPublish = 0;
unsigned long v2WifiConnectedSince = 0;
bool v2MqttConfigured = false;

// MQTT Phase 2 command handoff. The PubSubClient callback only captures the
// packet; execution happens from loop() after PubSubClient::loop() returns.
bool pendingMqttCommand = false;
String pendingMqttCommandPayload = "";

String wifiName = "";
String wifiPassword = "";
String nodeId = "";
String sourceKey = "";
String locationName = "";
String residentName = "";
String roomName = "";
String deviceName = "Motion Sensor";
String sensorMode = "motion";
String setupId = "";

const char* BASE_URL = "https://good-shepherd-server-j06f.onrender.com";
const char* REGISTER_URL = "https://good-shepherd-server-j06f.onrender.com/nodes/register";
const char* WEBHOOK_SECRET = "7e9c767aa079423227163be90943d7d2";

// Good Shepherd V2 MQTT primary transport
const char* MQTT_HOST = "c3f9bcc09adc4e7db6a3d29b63a24819.s1.eu.hivemq.cloud";
const uint16_t MQTT_PORT = 8883;
const char* MQTT_USERNAME = "good-shepherd-pilot";
const char* MQTT_PASSWORD = "Goodshepherd1!";
const uint16_t MQTT_KEEPALIVE_SECONDS = 30;
const unsigned long MQTT_RECONNECT_INTERVAL_MS = 10000;
const unsigned long MQTT_STATUS_PUBLISH_INTERVAL_MS = 60000;
const unsigned long BLE_BOOT_SETUP_WINDOW_MS = 60000;
const uint16_t MQTT_BUFFER_SIZE = 2048;
const unsigned long MQTT_WIFI_SETTLE_MS = 2500;

const char* SOFTWARE_VERSION = "esp32-good-shepherd-motion-v2.1.0-commissioning-v2";
//const char* SOFTWARE_VERSION = "esp32-good-shepherd-v2.0.0 update test";
// Factory Wi-Fi used only when the device has no saved Wi-Fi configuration.
// Set these two values before uploading production devices.
const char* FACTORY_WIFI_SSID = "PRESTIGE HOMECARE OFFICE";
const char* FACTORY_WIFI_PASSWORD = "Delaware109%";

// Isolated inventory bootstrap. Factory Wi-Fi is temporary only and is never
// written into the user's saved operational Wi-Fi settings.
const unsigned long FACTORY_REGISTRATION_WIFI_TIMEOUT_MS = 15000;
const char* FACTORY_REGISTRATION_MARKER_KEY = "invRegV1";

const char* BLE_SERVICE_UUID = "7d9f0001-2f4f-4c3a-8b2a-0b5f7f2a0001";
const char* BLE_STATUS_UUID  = "7d9f0002-2f4f-4c3a-8b2a-0b5f7f2a0001";
const char* BLE_COMMAND_UUID = "7d9f0003-2f4f-4c3a-8b2a-0b5f7f2a0001";
const char* BLE_RESULT_UUID  = "7d9f0004-2f4f-4c3a-8b2a-0b5f7f2a0001";

NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* bleStatusCharacteristic = nullptr;
NimBLECharacteristic* bleCommandCharacteristic = nullptr;
NimBLECharacteristic* bleResultCharacteristic = nullptr;

bool setupModeStarted = false;
bool nodeRegistered = false;
bool motionAlreadyReported = false;
bool firmwareUpdateInProgress = false;
bool bleStarted = false;
bool bleClientConnected = false;
bool bleShutdownInProgress = false;
bool bleReleasedForRuntime = false;
bool bleBootSetupWindowActive = false;
unsigned long bleBootSetupWindowStartedAt = 0;
bool pendingBleCommand = false;
bool pendingFirmwareStatusCheck = false;
bool wifiConnectInProgress = false;
bool pendingMotionDetectedEvent = false;

String pendingBleCommandPayload = "";
String lastWifiStatusText = "Not checked yet.";
String lastWifiFailureReason = "";
String lastConnectedIp = "";
String lastConnectedSsid = "";
String pendingRegistrationReason = "boot";
String lastWifiReconnectReason = "none";
String lastLiteHeartbeatCategory = "not_attempted";
String lastFullHeartbeatCategory = "not_attempted";

int lastPirState = -1;

unsigned long lastNoMotionTime = 0;
unsigned long lastHeartbeatAttemptTime = 0;
unsigned long lastHeartbeatSuccessTime = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long lastCommandPollTime = 0;
unsigned long lastRegistrationAttemptTime = 0;
unsigned long lastBleStatusUpdateTime = 0;
unsigned long lastFirmwareStatusCheckTime = 0;
unsigned long wifiConnectStartedAt = 0;
unsigned long lastServerSuccessTime = 0;
unsigned long lastServerFailureTime = 0;
unsigned long lastSelfHealCheckTime = 0;
unsigned long lastMotionWebhookAttemptTime = 0;
unsigned long lastMotionWebhookSuccessTime = 0;
unsigned long motionHeldActiveStartedAt = 0;
unsigned long lastBleClientConnectedAt = 0;
unsigned long lastBleClientDisconnectedAt = 0;
unsigned long lastBleCommandActivityAt = 0;
unsigned long lastBleStatusSetAt = 0;
unsigned long postBleWifiRecoveryAt = 0;
bool pendingBleWifiRecovery = false;

// Wi-Fi diagnostic telemetry only. These do not alter reconnect/reboot behavior.
unsigned long lastConnectivityDiagnosticTime = 0;
int lastObservedWifiStatus = -999;

unsigned long lastPriorityHttpAttemptTime = 0;

int consecutiveServerFailureCount = 0;
int lastLiteHeartbeatReturnCode = 0;
int lastFullHeartbeatReturnCode = 0;
bool serverFailureThresholdLogged = false;

const unsigned long RESET_AFTER_NO_MOTION_MS = 10000;
// Dedicated /node-health cadence.
// DIAGNOSTIC BUILD NOTE: there is deliberately NO immediate heartbeat after
// boot/registration. The first health heartbeat occurs on this timer.
const unsigned long HEARTBEAT_INTERVAL_MS = 300000;
const unsigned long RECONNECT_INTERVAL_MS = 10000;
const unsigned long QUIET_DEVICE_HEARTBEAT_FALLBACK_MS = 300000;
const unsigned long REGISTRATION_RETRY_INTERVAL_MS = 15000;
const unsigned long BLE_STATUS_INTERVAL_MS = 30000;
const unsigned long BLE_CONNECTED_STATUS_INTERVAL_MS = 10000;
const unsigned long FIRMWARE_STATUS_CHECK_COOLDOWN_MS = 300000;
const unsigned long HTTP_TIMEOUT_MS = 10000;
const unsigned long BLE_CONNECTED_BACKGROUND_HEARTBEAT_MS = 120000;
const unsigned long BLE_CONNECTED_CLOUD_QUIET_MS = 12000;
const unsigned long BLE_POST_DISCONNECT_RESUME_DELAY_MS = 15000;
const unsigned long BLE_POST_DISCONNECT_WIFI_RECOVERY_DELAY_MS = 8000;
const unsigned long BLE_MAX_SERVICE_SESSION_MS = 120000;
const unsigned long SELF_HEAL_CHECK_INTERVAL_MS = 30000;
const unsigned long CONNECTIVITY_DIAGNOSTIC_INTERVAL_MS = 30000;
const unsigned long SERVER_SILENCE_REBOOT_MS = 1800000;
const unsigned long MOTION_RETRY_INTERVAL_MS = 15000;
const unsigned long MOTION_HELD_ACTIVE_RESET_MS = 300000;
const int SERVER_FAILURE_RECONNECT_THRESHOLD = 5;

// MARK: - Forward declarations
void startSetupMode();
void stopSetupMode();
void connectToSavedWifi(bool allowSetupFallback = true);
void connectToNewWifiFromBle(String ssid, String password, String newLocation, String newResident, String newRoom, String newDeviceName, String newSensorMode, bool shouldRestartAfterSave);

void handleWifiReconnect();
void handleWifiDiagnosticEvent(WiFiEvent_t event, WiFiEventInfo_t info);
void logConnectivityDiagnostic(String reason);
void handleConnectivityDiagnosticTick();
void v2MqttHandle();
void v2MqttConfigureOnce();
bool v2MqttConnect();
bool v2PublishStatus(bool online);
bool v2PublishEvent(const String& payload);
bool v2PublishCommandResult(String commandId, String commandType, String status, String message);
void handlePendingMqttCommand();
void executeMqttCommand(String payload);
void v2MqttShutdown();
bool registerNode();
bool inventoryRegistrationAlreadyConfirmed();
void markInventoryRegistrationConfirmed();
bool runFactoryInventoryRegistrationBootstrap();
bool sendHeartbeat(bool criticalFailure = true);
bool checkLatestFirmwareStatusOnly();
void performFirmwareUpdate(String commandId, String firmwareUrl);
void stopBleForFirmwareUpdate();
void stopBleForNormalRuntime();
void finishFirmwareUpdateFailure(String commandId, String message);
String httpErrorDescription(HTTPClient& http, int responseCode);
String classifyHeartbeatResult(HTTPClient& http, int responseCode);
String heartbeatAgeText(unsigned long timestamp);
String resetReasonText();
void logHeartbeatRecoveryState(String outcome);
void logRegistrationAttempt();
void handleBleCommand(String payload);
bool bleClientCloudQuietActive();
bool blePostDisconnectHoldActive();
bool backgroundCloudAllowed();
void markBleCommandActivity();
void handlePostBleWifiRecovery();
void enforceBleServiceSessionTimeout();
void handleBleBootSetupWindow();
void handleMotionSensor();
bool sendMotionEvent();
void markServerSuccess(String context);
void markServerFailure(String context, int responseCode);
void markSetupCloudFailure(String context, int responseCode);
void reconnectWifiNow(String reason);
void handleSelfHealing();

// MARK: - Utility
String getChipId() {
  uint64_t chipid = ESP.getEfuseMac();
  char id[13];
  snprintf(id, sizeof(id), "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
  String result = String(id);
  result.toLowerCase();
  return result;
}

String normalizedSensorMode(String value) {
  value.trim();
  value.toLowerCase();
  value.replace("-", "_");
  value.replace(" ", "_");

  if (value == "motion" ||
      value == "motion_sensor" ||
      value == "pir" ||
      value == "pir_motion") {
    return "motion";
  }

  return "";
}


bool isValidSensorMode(String value) { return normalizedSensorMode(value).length() > 0; }
String safeSensorModeOrMotion(String value) { String parsed = normalizedSensorMode(value); return parsed.length() > 0 ? parsed : "motion"; }

String defaultDeviceNameForMode(String mode) {
  (void)mode;
  return "Motion Sensor";
}


bool modeUsesMotion() {
  return true;
}


int activeMonitorCountForMode() {
  return 1;
}

String sourcePrefixForMode() {
  return "motion";
}


void generateHardwareIds() {
  String chipId = getChipId();
  nodeId = "esp32-" + chipId;
  sourceKey = sourcePrefixForMode() + "-" + chipId;
}

String generateSetupId() {
  const char* alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  String result = "";
  randomSeed((uint32_t)ESP.getEfuseMac() ^ micros());
  for (int i = 0; i < 6; i++) result += alphabet[random(0, 32)];
  return result;
}

void ensureSetupId() {
  prefs.begin("gs-device", false);
  setupId = prefs.getString("setupId", "");
  if (setupId.length() != 6) {
    setupId = generateSetupId();
    prefs.putString("setupId", setupId);
    Serial.println("Generated setup ID: " + setupId);
  } else {
    Serial.println("Loaded setup ID: " + setupId);
  }
  prefs.end();
}

void setLed(bool isOn) { digitalWrite(LED_PIN, isOn ? LED_ACTIVE_STATE : LED_INACTIVE_STATE); }
String assignmentState() { return (locationName.length() == 0 || residentName.length() == 0 || roomName.length() == 0) ? "Unassigned" : "Assigned"; }

String sensorLabel() {
  String room = roomName;
  room.trim();
  if (room.length() == 0) room = "No Room";
  return nodeId + " | " + room + " | " + sensorMode;
}

void logSensor(String message) { Serial.println(sensorLabel() + " | " + message); }

String jsonEscape(String value) { value.replace("\\", "\\\\"); value.replace("\"", "\\\""); value.replace("\n", "\\n"); value.replace("\r", ""); return value; }
uint16_t readUInt16LE(uint8_t low, uint8_t high) { return ((uint16_t)high << 8) | low; }

String extractJsonString(String json, String key) {
  String pattern = "\"" + key + "\":\"";
  int start = json.indexOf(pattern);
  if (start < 0) return "";
  start += pattern.length();
  String result = "";
  bool escaped = false;
  for (int i = start; i < json.length(); i++) {
    char c = json.charAt(i);
    if (escaped) {
      if (c == 'n') result += '\n'; else if (c == 'r') result += '\r'; else if (c == 't') result += '\t'; else result += c;
      escaped = false;
    } else if (c == '\\') escaped = true;
    else if (c == '"') break;
    else result += c;
  }
  return result;
}

bool extractJsonBool(String json, String key, bool fallback) {
  if (json.indexOf("\"" + key + "\":true") >= 0) return true;
  if (json.indexOf("\"" + key + "\":false") >= 0) return false;
  return fallback;
}

String wifiStatusCodeText(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "idle";
    case WL_NO_SSID_AVAIL: return "ssid_not_found";
    case WL_SCAN_COMPLETED: return "scan_completed";
    case WL_CONNECTED: return "connected";
    case WL_CONNECT_FAILED: return "connect_failed";
    case WL_CONNECTION_LOST: return "connection_lost";
    case WL_DISCONNECTED: return "disconnected";
    default: return "unknown_" + String((int)status);
  }
}

String resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

String heartbeatAgeText(unsigned long timestamp) {
  if (timestamp == 0) return "never";
  return String((millis() - timestamp) / 1000) + "s";
}

String classifyHeartbeatResult(HTTPClient& http, int responseCode) {
  if (WiFi.status() != WL_CONNECTED) return "wifi_disconnected";
  if (responseCode < 0) {
    String errorText = http.errorToString(responseCode);
    errorText.toLowerCase();
    if (errorText.indexOf("timeout") >= 0 || errorText.indexOf("timed out") >= 0) return "timeout";
    return "negative_transport_error";
  }
  if (responseCode >= 400 && responseCode < 500) return "http_4xx";
  if (responseCode >= 500 && responseCode < 600) return "http_5xx";
  if (responseCode >= 200 && responseCode < 300) return "success";
  return "other_non_2xx";
}

void logHeartbeatRecoveryState(String outcome) {
  Serial.println(
    sensorLabel() + " | Heartbeat " + outcome +
    " | wifi=" + wifiStatusCodeText(WiFi.status()) +
    " | failures=" + String(consecutiveServerFailureCount) +
    " | lite=" + String(lastLiteHeartbeatReturnCode) + "/" + lastLiteHeartbeatCategory +
    " | full=" + String(lastFullHeartbeatReturnCode) + "/" + lastFullHeartbeatCategory +
    " | attemptAge=" + heartbeatAgeText(lastHeartbeatAttemptTime) +
    " | successAge=" + heartbeatAgeText(lastHeartbeatSuccessTime)
  );
}

void logRegistrationAttempt() {
  Serial.println(
    sensorLabel() + " | Registration attempt" +
    " | reason=" + pendingRegistrationReason +
    " | uptime=" + String(millis() / 1000) + "s" +
    " | reset=" + resetReasonText() +
    " | wifi=" + wifiStatusCodeText(WiFi.status()) +
    " | failures=" + String(consecutiveServerFailureCount) +
    " | heartbeatAttemptAge=" + heartbeatAgeText(lastHeartbeatAttemptTime) +
    " | heartbeatSuccessAge=" + heartbeatAgeText(lastHeartbeatSuccessTime)
  );
}

// MARK: - Wi-Fi Credential History




// MARK: - BLE
void publishBleResult(String status, String message) {
  String payload = "{";
  payload += "\"status\":\"" + jsonEscape(status) + "\",";
  payload += "\"message\":\"" + jsonEscape(message) + "\",";
  payload += "\"nodeId\":\"" + jsonEscape(nodeId) + "\",";
  payload += "\"sourceKey\":\"" + jsonEscape(sourceKey) + "\",";
  payload += "\"setupId\":\"" + jsonEscape(setupId) + "\",";
  payload += "\"sensorMode\":\"" + jsonEscape(sensorMode) + "\",";
  payload += "\"softwareVersion\":\"" + jsonEscape(String(SOFTWARE_VERSION)) + "\"";
  payload += "}";
  if (bleResultCharacteristic != nullptr) {
    bleResultCharacteristic->setValue(payload.c_str());
    if (bleClientConnected) bleResultCharacteristic->notify();
  }
  Serial.println("BLE result: " + payload);
}

String effectiveWifiStatus() {
  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) return "connected";
  if (wifiConnectInProgress) return "connecting";
  if (setupModeStarted) return "setup_ap";
  if (wifiName.length() == 0) return "unconfigured";
  if (lastWifiFailureReason.length() > 0) return "connection_failed";
  return "configured_offline";
}

void updateCachedWifiStatusText() {
  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    lastConnectedIp = WiFi.localIP().toString();
    lastConnectedSsid = WiFi.SSID();
    lastWifiFailureReason = "";
    lastWifiStatusText = "Wi-Fi connected: " + lastConnectedSsid + " / " + lastConnectedIp;
    return;
  }
  if (wifiConnectInProgress) {
    unsigned long seconds = (millis() - wifiConnectStartedAt) / 1000;
    lastWifiStatusText = "Connecting to Wi-Fi: " + wifiName + " (" + String(seconds) + "s)";
    return;
  }
  if (setupModeStarted) { lastWifiStatusText = "Setup hotspot active: GoodShepherd-Setup"; return; }
  if (wifiName.length() == 0) { lastWifiStatusText = "No Wi-Fi configured."; return; }
  if (lastWifiFailureReason.length() > 0) { lastWifiStatusText = lastWifiFailureReason; return; }
  lastWifiStatusText = "Wi-Fi configured but offline. Status: " + wifiStatusCodeText(status);
}

String buildBleStatusPayload() {
  updateCachedWifiStatusText();
  String wifiStatus = effectiveWifiStatus();

  // During the BLE setup window the ESP32 intentionally keeps normal Wi-Fi /
  // MQTT runtime offline. Report the SAVED SSID so the installer can see that
  // Wi-Fi configuration exists even while the radio is reserved for BLE.
  // localIp remains blank until there is a live Wi-Fi connection.
  String ip = "";
  String ssid = wifiName;
  int rssi = 0;

  if (WiFi.status() == WL_CONNECTED) {
    ip = WiFi.localIP().toString();
    ssid = WiFi.SSID();
    rssi = WiFi.RSSI();
  }
  int motionState = digitalRead(PIR_PIN);
  String payload = "{";
  payload += "\"nodeId\":\"" + jsonEscape(nodeId) + "\",";
  payload += "\"sourceKey\":\"" + jsonEscape(sourceKey) + "\",";
  payload += "\"setupId\":\"" + jsonEscape(setupId) + "\",";
  payload += "\"deviceName\":\"" + jsonEscape(deviceName) + "\",";
  payload += "\"sensorMode\":\"" + jsonEscape(sensorMode) + "\",";
  payload += "\"roomName\":\"" + jsonEscape(roomName) + "\",";
  payload += "\"residentName\":\"" + jsonEscape(residentName) + "\",";
  payload += "\"softwareVersion\":\"" + jsonEscape(String(SOFTWARE_VERSION)) + "\",";
  payload += "\"wifiStatus\":\"" + jsonEscape(wifiStatus) + "\",";
  payload += "\"wifiSsid\":\"" + jsonEscape(ssid) + "\",";
  payload += "\"localIp\":\"" + jsonEscape(ip) + "\",";
  payload += "\"wifiRssi\":" + String(rssi) + ",";
  payload += "\"motionState\":" + String(motionState == MOTION_ACTIVE_STATE ? "true" : "false") + ",";
  // Keep BLE setup status compact and stable.
  // Radar details are intentionally not included in BLE status.
  payload += "\"uptimeSeconds\":" + String(millis() / 1000) + ",";
  payload += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  payload += "\"serverFailures\":" + String(consecutiveServerFailureCount);
  payload += "}";
  Serial.print(sensorLabel() + " | BLE status bytes: "); Serial.println(payload.length());
  return payload;
}

void updateBleStatus() {
  if (bleStatusCharacteristic == nullptr) return;
  String payload = buildBleStatusPayload();
  bleStatusCharacteristic->setValue(payload.c_str());
  lastBleStatusSetAt = millis();

  // v1.9.4: the iOS Nearby Sensors screen expects live BLE status payloads.
  // Notify only while a BLE client is connected. Background cloud traffic is
  // still paused/throttled during the BLE service session, so BLE status updates
  // no longer compete with heartbeat/command HTTPS calls.
  if (bleClientConnected) {
    bleStatusCharacteristic->notify();
    Serial.println(sensorLabel() + " | BLE status notified.");
  }
}

void blinkIdentifyLed(unsigned long durationMs) {
  unsigned long startedAt = millis();
  bool ledOn = false;
  logSensor("Identify LED started.");
  while (millis() - startedAt < durationMs) { ledOn = !ledOn; setLed(ledOn); delay(150); }
  setLed(false);
  logSensor("Identify LED finished.");
}

class GoodShepherdBleServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(
    NimBLEServer* server,
    NimBLEConnInfo& connInfo
  ) override {
    bleClientConnected = true;
    bleShutdownInProgress = false;
    lastBleClientConnectedAt = millis();
    lastBleCommandActivityAt = millis();

    if (bleBootSetupWindowActive) {
      bleBootSetupWindowStartedAt = 0;
    }

    updateBleStatus();
    Serial.println("BLE client connected.");
  }

  void onDisconnect(
    NimBLEServer* server,
    NimBLEConnInfo& connInfo,
    int reason
  ) override {
    bleClientConnected = false;
    lastBleClientDisconnectedAt = millis();

    if (!bleReleasedForRuntime && bleStarted) {
      NimBLEDevice::startAdvertising();

      if (bleBootSetupWindowActive &&
          wifiName.length() > 0 &&
          wifiPassword.length() > 0) {
        bleBootSetupWindowStartedAt = millis();
      }
    }

    Serial.println("BLE client disconnected.");
  }
};

class GoodShepherdBleCommandCallbacks :
  public NimBLECharacteristicCallbacks {
  void onWrite(
    NimBLECharacteristic* characteristic,
    NimBLEConnInfo& connInfo
  ) override {
    if (pendingBleCommand) return;

    std::string value = characteristic->getValue();
    if (value.empty()) return;

    pendingBleCommandPayload = String(value.c_str());
    pendingBleCommand = true;
    lastBleCommandActivityAt = millis();

    Serial.print("BLE command received. Bytes: ");
    Serial.println(pendingBleCommandPayload.length());
    Serial.println(
      "Content omitted because it may contain credentials."
    );
  }
};

void startBleManagement() {
  if (bleStarted) return;

  bleShutdownInProgress = false;
  bleReleasedForRuntime = false;

  generateHardwareIds();

  String bleName = "GoodShepherd-" + setupId;

  NimBLEDevice::init(bleName.c_str());
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new GoodShepherdBleServerCallbacks());

  NimBLEService* service =
    bleServer->createService(BLE_SERVICE_UUID);

  bleStatusCharacteristic = service->createCharacteristic(
    BLE_STATUS_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  bleCommandCharacteristic = service->createCharacteristic(
    BLE_COMMAND_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );

  bleCommandCharacteristic->setCallbacks(
    new GoodShepherdBleCommandCallbacks()
  );

  bleResultCharacteristic = service->createCharacteristic(
    BLE_RESULT_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  service->start();

  NimBLEAdvertising* advertising =
    NimBLEDevice::getAdvertising();

  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->enableScanResponse(true);

  updateBleStatus();
  NimBLEDevice::startAdvertising();

  bleStarted = true;
  bleReleasedForRuntime = false;

  Serial.println("NimBLE management started.");
  Serial.println("BLE name: " + bleName);
}


// MARK: - BLE / Cloud Coexistence
void markBleCommandActivity() {
  lastBleCommandActivityAt = millis();
}

bool bleClientCloudQuietActive() {
  if (!bleClientConnected) return false;
  unsigned long now = millis();
  if (lastBleCommandActivityAt > 0 && now - lastBleCommandActivityAt < BLE_CONNECTED_CLOUD_QUIET_MS) return true;
  return false;
}

bool blePostDisconnectHoldActive() {
  if (bleClientConnected) return false;
  if (lastBleClientDisconnectedAt == 0) return false;
  return millis() - lastBleClientDisconnectedAt < BLE_POST_DISCONNECT_RESUME_DELAY_MS;
}

bool backgroundCloudAllowed() {
  if (firmwareUpdateInProgress || wifiConnectInProgress) return false;
  if (blePostDisconnectHoldActive()) return false;
  if (bleClientConnected && bleClientCloudQuietActive()) return false;
  return true;
}

void enforceBleServiceSessionTimeout() {
  if (bleBootSetupWindowActive) return;
  if (!bleClientConnected || lastBleClientConnectedAt == 0) return;

  if (millis() - lastBleClientConnectedAt >=
      BLE_MAX_SERVICE_SESSION_MS) {
    Serial.println(
      "BLE service session exceeded safety window; "
      "waiting for client disconnect."
    );
  }
}


void handleBleBootSetupWindow() {
  if (!bleBootSetupWindowActive) return;

  // Stay available for commissioning indefinitely while unassigned.
  // Saved Wi-Fi is intentionally not started until BLE setup supplies
  // the next resident/location/room assignment.
  if (assignmentState() == "Unassigned") {
    bleBootSetupWindowStartedAt = 0;
    return;
  }
  if (!bleStarted || bleReleasedForRuntime) {
    bleBootSetupWindowActive = false;
    return;
  }

  // A connected installer owns the setup session; there is no setup timer
  // while the phone remains connected.
  if (bleClientConnected) return;

  // No usable saved Wi-Fi means there is no normal runtime to fall back to.
  // Keep BLE available indefinitely for setup/recovery.
  if (wifiName.length() == 0 || wifiPassword.length() == 0) return;

  if (bleBootSetupWindowStartedAt == 0) {
    bleBootSetupWindowStartedAt = millis();
  }

  if (millis() - bleBootSetupWindowStartedAt < BLE_BOOT_SETUP_WINDOW_MS) return;

  bleBootSetupWindowActive = false;

  Serial.println("60-second BLE setup window expired with no client connected.");
  Serial.println("Transitioning to saved Wi-Fi and MQTT runtime.");

  connectToSavedWifi(false);
}

void handlePostBleWifiRecovery() {
  if (!pendingBleWifiRecovery) return;
  if (bleClientConnected || firmwareUpdateInProgress || wifiConnectInProgress || setupModeStarted) return;
  if (millis() < postBleWifiRecoveryAt) return;

  pendingBleWifiRecovery = false;

  if (wifiName.length() == 0 || wifiPassword.length() == 0) return;

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(sensorLabel() + " | BLE recovery decision | wifi=connected | action=retain_connection_and_registration");
    // v1.9.9: keep the healthy Wi-Fi connection and existing registration, but make
    // the normal heartbeat due as soon as the existing post-BLE hold period ends.
    lastHeartbeatAttemptTime = millis() - HEARTBEAT_INTERVAL_MS;
    return;
  }

  Serial.println(sensorLabel() + " | BLE recovery decision | wifi=" + wifiStatusCodeText(WiFi.status()) + " | action=reconnect_and_reregister");
  pendingRegistrationReason = "post_ble_wifi_unhealthy";
  lastWifiReconnectReason = "post_ble_wifi_unhealthy";
  nodeRegistered = false;
  lastWifiStatusText = "BLE session ended with unhealthy Wi-Fi. Reconnecting before cloud resume.";

  WiFi.disconnect(false);
  delay(300);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  delay(100);
  WiFi.begin(wifiName.c_str(), wifiPassword.c_str());

  lastReconnectAttempt = millis();
  lastRegistrationAttemptTime = 0;
  lastCommandPollTime = millis();
}

// MARK: - Settings
void loadSettings() {
  prefs.begin("gs-device", true);
  wifiName = prefs.getString("wifiName", "");
  wifiPassword = prefs.getString("wifiPass", "");
  locationName = prefs.getString("location", "");
  residentName = prefs.getString("resident", "");
  roomName = prefs.getString("room", "");
  sensorMode = safeSensorModeOrMotion(prefs.getString("sensorMode", "motion"));
  deviceName = prefs.getString("deviceName", defaultDeviceNameForMode(sensorMode));
  prefs.end();
  generateHardwareIds();
  Serial.println("Settings loaded.");
  Serial.println("Hardware ID: " + nodeId);
  Serial.println("Source Key: " + sourceKey);
  Serial.println("Setup ID: " + setupId);
  Serial.println("Wi-Fi Name: " + wifiName);
  Serial.println("Room Name: " + roomName);
  Serial.println("Device Name: " + deviceName);
  Serial.println("Sensor Mode: " + sensorMode);

  sensorMode = "motion";
}

void saveSettings() {
  sensorMode = safeSensorModeOrMotion(sensorMode);
  prefs.begin("gs-device", false);
  prefs.putString("wifiName", wifiName);
  prefs.putString("wifiPass", wifiPassword);
  prefs.remove("nodeId");
  prefs.remove("sourceKey");
  prefs.putString("location", locationName);
  prefs.putString("resident", residentName);
  prefs.putString("room", roomName);
  prefs.putString("deviceName", deviceName);
  prefs.putString("sensorMode", sensorMode);
  prefs.end();
  generateHardwareIds();
  Serial.println("Settings saved.");
  Serial.println("Sensor Mode: " + sensorMode);
}

void clearAssignmentSettingsOnly() {
  // Recommissioning intentionally preserves the physical sensor identity,
  // current firmware, sensor mode/name, setup ID, and saved Wi-Fi.
  // Only resident-specific assignment fields are removed.
  prefs.begin("gs-device", false);
  prefs.remove("location");
  prefs.remove("resident");
  prefs.remove("room");
  prefs.end();

  locationName = "";
  residentName = "";
  roomName = "";

  nodeRegistered = false;
  pendingRegistrationReason = "explicit_reconfigure";
  lastWifiFailureReason = "";
  lastWifiStatusText =
    "Assignment cleared. Sensor is waiting in BLE commissioning mode.";
  lastConnectedIp = "";
  lastConnectedSsid = "";

  generateHardwareIds();

  Serial.println(
    "Assignment cleared. Wi-Fi, setup ID, device identity, "
    "device name, and sensor mode preserved."
  );
}

void factoryClearEverything() {
  prefs.begin("gs-device", false); prefs.clear(); prefs.end();
  wifiName = ""; wifiPassword = ""; locationName = ""; residentName = ""; roomName = ""; sensorMode = "motion"; deviceName = defaultDeviceNameForMode(sensorMode);
  nodeRegistered = false; pendingRegistrationReason = "factory_reset"; lastWifiFailureReason = ""; lastWifiStatusText = "Factory reset complete. No Wi-Fi configured."; lastConnectedIp = ""; lastConnectedSsid = "";
  generateHardwareIds(); ensureSetupId();
  Serial.println("Factory settings cleared, including saved Wi-Fi history. New setup ID generated.");
}

void clearSettingsAndRestart() { logSensor("Reconfigure requested. Clearing assignment settings and restarting..."); clearAssignmentSettingsOnly(); delay(1000); ESP.restart(); }

void markServerSuccess(String context) {
  lastServerSuccessTime = millis(); consecutiveServerFailureCount = 0; serverFailureThresholdLogged = false; lastWifiFailureReason = "";
  if (WiFi.status() == WL_CONNECTED) { lastConnectedIp = WiFi.localIP().toString(); lastConnectedSsid = WiFi.SSID(); lastWifiStatusText = "Wi-Fi connected: " + lastConnectedSsid + " / " + lastConnectedIp; }
  Serial.println(sensorLabel() + " | Server success: " + context);
}

void markServerFailure(String context, int responseCode) {
  lastServerFailureTime = millis(); consecutiveServerFailureCount++;
  String responseText = responseCode == 0 ? "no_response" : String(responseCode);
  lastWifiFailureReason = context + " failed. Response: " + responseText + ". Failure count: " + String(consecutiveServerFailureCount);
  lastWifiStatusText = lastWifiFailureReason;
  Serial.println(sensorLabel() + " | " + lastWifiFailureReason);
}

void markSetupCloudFailure(String context, int responseCode) {
  // Initial setup must treat Wi-Fi and cloud registration separately.
  // A temporary Render/HTTPS failure must not make BLE show Wi-Fi as failed,
  // must not increment the self-heal counter, and must not reconnect Wi-Fi.
  lastServerFailureTime = millis();
  String responseText = responseCode == 0 ? "no_response" : String(responseCode);
  Serial.println(sensorLabel() + " | Setup cloud sync pending: " + context + " response " + responseText + ". Wi-Fi remains connected.");
  if (WiFi.status() == WL_CONNECTED) {
    lastConnectedIp = WiFi.localIP().toString();
    lastConnectedSsid = WiFi.SSID();
    lastWifiFailureReason = "";
    lastWifiStatusText = "Wi-Fi connected; cloud sync pending: " + lastConnectedSsid + " / " + lastConnectedIp;
  }
}

void handleWifiDiagnosticEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  // Espressif documents Wi-Fi event callbacks as running on a separate
  // FreeRTOS task. Keep this callback diagnostic-only: Serial output only,
  // no changes to shared firmware state or Wi-Fi behavior.
  unsigned long nowMs = millis();
  Serial.print(sensorLabel() + " | WIFI_EVENT | uptimeMs=" + String(nowMs) + " | event=" + String((int)event));

  switch (event) {
    case ARDUINO_EVENT_WIFI_READY:
      Serial.println(" | name=WIFI_READY");
      break;
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.println(" | name=STA_START");
      break;
    case ARDUINO_EVENT_WIFI_STA_STOP:
      Serial.println(" | name=STA_STOP");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println(" | name=STA_CONNECTED");
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.print(" | name=STA_DISCONNECTED | reason=");
      Serial.println((int)info.wifi_sta_disconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print(" | name=STA_GOT_IP | ip=");
      Serial.println(WiFi.localIP());
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      Serial.println(" | name=STA_LOST_IP");
      break;
    case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
      Serial.println(" | name=STA_AUTHMODE_CHANGE");
      break;
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.println(" | name=AP_START");
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      Serial.println(" | name=AP_STOP");
      break;
    default:
      Serial.println(" | name=OTHER");
      break;
  }
}

void logConnectivityDiagnostic(String reason) {
  unsigned long nowMs = millis();
  int status = (int)WiFi.status();

  String ssid = WiFi.SSID();
  String bssid = WiFi.BSSIDstr();
  String ip = WiFi.localIP().toString();
  String gateway = WiFi.gatewayIP().toString();
  String dns = WiFi.dnsIP().toString();

  unsigned long serverSuccessAgeMs =
    lastServerSuccessTime > 0 ? nowMs - lastServerSuccessTime : 0;
  unsigned long serverFailureAgeMs =
    lastServerFailureTime > 0 ? nowMs - lastServerFailureTime : 0;
  unsigned long heartbeatAttemptAgeMs =
    lastHeartbeatAttemptTime > 0 ? nowMs - lastHeartbeatAttemptTime : 0;
  unsigned long heartbeatSuccessAgeMs =
    lastHeartbeatSuccessTime > 0 ? nowMs - lastHeartbeatSuccessTime : 0;

  Serial.println(
    sensorLabel() +
    " | NET_DIAG" +
    " | reason=" + reason +
    " | uptimeMs=" + String(nowMs) +
    " | wifiStatus=" + String(status) +
    "/" + wifiStatusCodeText((wl_status_t)status) +
    " | ssid=" + (ssid.length() ? ssid : "<none>") +
    " | bssid=" + (bssid.length() ? bssid : "<none>") +
    " | rssi=" + String(WiFi.RSSI()) +
    " | ip=" + ip +
    " | gateway=" + gateway +
    " | dns=" + dns +
    " | nodeRegistered=" + String(nodeRegistered ? "true" : "false") +
    " | setupMode=" + String(setupModeStarted ? "true" : "false") +
    " | wifiConnectInProgress=" + String(wifiConnectInProgress ? "true" : "false") +
    " | bleConnected=" + String(bleClientConnected ? "true" : "false") +
    " | serverFailures=" + String(consecutiveServerFailureCount) +
    " | lastServerSuccessAgeMs=" + String(serverSuccessAgeMs) +
    " | lastServerFailureAgeMs=" + String(serverFailureAgeMs) +
    " | lastHeartbeatAttemptAgeMs=" + String(heartbeatAttemptAgeMs) +
    " | lastHeartbeatSuccessAgeMs=" + String(heartbeatSuccessAgeMs) +
    " | liteHeartbeat=" + String(lastLiteHeartbeatReturnCode) + "/" + lastLiteHeartbeatCategory +
    " | fullHeartbeat=" + String(lastFullHeartbeatReturnCode) + "/" + lastFullHeartbeatCategory +
    " | reconnectReason=" + lastWifiReconnectReason +
    " | freeHeap=" + String(ESP.getFreeHeap()) +
    " | minFreeHeap=" + String(ESP.getMinFreeHeap()) +
    " | resetReason=" + resetReasonText()
  );
}

void handleConnectivityDiagnosticTick() {
  int currentStatus = (int)WiFi.status();

  if (lastObservedWifiStatus == -999) {
    lastObservedWifiStatus = currentStatus;
    logConnectivityDiagnostic("initial_state");
  } else if (currentStatus != lastObservedWifiStatus) {
    int previousStatus = lastObservedWifiStatus;
    lastObservedWifiStatus = currentStatus;
    Serial.println(
      sensorLabel() +
      " | WIFI_STATUS_TRANSITION | from=" + String(previousStatus) +
      " | to=" + String(currentStatus) +
      " | uptimeMs=" + String(millis())
    );
    logConnectivityDiagnostic("wifi_status_transition");
  }

  if (millis() - lastConnectivityDiagnosticTime >= CONNECTIVITY_DIAGNOSTIC_INTERVAL_MS) {
    lastConnectivityDiagnosticTime = millis();
    logConnectivityDiagnostic("periodic_30s");
  }
}

void reconnectWifiNow(String reason) {
  if (wifiName.length() == 0 || wifiPassword.length() == 0 || firmwareUpdateInProgress || wifiConnectInProgress) return;
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(sensorLabel() + " | Wi-Fi reconnect skipped | reason=station_still_connected | requestedBy=" + reason);
    return;
  }
  lastWifiReconnectReason = reason;
  pendingRegistrationReason = "wifi_reconnect";
  Serial.println(sensorLabel() + " | Wi-Fi reconnect | reason=" + reason + " | wifi=" + wifiStatusCodeText(WiFi.status()));
  lastWifiStatusText = "Self-heal reconnect: " + reason;
  nodeRegistered = false;
  WiFi.disconnect(false); delay(250); WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.begin(wifiName.c_str(), wifiPassword.c_str());
  lastReconnectAttempt = millis(); updateBleStatus();
}

void handleSelfHealing() {
  if (setupModeStarted || firmwareUpdateInProgress || wifiName.length() == 0) return;
  if (!nodeRegistered && WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastSelfHealCheckTime < SELF_HEAL_CHECK_INTERVAL_MS) return;
  lastSelfHealCheckTime = millis();
  if (WiFi.status() != WL_CONNECTED) { reconnectWifiNow("Wi-Fi is disconnected"); return; }
  if (consecutiveServerFailureCount >= SERVER_FAILURE_RECONNECT_THRESHOLD && !serverFailureThresholdLogged) {
    serverFailureThresholdLogged = true;
    Serial.println(sensorLabel() + " | Server failures reached " + String(consecutiveServerFailureCount) + " while Wi-Fi remains connected; retaining Wi-Fi and registration.");
  }
  if (lastServerSuccessTime > 0 && millis() - lastServerSuccessTime >= SERVER_SILENCE_REBOOT_MS) { Serial.println(sensorLabel() + " | Rebooting after prolonged server silence."); delay(500); ESP.restart(); }
}


// MARK: - Factory Inventory Registration Bootstrap
bool inventoryRegistrationAlreadyConfirmed() {
  prefs.begin("gs-device", true);
  bool confirmed = prefs.getBool(FACTORY_REGISTRATION_MARKER_KEY, false);
  prefs.end();
  return confirmed;
}

void markInventoryRegistrationConfirmed() {
  prefs.begin("gs-device", false);
  prefs.putBool(FACTORY_REGISTRATION_MARKER_KEY, true);
  prefs.end();
}

bool runFactoryInventoryRegistrationBootstrap() {
  if (inventoryRegistrationAlreadyConfirmed()) {
    Serial.println(sensorLabel() + " | Factory inventory registration already confirmed. Bootstrap skipped.");
    return true;
  }

  if (String(FACTORY_WIFI_SSID).length() == 0 ||
      String(FACTORY_WIFI_SSID) == "YOUR_FACTORY_WIFI_NAME" ||
      String(FACTORY_WIFI_PASSWORD).length() == 0) {
    Serial.println(sensorLabel() + " | Factory inventory bootstrap skipped: factory Wi-Fi credentials are not configured.");
    return false;
  }

  Serial.println(sensorLabel() + " | Factory inventory bootstrap starting BEFORE BLE.");
  Serial.println(sensorLabel() + " | Temporary factory Wi-Fi: " + String(FACTORY_WIFI_SSID));
  Serial.println(sensorLabel() + " | Factory Wi-Fi will NOT be saved as operational Wi-Fi.");

  WiFi.disconnect(true);
  delay(250);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  delay(150);
  WiFi.begin(FACTORY_WIFI_SSID, FACTORY_WIFI_PASSWORD);

  unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < FACTORY_REGISTRATION_WIFI_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(
      sensorLabel() +
      " | Factory inventory bootstrap could not connect within " +
      String(FACTORY_REGISTRATION_WIFI_TIMEOUT_MS / 1000) +
      "s. Continuing to BLE setup."
    );
    WiFi.disconnect(true);
    delay(250);
    WiFi.mode(WIFI_STA);
    return false;
  }

  Serial.println(
    sensorLabel() +
    " | Factory inventory Wi-Fi connected | ip=" +
    WiFi.localIP().toString() +
    " | rssi=" + String(WiFi.RSSI())
  );

  pendingRegistrationReason = "factory_inventory_bootstrap";
  bool registered = registerNode();

  if (registered) {
    markInventoryRegistrationConfirmed();
    Serial.println(sensorLabel() + " | Factory inventory registration CONFIRMED by Render.");
  } else {
    Serial.println(sensorLabel() + " | Factory inventory registration NOT confirmed. BLE setup will continue normally.");
  }

  // Tear the temporary factory connection down before BLE starts.
  nodeRegistered = false;
  v2WifiConnectedSince = 0;
  v2LastMqttConnectAttempt = 0;
  WiFi.disconnect(true);
  delay(350);
  WiFi.mode(WIFI_STA);
  delay(150);

  Serial.println(sensorLabel() + " | Factory inventory bootstrap finished. Starting normal BLE commissioning path.");
  return registered;
}


// MARK: - Good Shepherd V2 MQTT Transport
String v2StatusTopic() { return String("good-shepherd/v2/nodes/") + nodeId + "/status"; }
String v2EventsTopic() { return String("good-shepherd/v2/nodes/") + nodeId + "/events"; }
String v2CommandsTopic() { return String("good-shepherd/v2/nodes/") + nodeId + "/commands"; }
String v2ResultsTopic() { return String("good-shepherd/v2/nodes/") + nodeId + "/results"; }

String v2ClientId() {
  String chip = nodeId;
  chip.replace("esp32-", "");
  if (chip.length() > 12) chip = chip.substring(chip.length() - 12);
  return "gsv2-" + chip;
}

String v2StatusPayload(bool online) {
  generateHardwareIds();
  String p = "{";
  p += "\"protocolVersion\":\"2.0\",";
  p += "\"nodeId\":\"" + jsonEscape(nodeId) + "\",";
  p += "\"nodeName\":\"" + jsonEscape(deviceName + " - " + roomName) + "\",";
  p += "\"deviceName\":\"" + jsonEscape(deviceName) + "\",";
  p += "\"locationName\":\"" + jsonEscape(locationName) + "\",";
  p += "\"residentName\":\"" + jsonEscape(residentName) + "\",";
  p += "\"roomName\":\"" + jsonEscape(roomName) + "\",";
  p += "\"setupId\":\"" + jsonEscape(setupId) + "\",";
  p += "\"assignmentState\":\"" + jsonEscape(assignmentState()) + "\",";
  p += "\"softwareVersion\":\"" + jsonEscape(String(SOFTWARE_VERSION)) + "\",";
  p += "\"sensorMode\":\"" + jsonEscape(sensorMode) + "\",";
  p += "\"sourceKey\":\"" + jsonEscape(sourceKey) + "\",";
  p += "\"localIp\":\"" + WiFi.localIP().toString() + "\",";
  p += "\"wifiSsid\":\"" + jsonEscape(WiFi.SSID()) + "\",";
  p += "\"wifiRssi\":" + String(WiFi.RSSI()) + ",";
  p += "\"uptimeSeconds\":" + String(millis()/1000) + ",";
  p += "\"online\":" + String(online ? "true" : "false");
  p += "}";
  return p;
}

String v2LastWillPayload() {
  // Keep the CONNECT packet small. The server only needs nodeId + online=false
  // to mark an unexpected disconnect offline immediately.
  String p = "{";
  p += "\"protocolVersion\":\"2.0\",";
  p += "\"nodeId\":\"" + jsonEscape(nodeId) + "\",";
  p += "\"online\":false";
  p += "}";
  return p;
}

void v2MqttCallback(char* topic, byte* payload, unsigned int length) {
  String body;
  body.reserve(length + 1);

  for (unsigned int i = 0; i < length; i++) {
    body += (char)payload[i];
  }

  String commandId = extractJsonString(body, "commandId");
  String commandType = extractJsonString(body, "commandType");

  if (commandId.length() == 0 || commandType.length() == 0) {
    logSensor("V2 MQTT command ignored | reason=missing_command_id_or_type");
    return;
  }

  // Never run a long command from inside the PubSubClient callback. Hand it
  // back to the normal firmware loop so identify/reboot/reset/OTA cannot
  // corrupt the MQTT receive stack.
  if (pendingMqttCommand) {
    logSensor("V2 MQTT command deferred/dropped | reason=command_already_pending | commandId=" + commandId);
    return;
  }

  pendingMqttCommandPayload = body;
  pendingMqttCommand = true;

  logSensor(
    "V2 MQTT command received | commandId=" + commandId +
    " | commandType=" + commandType +
    " | bytes=" + String(length)
  );
}

void v2MqttConfigureOnce() {
  if (v2MqttConfigured) return;

  // This is intentionally the same TLS/MQTT client pattern that succeeded in
  // good_shepherd_minimal_hivemq_test.ino.
  v2MqttTlsClient.setInsecure();
  v2MqttTlsClient.setTimeout(12000);

  v2MqttClient.setServer(MQTT_HOST, MQTT_PORT);
  v2MqttClient.setKeepAlive(MQTT_KEEPALIVE_SECONDS);
  v2MqttClient.setSocketTimeout(8);
  v2MqttClient.setBufferSize(MQTT_BUFFER_SIZE);
  v2MqttClient.setCallback(v2MqttCallback);

  v2MqttConfigured = true;

  logSensor(
    "V2 MQTT proven transport configured | host=" +
    String(MQTT_HOST) +
    " | port=" + String(MQTT_PORT)
  );
}

bool v2MqttConnect() {
  if (WiFi.status() != WL_CONNECTED) {
    v2WifiConnectedSince = 0;
    nodeRegistered = false;
    return false;
  }

  if (v2WifiConnectedSince == 0) {
    v2WifiConnectedSince = millis();
    logSensor("V2 MQTT waiting for Wi-Fi stack to settle.");
    return false;
  }

  if (millis() - v2WifiConnectedSince < MQTT_WIFI_SETTLE_MS) {
    return false;
  }

  v2MqttConfigureOnce();

  if (v2MqttClient.connected()) {
    nodeRegistered = true;
    return true;
  }

  unsigned long now = millis();
  if (v2LastMqttConnectAttempt != 0 &&
      now - v2LastMqttConnectAttempt < MQTT_RECONNECT_INTERVAL_MS) {
    return false;
  }

  v2LastMqttConnectAttempt = now;

  String clientId = v2ClientId();

  logSensor(
    "V2 MQTT connect attempt | clientId=" + clientId +
    " | freeHeap=" + String(ESP.getFreeHeap()) +
    " | minFreeHeap=" + String(ESP.getMinFreeHeap())
  );

  // Exact behavior from the successful standalone test.
  v2MqttTlsClient.stop();
  v2MqttTlsClient.setInsecure();
  v2MqttTlsClient.setTimeout(12000);

  String willTopic = v2StatusTopic();
  String willPayload = v2LastWillPayload();

  logSensor(
    "V2 MQTT LWT armed | topic=" + willTopic +
    " | qos=1 | retained=true | bytes=" + String(willPayload.length()) +
    " | keepAlive=" + String(MQTT_KEEPALIVE_SECONDS)
  );

  bool ok = v2MqttClient.connect(
    clientId.c_str(),
    MQTT_USERNAME,
    MQTT_PASSWORD,
    willTopic.c_str(),
    1,
    true,
    willPayload.c_str()
  );

  logSensor(
    String("V2 MQTT connect result | ok=") +
    (ok ? "true" : "false") +
    " | state=" + String(v2MqttClient.state())
  );

  if (!ok) {
    nodeRegistered = false;
    return false;
  }

  bool subscribed = v2MqttClient.subscribe(v2CommandsTopic().c_str(), 1);
  bool statusPublished = v2PublishStatus(true);

  nodeRegistered = true;

  logSensor(
    String("V2 MQTT connected | subscribe=") +
    (subscribed ? "ok" : "failed") +
    " | retainedStatus=" +
    (statusPublished ? "ok" : "failed") +
    " | lwt=armed" +
    " | buffer=" + String(MQTT_BUFFER_SIZE)
  );

  return true;
}

void v2MqttHandle() {
  if (WiFi.status() != WL_CONNECTED) {
    nodeRegistered = false;
    v2WifiConnectedSince = 0;
    return;
  }

  if (v2WifiConnectedSince == 0) {
    v2WifiConnectedSince = millis();
  }

  v2MqttConfigureOnce();

  if (!v2MqttClient.connected()) {
    nodeRegistered = false;
    v2MqttConnect();
    return;
  }

  bool loopOk = v2MqttClient.loop();

  if (!loopOk || !v2MqttClient.connected()) {
    nodeRegistered = false;
    logSensor(
      "V2 MQTT session lost | state=" +
      String(v2MqttClient.state())
    );
    return;
  }

  nodeRegistered = true;

  if (v2LastStatusPublish == 0 ||
      millis() - v2LastStatusPublish >= MQTT_STATUS_PUBLISH_INTERVAL_MS) {
    v2PublishStatus(true);
  }
}

bool v2PublishStatus(bool online) {
  if (!v2MqttClient.connected()) {
    logSensor("V2 MQTT status publish skipped | reason=not_connected");
    return false;
  }

  String topic = v2StatusTopic();
  String payload = v2StatusPayload(online);

  logSensor(
    "V2 MQTT status publish attempt | bytes=" +
    String(payload.length()) +
    " | buffer=" + String(MQTT_BUFFER_SIZE) +
    " | topic=" + topic
  );

  bool ok = v2MqttClient.publish(
    topic.c_str(),
    payload.c_str(),
    true
  );

  if (!ok) {
    logSensor(
      "V2 MQTT status publish failed | state=" +
      String(v2MqttClient.state()) +
      " | connected=" +
      String(v2MqttClient.connected() ? "true" : "false")
    );

    // Give PubSubClient one loop pass, then retry exactly once.
    if (v2MqttClient.connected()) {
      v2MqttClient.loop();
      delay(25);

      ok = v2MqttClient.publish(
        topic.c_str(),
        payload.c_str(),
        true
      );

      logSensor(
        String("V2 MQTT status publish retry | result=") +
        (ok ? "ok" : "failed") +
        " | state=" + String(v2MqttClient.state())
      );
    }
  }

  if (ok && online) {
    v2LastStatusPublish = millis();
    lastHeartbeatAttemptTime = millis();
    lastHeartbeatSuccessTime = millis();
    lastServerSuccessTime = millis();
  }

  return ok;
}

bool v2PublishEvent(const String& payload) {
  if (!v2MqttClient.connected()) {
    logSensor("V2 MQTT event publish skipped | reason=not_connected");
    return false;
  }

  String topic = v2EventsTopic();

  logSensor(
    "V2 MQTT event publish attempt | bytes=" +
    String(payload.length()) +
    " | buffer=" + String(MQTT_BUFFER_SIZE) +
    " | topic=" + topic
  );

  bool ok = v2MqttClient.publish(
    topic.c_str(),
    payload.c_str(),
    false
  );

  if (!ok) {
    logSensor(
      "V2 MQTT event publish failed | state=" +
      String(v2MqttClient.state()) +
      " | connected=" +
      String(v2MqttClient.connected() ? "true" : "false")
    );
  }

  if (ok) {
    lastServerSuccessTime = millis();
  }

  return ok;
}

bool v2PublishCommandResult(String commandId, String commandType, String status, String message) {
  if (!v2MqttClient.connected()) {
    logSensor(
      "V2 MQTT command result skipped | reason=not_connected | commandId=" + commandId +
      " | status=" + status
    );
    return false;
  }

  String payload = "{";
  payload += "\"protocolVersion\":\"2.0\",";
  payload += "\"nodeId\":\"" + jsonEscape(nodeId) + "\",";
  payload += "\"commandId\":\"" + jsonEscape(commandId) + "\",";
  payload += "\"commandType\":\"" + jsonEscape(commandType) + "\",";
  payload += "\"status\":\"" + jsonEscape(status) + "\",";
  payload += "\"message\":\"" + jsonEscape(message) + "\"";
  payload += "}";

  bool ok = v2MqttClient.publish(v2ResultsTopic().c_str(), payload.c_str(), false);
  if (ok) {
    v2MqttClient.loop();
    lastServerSuccessTime = millis();
  }

  logSensor(
    "V2 MQTT command result | commandId=" + commandId +
    " | commandType=" + commandType +
    " | status=" + status +
    " | result=" + String(ok ? "published" : "failed")
  );
  return ok;
}

void handlePendingMqttCommand() {
  if (!pendingMqttCommand) return;

  String payload = pendingMqttCommandPayload;
  pendingMqttCommandPayload = "";
  pendingMqttCommand = false;

  executeMqttCommand(payload);
}

void executeMqttCommand(String payload) {
  String commandId = extractJsonString(payload, "commandId");
  String commandType = extractJsonString(payload, "commandType");
  commandType.trim();
  commandType.toLowerCase();

  if (commandId.length() == 0 || commandType.length() == 0) {
    logSensor("V2 MQTT command execution rejected | missing commandId/commandType");
    return;
  }

  logSensor("Executing V2 MQTT command | commandId=" + commandId + " | commandType=" + commandType);

  // The server marks the durable row running before it publishes. Echoing
  // running here confirms that the physical ESP32 actually received it.
  v2PublishCommandResult(commandId, commandType, "running", "ESP32 received command and started execution.");

  if (commandType == "ping") {
    v2PublishCommandResult(commandId, commandType, "success", "Ping received by ESP32 over MQTT.");
    return;
  }

  if (commandType == "identify" || commandType == "locate") {
    blinkIdentifyLed(12000);
    v2PublishCommandResult(commandId, commandType, "success", "Identify LED completed.");
    return;
  }

  if (commandType == "reboot") {
    v2PublishCommandResult(commandId, commandType, "success", "Reboot command accepted. ESP32 restarting.");
    v2MqttClient.loop();
    delay(500);
    ESP.restart();
    return;
  }

  if (commandType == "reconfigure") {
    v2PublishCommandResult(commandId, commandType, "success", "Reconfigure command accepted. Assignment will be cleared and ESP32 restarted.");
    v2MqttClient.loop();
    delay(500);
    clearAssignmentSettingsOnly();
    delay(250);
    ESP.restart();
    return;
  }

  if (commandType == "factory_reset") {
    v2PublishCommandResult(commandId, commandType, "success", "Factory reset command accepted. ESP32 restarting.");
    v2MqttClient.loop();
    delay(500);
    factoryClearEverything();
    delay(250);
    ESP.restart();
    return;
  }

  if (commandType == "update_firmware") {
    String firmwareUrl = extractJsonString(payload, "firmwareUrl");
    if (firmwareUrl.length() == 0) {
      v2PublishCommandResult(commandId, commandType, "failed", "Firmware command is missing firmwareUrl.");
      return;
    }

    v2PublishCommandResult(commandId, commandType, "running", "Firmware download is starting.");
    v2MqttClient.loop();
    delay(150);

    // OTA deliberately shuts MQTT down to free TLS/radio resources.
    // Terminal OTA result handling is finalized separately after this
    // architecture-size gate is validated.
    performFirmwareUpdate(commandId, firmwareUrl);
    return;
  }

  v2PublishCommandResult(commandId, commandType, "failed", "Unsupported MQTT command type: " + commandType);
}

void v2MqttShutdown() {
  if (v2MqttClient.connected()) {
    // Graceful shutdown publishes offline explicitly. The broker LWT is
    // reserved for unexpected power/network loss.
    v2PublishStatus(false);
    delay(100);
    v2MqttClient.disconnect();
  }

  v2MqttTlsClient.stop();

  nodeRegistered = false;
  v2LastStatusPublish = 0;
  v2LastMqttConnectAttempt = 0;
  v2WifiConnectedSince = 0;

  logSensor("V2 MQTT session shut down gracefully | explicitOffline=sent_if_connected");
}

// MARK: - Arduino
void setup() {
  Serial.begin(115200); delay(1000);
  // Diagnostic observer only. It does not initiate reconnects or mutate
  // connectivity state.
  WiFi.onEvent(handleWifiDiagnosticEvent);
  pinMode(PIR_PIN, INPUT); pinMode(LED_PIN, OUTPUT);
  setLed(false);
  Serial.println();
  Serial.println("Good Shepherd ESP32 V2.0.10 Factory Registration Bootstrap");
  generateHardwareIds();
  ensureSetupId();
  loadSettings();

  // Registration bootstrap is isolated from commissioning. It runs before BLE,
  // never overwrites wifiName/wifiPassword, and never blocks BLE if it fails.
  runFactoryInventoryRegistrationBootstrap();

  startBleManagement();
  lastServerSuccessTime = millis();

  bleBootSetupWindowActive = true;

  if (wifiName.length() > 0 && wifiPassword.length() > 0) {
    bleBootSetupWindowStartedAt = millis();
    Serial.println("BLE startup setup window active for 60 seconds.");
    Serial.println("Connect with iOS BLE Sensor Setup now to configure or reconfigure this sensor.");
    Serial.println("If nobody connects, saved Wi-Fi and MQTT runtime will start automatically.");
  } else {
    bleBootSetupWindowStartedAt = 0;
    Serial.println("No saved user Wi-Fi. BLE setup will remain available until configuration succeeds.");
    Serial.println("Commissioning V2: SoftAP fallback is disabled; configure this sensor through BLE.");
  }
}

void loop() {
  handleConnectivityDiagnosticTick();

  if (firmwareUpdateInProgress) {
    delay(100);
    return;
  }

  if (pendingBleCommand) {
    String command = pendingBleCommandPayload;
    pendingBleCommandPayload = "";
    pendingBleCommand = false;
    handleBleCommand(command);
  }

  // Keep local sensing and BLE setup/recovery alive.
  enforceBleServiceSessionTimeout();
  handleBleBootSetupWindow();
  handlePostBleWifiRecovery();

  // Saved Wi-Fi and MQTT intentionally remain idle during the startup BLE
  // window so BLE setup has maximum memory/radio headroom.
  if (!bleBootSetupWindowActive) {
    handleWifiReconnect();
  }

  if (WiFi.status() == WL_CONNECTED) {
    // V2 primary cloud transport. MQTT establishes/maintains nodeRegistered.
    if (!bleClientConnected && !blePostDisconnectHoldActive()) {
      v2MqttHandle();
      handlePendingMqttCommand();
    }

    if (nodeRegistered) {
      if (backgroundCloudAllowed()) {
        handleSelfHealing();
      }

      if (pendingFirmwareStatusCheck) {
        pendingFirmwareStatusCheck = false;
        logSensor("V2 firmware status check is server-side; ESP32 HTTPS check skipped.");
      }

      // Existing heartbeat scheduler now publishes the retained MQTT status
      // instead of opening a routine HTTPS /node-health request.
      if (bleClientConnected) {
        if (!bleClientCloudQuietActive() &&
            millis() - lastHeartbeatAttemptTime >= BLE_CONNECTED_BACKGROUND_HEARTBEAT_MS) {
          sendHeartbeat(false);
        }
      } else if (!blePostDisconnectHoldActive() &&
                 millis() - lastHeartbeatAttemptTime >= HEARTBEAT_INTERVAL_MS) {
        sendHeartbeat();
      }

      // MQTT Phase 2 owns ESP32 device controls. Legacy HTTP command polling remains disabled.
      if (modeUsesMotion()) {
        handleMotionSensor();
      }

}
  }

  if (bleStarted) {
    unsigned long statusInterval =
      bleClientConnected ? BLE_CONNECTED_STATUS_INTERVAL_MS : BLE_STATUS_INTERVAL_MS;

    if (millis() - lastBleStatusUpdateTime >= statusInterval) {
      lastBleStatusUpdateTime = millis();
      updateBleStatus();
    }
  }

  delay(25);
}

// MARK: - Wi-Fi
void startSetupMode() {
  setupModeStarted = false;
  lastWifiStatusText =
    "BLE commissioning required. Legacy setup hotspot is disabled.";
  updateBleStatus();
}


void stopSetupMode() {
  setupModeStarted = false;
}


void connectToSavedWifi(bool allowSetupFallback) {
  (void)allowSetupFallback;

  String cleanSsid = wifiName;
  cleanSsid.trim();

  if (cleanSsid.length() == 0 || wifiPassword.length() == 0) {
    wifiConnectInProgress = false;
    lastWifiFailureReason = "Missing saved Wi-Fi SSID or password.";
    lastWifiStatusText = lastWifiFailureReason;
    publishBleResult("failed", lastWifiFailureReason);
    updateBleStatus();
    return;
  }

  wifiName = cleanSsid;

  // Commissioning V2 is BLE-only; legacy SoftAP/WebServer setup is removed.
  // compiled for rollback compatibility but is not entered by this path.
  if (setupModeStarted) {
    stopSetupMode();
  }

  wifiConnectInProgress = true;
  wifiConnectStartedAt = millis();
  lastReconnectAttempt = millis();
  lastWifiFailureReason = "";
  lastWifiReconnectReason = "commissioning_v2";
  lastWifiStatusText = "Connecting to Wi-Fi: " + wifiName;

  nodeRegistered = false;
  pendingRegistrationReason = "wifi_connecting";
  pendingFirmwareStatusCheck = false;

  Serial.println(sensorLabel() + " | Commissioning V2 Wi-Fi start | ssid=" + wifiName);

  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(wifiName.c_str(), wifiPassword.c_str());

  updateBleStatus();
  publishBleResult("running", lastWifiStatusText);

  // Intentionally return immediately. handleWifiReconnect() completes
  // connection state, BLE release, and MQTT eligibility from loop().
}

void connectToNewWifiFromBle(
  String ssid,
  String password,
  String newLocation,
  String newResident,
  String newRoom,
  String newDeviceName,
  String newSensorMode,
  bool shouldRestartAfterSave
) {
  ssid.trim();

  if (ssid.length() == 0 || password.length() == 0) {
    publishBleResult("failed", "Missing Wi-Fi SSID or password.");
    return;
  }

  String requestedMode = normalizedSensorMode(newSensorMode);

  // This firmware is dedicated motion hardware.
  if (requestedMode.length() > 0 && requestedMode != "motion") {
    publishBleResult("failed", "This device supports motion mode only.");
    return;
  }

  wifiName = ssid;
  wifiPassword = password;

  if (newLocation.length() > 0) locationName = newLocation;
  if (newResident.length() > 0) residentName = newResident;
  if (newRoom.length() > 0) roomName = newRoom;

  sensorMode = "motion";

  if (newDeviceName.length() > 0) {
    deviceName = newDeviceName;
  } else {
    deviceName = "Motion Sensor";
  }

  pendingRegistrationReason = "configuration_change";

  saveSettings();

  publishBleResult("success", "saved");
  lastWifiStatusText = "Wi-Fi configuration saved for " + wifiName + ".";
  updateBleStatus();

  if (shouldRestartAfterSave) {
    delay(500);
    ESP.restart();
    return;
  }

  // End the boot commissioning hold immediately after a valid BLE save.
  // BLE remains alive while Wi-Fi begins asynchronously. Once Wi-Fi is
  // connected, handleWifiReconnect() releases BLE and enables MQTT runtime.
  bleBootSetupWindowActive = false;
  bleBootSetupWindowStartedAt = 0;

  connectToSavedWifi(false);
}

void handleWifiReconnect() {
  if (wifiName.length() == 0 ||
      firmwareUpdateInProgress ||
      setupModeStarted) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    bool wasConnecting = wifiConnectInProgress;
    bool recoveredFromRetry =
      lastWifiFailureReason.length() > 0 ||
      lastWifiReconnectReason.length() > 0;

    wifiConnectInProgress = false;

    lastConnectedIp = WiFi.localIP().toString();
    lastConnectedSsid = WiFi.SSID();
    lastWifiFailureReason = "";
    lastWifiStatusText =
      "Wi-Fi connected: " + lastConnectedSsid + " / " + lastConnectedIp;

    if (wasConnecting || recoveredFromRetry) {
      Serial.println(
        sensorLabel() +
        " | Wi-Fi connected | ssid=" +
        lastConnectedSsid +
        " | ip=" +
        lastConnectedIp
      );

      pendingRegistrationReason = "wifi_connected";
      nodeRegistered = false;

      updateBleStatus();
      publishBleResult(
        "success",
        "Wi-Fi connected: " + lastConnectedIp +
        ". Switching to normal runtime."
      );
    }

    // Match the proven commissioning architecture: BLE is released only
    // after operational Wi-Fi exists.
    if (bleStarted && !bleReleasedForRuntime) {
      stopBleForNormalRuntime();
    }

    if (v2WifiConnectedSince == 0 || wasConnecting || recoveredFromRetry) {
      v2WifiConnectedSince = millis();
      v2LastMqttConnectAttempt = 0;
    }

    lastWifiReconnectReason = "";
    return;
  }

  // Connection attempts are asynchronous. Do not block the Arduino loop.
  if (wifiConnectInProgress) {
    unsigned long elapsed = millis() - wifiConnectStartedAt;

    // Give each station attempt 15 seconds. Failure does not enter SoftAP.
    if (elapsed < 15000UL) {
      return;
    }

    wifiConnectInProgress = false;
    nodeRegistered = false;

    lastWifiFailureReason =
      "Wi-Fi connection not established yet. Automatic retry will continue.";

    lastWifiStatusText = lastWifiFailureReason;
    lastWifiReconnectReason = "commissioning_retry";

    Serial.println(
      sensorLabel() +
      " | Commissioning V2 Wi-Fi attempt timed out; BLE-only recovery remains available."
    );

    publishBleResult("running", lastWifiFailureReason);
    updateBleStatus();

    lastReconnectAttempt = millis();
    return;
  }

  // Retry indefinitely without switching to the legacy setup hotspot.
  if (millis() - lastReconnectAttempt >= RECONNECT_INTERVAL_MS) {
    lastReconnectAttempt = millis();
    wifiConnectInProgress = true;
    wifiConnectStartedAt = millis();

    nodeRegistered = false;
    pendingRegistrationReason = "wifi_retry";
    lastWifiReconnectReason = "wifi_retry";

    lastWifiStatusText =
      "Retrying Wi-Fi connection to " + wifiName + ".";

    Serial.println(
      sensorLabel() +
      " | Commissioning V2 Wi-Fi retry | ssid=" +
      wifiName
    );

    WiFi.disconnect(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(wifiName.c_str(), wifiPassword.c_str());

    updateBleStatus();
  }
}

// MARK: - Render API
bool registerNode() {
  if (WiFi.status() != WL_CONNECTED) return false;
  generateHardwareIds(); logRegistrationAttempt();
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(HTTP_TIMEOUT_MS); http.setReuse(false); http.begin(client, REGISTER_URL); http.addHeader("Content-Type", "application/json"); http.addHeader("x-webhook-secret", WEBHOOK_SECRET);
  String payload = "{";
  payload += "\"nodeId\":\"" + nodeId + "\",";
  payload += "\"nodeName\":\"" + jsonEscape(deviceName + " - " + roomName) + "\",";
  payload += "\"locationName\":\"" + jsonEscape(locationName) + "\",";
  payload += "\"localIp\":\"" + WiFi.localIP().toString() + "\",";
  payload += "\"localConfigPort\":80,";
  payload += "\"cameraCount\":0,";
  payload += "\"cameraSummary\":[],";
  payload += "\"setupId\":\"" + setupId + "\",";
  payload += "\"assignmentState\":\"" + assignmentState() + "\",";
  payload += "\"softwareVersion\":\"" + String(SOFTWARE_VERSION) + "\",";
  payload += "\"sensorMode\":\"" + jsonEscape(sensorMode) + "\",";
  payload += "\"sourceKey\":\"" + jsonEscape(sourceKey) + "\",";
  payload += "}";
  int responseCode = http.POST(payload); Serial.print(sensorLabel() + " | Registration response code: "); Serial.println(responseCode);
  http.end(); client.stop(); delay(50);
  if (responseCode >= 200 && responseCode < 300) { markServerSuccess("registerNode"); pendingRegistrationReason = "registered"; return true; }
  pendingRegistrationReason = "registration_retry"; markSetupCloudFailure("registerNode", responseCode); return false;
}



bool sendHeartbeat(bool criticalFailure) {
  lastHeartbeatAttemptTime = millis();
  bool ok = v2PublishStatus(true);
  if (ok) {
    markServerSuccess("v2MqttStatus");
    lastLiteHeartbeatReturnCode = 200;
    lastLiteHeartbeatCategory = "mqtt_retained_status";
    logHeartbeatRecoveryState("mqtt_status_success");
    return true;
  }
  if (criticalFailure) markServerFailure("v2MqttStatus", -1);
  lastLiteHeartbeatReturnCode = -1;
  lastLiteHeartbeatCategory = "mqtt_publish_failed";
  return false;
}






// MARK: - Firmware
bool checkLatestFirmwareStatusOnly() {
  String message = "V2 firmware status is server-side. Running " + String(SOFTWARE_VERSION) + ".";
  logSensor(message);
  publishBleResult("success", message);
  return true;
}

String httpErrorDescription(HTTPClient& http, int responseCode) { return responseCode > 0 ? "HTTP " + String(responseCode) : "HTTP " + String(responseCode) + " (" + http.errorToString(responseCode) + ")"; }

void stopBleForNormalRuntime() {
  if (!bleStarted || bleReleasedForRuntime) return;
  if (bleClientConnected) return;

  bleBootSetupWindowActive = false;
  bleBootSetupWindowStartedAt = 0;

  Serial.println(
    sensorLabel() +
    " | Releasing NimBLE before MQTT TLS runtime."
  );

  bleShutdownInProgress = true;
  pendingBleWifiRecovery = false;

  NimBLEDevice::stopAdvertising();
  NimBLEDevice::deinit(true);

  bleServer = nullptr;
  bleStatusCharacteristic = nullptr;
  bleCommandCharacteristic = nullptr;
  bleResultCharacteristic = nullptr;

  bleStarted = false;
  bleShutdownInProgress = false;
  bleReleasedForRuntime = true;
}


void stopBleForFirmwareUpdate() {
  if (!bleStarted) return;

  bleBootSetupWindowActive = false;
  bleBootSetupWindowStartedAt = 0;

  bleShutdownInProgress = true;

  NimBLEDevice::stopAdvertising();
  NimBLEDevice::deinit(true);

  bleServer = nullptr;
  bleStatusCharacteristic = nullptr;
  bleCommandCharacteristic = nullptr;
  bleResultCharacteristic = nullptr;

  bleClientConnected = false;
  bleStarted = false;
  bleReleasedForRuntime = true;
  bleShutdownInProgress = false;
}


void finishFirmwareUpdateFailure(String commandId, String message) {
  Serial.println(sensorLabel() + " | " + message);
  firmwareUpdateInProgress = false;

  bool mqttRecovered = false;

  if (WiFi.status() == WL_CONNECTED) {
    v2WifiConnectedSince = millis();
    v2LastMqttConnectAttempt = 0;
    mqttRecovered = v2MqttConnect();
  }

  if (commandId.length() > 0 && mqttRecovered) {
    v2PublishCommandResult(
      commandId,
      "update_firmware",
      "failed",
      message
    );
    v2MqttClient.loop();
    delay(150);
  }

  Serial.println(sensorLabel() + " | OTA failure recovery returned to MQTT runtime.");
}

void performFirmwareUpdate(String commandId, String firmwareUrl) {
  if (WiFi.status() != WL_CONNECTED) { finishFirmwareUpdateFailure(commandId, "Firmware update failed: Wi-Fi offline."); return; }
  v2MqttShutdown();
  firmwareUpdateInProgress = true;
  logSensor("Firmware update requested."); logSensor("Firmware URL: " + firmwareUrl);
  Serial.print(sensorLabel() + " | Free heap before OTA prep: "); Serial.println(ESP.getFreeHeap());
  if (setupModeStarted) {
    setupModeStarted = false;
  }
  stopBleForFirmwareUpdate();
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); delay(750);
  if (WiFi.status() != WL_CONNECTED) { finishFirmwareUpdateFailure(commandId, "Firmware update failed: Wi-Fi disconnected before download."); return; }
  Serial.print(sensorLabel() + " | Free heap before HTTPS begin: "); Serial.println(ESP.getFreeHeap());
  WiFiClientSecure client; client.setInsecure(); client.setTimeout(120000);
  HTTPClient http; http.setTimeout(180000); http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); http.setReuse(false);
  if (!http.begin(client, firmwareUrl)) { finishFirmwareUpdateFailure(commandId, "Firmware update failed: could not open firmware URL."); return; }
  http.addHeader("User-Agent", "GoodShepherd-ESP32-OTA/1.9.5");
  int responseCode = http.GET(); Serial.print(sensorLabel() + " | Firmware HTTP response: "); Serial.println(httpErrorDescription(http, responseCode));
  if (responseCode != HTTP_CODE_OK) { String message = "Firmware update failed: download returned " + httpErrorDescription(http, responseCode) + "."; http.end(); client.stop(); finishFirmwareUpdateFailure(commandId, message); return; }
  int contentLength = http.getSize();
  if (contentLength == 0) { http.end(); client.stop(); finishFirmwareUpdateFailure(commandId, "Firmware update failed: firmware file was empty."); return; }
  Serial.print(sensorLabel() + " | Firmware size: "); Serial.println(contentLength);
  Serial.print(sensorLabel() + " | Free heap before Update.begin: "); Serial.println(ESP.getFreeHeap());
  bool canBegin = contentLength > 0 ? Update.begin(contentLength) : Update.begin(UPDATE_SIZE_UNKNOWN);
  if (!canBegin) { String message = "Firmware update failed: not enough OTA space. Error " + String(Update.getError()) + "."; http.end(); client.stop(); finishFirmwareUpdateFailure(commandId, message); return; }
  WiFiClient* stream = http.getStreamPtr(); size_t written = Update.writeStream(*stream);
  Serial.print(sensorLabel() + " | Firmware bytes written: "); Serial.println(written);
  if (contentLength > 0 && written != (size_t)contentLength) { String message = "Firmware update failed: wrote " + String(written) + " of " + String(contentLength) + " bytes."; Update.abort(); http.end(); client.stop(); finishFirmwareUpdateFailure(commandId, message); return; }
  if (!Update.end()) { String message = "Firmware update failed during finalization. Error " + String(Update.getError()) + "."; http.end(); client.stop(); finishFirmwareUpdateFailure(commandId, message); return; }
  if (!Update.isFinished()) { http.end(); client.stop(); finishFirmwareUpdateFailure(commandId, "Firmware update failed: update did not finish."); return; }
  http.end();
  client.stop();

  Serial.println(sensorLabel() + " | Firmware update complete.");

  if (commandId.length() > 0 && WiFi.status() == WL_CONNECTED) {
    v2WifiConnectedSince = millis();
    v2LastMqttConnectAttempt = 0;

    if (v2MqttConnect()) {
      v2PublishCommandResult(
        commandId,
        "update_firmware",
        "success",
        "Firmware update installed successfully. ESP32 is rebooting."
      );
      v2MqttClient.loop();
      delay(250);
    } else {
      Serial.println(
        sensorLabel() +
        " | OTA succeeded but MQTT terminal success acknowledgement could not reconnect before reboot."
      );
    }
  }

  Serial.println(sensorLabel() + " | Firmware update complete. Rebooting...");
  delay(1500);
  ESP.restart();
}

// MARK: - BLE Commands
void handleBleCommand(String payload) {
  String command = extractJsonString(payload, "command"); if (command.length() == 0) command = extractJsonString(payload, "action"); command.toLowerCase();
  if (command.length() == 0) { publishBleResult("failed", "Missing BLE command."); return; }
  markBleCommandActivity();
  logSensor("Handling BLE command: " + command);
  if (command == "get_status" || command == "status") { updateBleStatus(); publishBleResult("success", "Status refreshed. " + lastWifiStatusText); return; }
  if (command == "identify" || command == "locate") { publishBleResult("running", "Identify LED started."); blinkIdentifyLed(12000); publishBleResult("success", "Identify LED completed."); return; }
  if (command == "set_wifi" || command == "sync_wifi" || command == "sync_config") {
    String requestedSensorMode = extractJsonString(payload, "sensorMode"); if (requestedSensorMode.length() == 0) requestedSensorMode = extractJsonString(payload, "deviceType");
    connectToNewWifiFromBle(extractJsonString(payload, "ssid"), extractJsonString(payload, "password"), extractJsonString(payload, "locationName"), extractJsonString(payload, "residentName"), extractJsonString(payload, "roomName"), extractJsonString(payload, "deviceName"), requestedSensorMode, extractJsonBool(payload, "restartAfterSave", false)); return;
  }
  if (command == "set_sensor_mode") {
    String requestedSensorMode = extractJsonString(payload, "sensorMode"); String parsedSensorMode = normalizedSensorMode(requestedSensorMode);
    if (parsedSensorMode.length() == 0) { publishBleResult("failed", "Missing or invalid sensorMode."); return; }
    sensorMode = parsedSensorMode;
    String requestedDeviceName = extractJsonString(payload, "deviceName"); if (requestedDeviceName.length() > 0) deviceName = requestedDeviceName; else deviceName = defaultDeviceNameForMode(sensorMode);
    saveSettings(); publishBleResult("success", "Sensor mode saved: " + sensorMode + ". Restarting."); delay(750); ESP.restart(); return;
  }
  if (command == "check_firmware") {
    // During an active BLE session, do not make the ESP32 call Render for a
    // firmware check. The app can compare this current version against the
    // server release. Keep the BLE screen responsive and avoid BLE/HTTPS radio
    // contention.
    if (bleClientConnected) {
      updateBleStatus();
      publishBleResult("success", "Current firmware: " + String(SOFTWARE_VERSION) + ". Server check should be done by the app during BLE service.");
      return;
    }
    checkLatestFirmwareStatusOnly();
    return;
  }
  if (command == "force_update" || command == "update_firmware") { String firmwareUrl = extractJsonString(payload, "firmwareUrl"); if (firmwareUrl.length() == 0) { publishBleResult("failed", "BLE firmware update requires firmwareUrl. Auto-install from latest is disabled."); return; } publishBleResult("running", "Firmware update starting from BLE URL."); performFirmwareUpdate("", firmwareUrl); return; }
  if (command == "restart" || command == "reboot") { publishBleResult("success", "Restarting sensor."); delay(750); ESP.restart(); return; }
  if (command == "reconfigure") { publishBleResult("success", "Clearing assignment and restarting."); clearAssignmentSettingsOnly(); delay(750); ESP.restart(); return; }
  if (command == "factory_reset") { publishBleResult("success", "Factory reset started."); factoryClearEverything(); delay(750); ESP.restart(); return; }
  if (command == "ping") { publishBleResult("success", "Ping received over BLE."); return; }
  publishBleResult("failed", "Unknown BLE command: " + command);
}

// MARK: - Motion
void handleMotionSensor() {
  int motion = digitalRead(PIR_PIN);
  if (motion != lastPirState) { lastPirState = motion; logSensor("PIR changed to " + String(motion)); }
  if (motion == MOTION_ACTIVE_STATE) {
    if (motionHeldActiveStartedAt == 0) motionHeldActiveStartedAt = millis();
    else if (motionAlreadyReported && millis() - motionHeldActiveStartedAt >= MOTION_HELD_ACTIVE_RESET_MS) { logSensor("Motion held active too long. Resetting reported state so future activity is not suppressed."); motionAlreadyReported = false; motionHeldActiveStartedAt = millis(); }
  } else motionHeldActiveStartedAt = 0;
  if (motion == MOTION_ACTIVE_STATE && !motionAlreadyReported) pendingMotionDetectedEvent = true;
  if (pendingMotionDetectedEvent && !motionAlreadyReported) {
    if (lastMotionWebhookAttemptTime == 0 || millis() - lastMotionWebhookAttemptTime >= MOTION_RETRY_INTERVAL_MS) {
      if (bleClientConnected) {
        lastMotionWebhookAttemptTime = millis();
        logSensor("Motion detected while BLE client is connected. Cloud event will wait until BLE disconnects.");
        return;
      }
      lastMotionWebhookAttemptTime = millis(); logSensor("Motion detected. Sending webhook...");
      if (sendMotionEvent()) { pendingMotionDetectedEvent = false; motionAlreadyReported = true; lastMotionWebhookSuccessTime = millis(); lastNoMotionTime = 0; } else logSensor("Motion webhook failed. Will retry the retained event.");
    }
  }
  if (motion != MOTION_ACTIVE_STATE && motionAlreadyReported) {
    if (lastNoMotionTime == 0) lastNoMotionTime = millis();
    if (millis() - lastNoMotionTime > RESET_AFTER_NO_MOTION_MS) { logSensor("Motion reset."); motionAlreadyReported = false; lastNoMotionTime = 0; }
  }
  if (motion == MOTION_ACTIVE_STATE) lastNoMotionTime = 0;
}

bool sendMotionEvent() {
  if (WiFi.status() != WL_CONNECTED || !v2MqttClient.connected()) return false;
  generateHardwareIds();
  String fullSourceName = deviceName + " - " + roomName;
  String message = "Motion detected from " + fullSourceName;
  String payload = "{";
  payload += "\"protocolVersion\":\"2.0\",";
  payload += "\"nodeId\":\"" + nodeId + "\",";
  payload += "\"locationName\":\"" + jsonEscape(locationName) + "\",";
  payload += "\"sourceKey\":\"" + sourceKey + "\",";
  payload += "\"sourceName\":\"" + jsonEscape(fullSourceName) + "\",";
  payload += "\"residentName\":\"" + jsonEscape(residentName) + "\",";
  payload += "\"message\":\"" + jsonEscape(message) + "\",";
  payload += "\"alertLevel\":\"Normal\",\"timeText\":\"ESP32 Motion Event\",\"sensorMode\":\"" + jsonEscape(sensorMode) + "\",\"sensorType\":\"motion\",\"source\":\"pir\",\"eventType\":\"motion_detected\",\"motion\":true}";
  bool ok = v2PublishEvent(payload);
  logSensor(String("V2 MQTT motion event publish: ") + (ok ? "ok" : "failed"));
  if (ok) markServerSuccess("v2MqttMotionEvent");
  return ok;
}



// MARK: - Fallback Web Setup


