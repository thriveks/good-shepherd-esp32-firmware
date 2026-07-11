/*
  Good Shepherd ESP32 Multi-Sensor Firmware
  Version: esp32-good-shepherd-v1.8.7-gpio-presence-only
  Date: 2026-07-11

  Supported sensor modes:
    motion            = PIR motion sensor on GPIO27
    human_presence    = LD2410 OUT pin on GPIO21 only
    motion_presence   = PIR GPIO27 + LD2410 OUT pin on GPIO21

  Wiring:
    PIR VCC      -> ESP32 3V3
    PIR GND      -> ESP32 GND
    PIR OUT      -> ESP32 GPIO27

    LD2410 VCC   -> ESP32 5V / VIN
    LD2410 GND   -> ESP32 GND
    LD2410 OUT   -> ESP32 GPIO21
    LD2410 TX    -> Not used in this firmware
    LD2410 RX    -> Not used in this firmware

  Notes:
    - LD2410 OUT uses INPUT_PULLDOWN on GPIO21.
    - v1.8.7 removes LD2410 UART parsing and continuous presence telemetry.
    - v1.8.7 keeps Human Presence as a simple, stable GPIO21 digital sensor.
    - presence_detected and presence_cleared remain priority events.
    - BLE, Wi-Fi, registration, heartbeat, OTA, motion, and AI event behavior are preserved.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define PIR_PIN 27
#define MOTION_ACTIVE_STATE HIGH

#define PRESENCE_PIN 21
#define PRESENCE_ACTIVE_STATE HIGH


#define LED_PIN 2
#define LED_ACTIVE_STATE HIGH
#define LED_INACTIVE_STATE LOW

WebServer server(80);
Preferences prefs;

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
const char* WEBHOOK_URL = "https://good-shepherd-server-j06f.onrender.com/webhook";
const char* REGISTER_URL = "https://good-shepherd-server-j06f.onrender.com/nodes/register";
const char* HEARTBEAT_URL = "https://good-shepherd-server-j06f.onrender.com/node-health";
const char* LATEST_FIRMWARE_URL = "https://good-shepherd-server-j06f.onrender.com/firmware/latest";
const char* WEBHOOK_SECRET = "7e9c767aa079423227163be90943d7d2";

const char* SOFTWARE_VERSION = "esp32-good-shepherd-v1.8.8-gpio-presence-command-poll-noncritical";

const char* BLE_SERVICE_UUID = "7d9f0001-2f4f-4c3a-8b2a-0b5f7f2a0001";
const char* BLE_STATUS_UUID  = "7d9f0002-2f4f-4c3a-8b2a-0b5f7f2a0001";
const char* BLE_COMMAND_UUID = "7d9f0003-2f4f-4c3a-8b2a-0b5f7f2a0001";
const char* BLE_RESULT_UUID  = "7d9f0004-2f4f-4c3a-8b2a-0b5f7f2a0001";

BLEServer* bleServer = nullptr;
BLECharacteristic* bleStatusCharacteristic = nullptr;
BLECharacteristic* bleCommandCharacteristic = nullptr;
BLECharacteristic* bleResultCharacteristic = nullptr;

bool setupModeStarted = false;
bool nodeRegistered = false;
bool motionAlreadyReported = false;
bool presenceAlreadyReported = false;
bool firmwareUpdateInProgress = false;
bool bleStarted = false;
bool bleClientConnected = false;
bool pendingBleCommand = false;
bool pendingFirmwareStatusCheck = false;
bool wifiConnectInProgress = false;
bool pendingPresenceDetectedEvent = false;
bool pendingPresenceClearedEvent = false;

String pendingBleCommandPayload = "";
String lastWifiStatusText = "Not checked yet.";
String lastWifiFailureReason = "";
String lastConnectedIp = "";
String lastConnectedSsid = "";

int lastPirState = -1;
int lastPresenceState = -1;

unsigned long lastNoMotionTime = 0;
unsigned long lastNoPresenceTime = 0;
unsigned long lastHeartbeatTime = 0;
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
unsigned long lastPresenceWebhookAttemptTime = 0;
unsigned long lastPresenceWebhookSuccessTime = 0;
unsigned long motionHeldActiveStartedAt = 0;
unsigned long presenceHeldActiveStartedAt = 0;

unsigned long lastPriorityHttpAttemptTime = 0;

int consecutiveServerFailureCount = 0;

const unsigned long RESET_AFTER_NO_MOTION_MS = 10000;
const unsigned long RESET_AFTER_NO_PRESENCE_MS = 10000;
const unsigned long HEARTBEAT_INTERVAL_MS = 60000;
const unsigned long RECONNECT_INTERVAL_MS = 10000;
const unsigned long COMMAND_POLL_INTERVAL_MS = 60000;
const unsigned long REGISTRATION_RETRY_INTERVAL_MS = 15000;
const unsigned long BLE_STATUS_INTERVAL_MS = 5000;
const unsigned long FIRMWARE_STATUS_CHECK_COOLDOWN_MS = 300000;
const unsigned long HTTP_TIMEOUT_MS = 10000;
const unsigned long SELF_HEAL_CHECK_INTERVAL_MS = 30000;
const unsigned long SERVER_SILENCE_REBOOT_MS = 1800000;
const unsigned long MOTION_RETRY_INTERVAL_MS = 15000;
const unsigned long PRESENCE_RETRY_INTERVAL_MS = 15000;
const unsigned long MOTION_HELD_ACTIVE_RESET_MS = 300000;
const unsigned long PRESENCE_HELD_ACTIVE_RESET_MS = 300000;
const int SERVER_FAILURE_RECONNECT_THRESHOLD = 5;
const int SERVER_FAILURE_REBOOT_THRESHOLD = 20;
const int MAX_SAVED_WIFI = 8;

// MARK: - Forward declarations
void startSetupMode();
void stopSetupMode();
void connectToSavedWifi();
void connectToNewWifiFromBle(String ssid, String password, String newLocation, String newResident, String newRoom, String newDeviceName, String newSensorMode, bool shouldRestartAfterSave);
void handleWifiReconnect();
bool registerNode();
bool sendHeartbeat();
void pollSensorCommands();
void reportCommandResult(String commandId, String status, String message);
bool checkLatestFirmwareStatusOnly();
void performFirmwareUpdate(String commandId, String firmwareUrl);
void stopBleForFirmwareUpdate();
void finishFirmwareUpdateFailure(String commandId, String message);
String httpErrorDescription(HTTPClient& http, int responseCode);
void handleBleCommand(String payload);
void handleMotionSensor();
void handlePresenceSensor();
bool sendMotionEvent();
bool sendPresenceStateEvent(bool presenceActive);
void markServerSuccess(String context);
void markServerFailure(String context, int responseCode);
void markSetupCloudFailure(String context, int responseCode);
void reconnectWifiNow(String reason);
void handleSelfHealing();
void handleRoot();
void handleSave();
void handleReset();
void handleFactoryReset();

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
  if (value == "human_presence" || value == "presence" || value == "presence_sensor" || value == "human_presence_sensor" || value == "ld2410" || value == "ld2410_presence" || value == "humanpresence") return "human_presence";
  if (value == "motion_presence" || value == "motion_presence_sensor" || value == "motion_plus_presence" || value == "motion+presence" || value == "combo") return "motion_presence";
  if (value == "motion" || value == "motion_sensor" || value == "pir" || value == "pir_motion") return "motion";
  return "";
}

bool isValidSensorMode(String value) { return normalizedSensorMode(value).length() > 0; }
String safeSensorModeOrMotion(String value) { String parsed = normalizedSensorMode(value); return parsed.length() > 0 ? parsed : "motion"; }

String defaultDeviceNameForMode(String mode) {
  mode = safeSensorModeOrMotion(mode);
  if (mode == "human_presence") return "Human Presence Sensor";
  if (mode == "motion_presence") return "Motion + Presence Sensor";
  return "Motion Sensor";
}

bool modeUsesMotion() { return sensorMode == "motion" || sensorMode == "motion_presence"; }
bool modeUsesPresence() { return sensorMode == "human_presence" || sensorMode == "motion_presence"; }
int activeMonitorCountForMode() { int count = 0; if (modeUsesMotion()) count++; if (modeUsesPresence()) count++; return count <= 0 ? 1 : count; }
String sourcePrefixForMode() { if (sensorMode == "human_presence") return "presence"; if (sensorMode == "motion_presence") return "motion-presence"; return "motion"; }

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

String htmlEscape(String value) { value.replace("&", "&amp;"); value.replace("<", "&lt;"); value.replace(">", "&gt;"); value.replace("\"", "&quot;"); value.replace("'", "&#39;"); return value; }
String jsEscape(String value) { value.replace("\\", "\\\\"); value.replace("'", "\\'"); value.replace("\"", "\\\""); value.replace("\n", "\\n"); value.replace("\r", ""); return value; }
String jsonEscape(String value) { value.replace("\\", "\\\\"); value.replace("\"", "\\\""); value.replace("\n", "\\n"); value.replace("\r", ""); return value; }
String wifiSsidKey(int index) { return "ssid" + String(index); }
String wifiPassKey(int index) { return "pass" + String(index); }
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

// MARK: - Wi-Fi Credential History
void rememberWifiCredential(String ssid, String password) {
  ssid.trim();
  if (ssid.length() == 0 || password.length() == 0) return;
  prefs.begin("gs-device", false);
  int existingIndex = -1;
  int emptyIndex = -1;
  for (int i = 0; i < MAX_SAVED_WIFI; i++) {
    String savedSsid = prefs.getString(wifiSsidKey(i).c_str(), "");
    if (savedSsid == ssid) { existingIndex = i; break; }
    if (savedSsid.length() == 0 && emptyIndex < 0) emptyIndex = i;
  }
  int targetIndex = existingIndex >= 0 ? existingIndex : emptyIndex;
  if (targetIndex < 0) targetIndex = 0;
  prefs.putString(wifiSsidKey(targetIndex).c_str(), ssid);
  prefs.putString(wifiPassKey(targetIndex).c_str(), password);
  prefs.end();
  Serial.println("Saved Wi-Fi credential for: " + ssid);
}

String savedWifiPasswordFor(String ssid) {
  ssid.trim();
  if (ssid.length() == 0) return "";
  prefs.begin("gs-device", true);
  String result = "";
  for (int i = 0; i < MAX_SAVED_WIFI; i++) {
    if (prefs.getString(wifiSsidKey(i).c_str(), "") == ssid) {
      result = prefs.getString(wifiPassKey(i).c_str(), "");
      break;
    }
  }
  prefs.end();
  return result;
}

String savedWifiJavaScriptMap() {
  prefs.begin("gs-device", true);
  String js = "{";
  bool first = true;
  for (int i = 0; i < MAX_SAVED_WIFI; i++) {
    String savedSsid = prefs.getString(wifiSsidKey(i).c_str(), "");
    String savedPass = prefs.getString(wifiPassKey(i).c_str(), "");
    if (savedSsid.length() > 0 && savedPass.length() > 0) {
      if (!first) js += ",";
      js += "\"" + jsEscape(savedSsid) + "\":\"" + jsEscape(savedPass) + "\"";
      first = false;
    }
  }
  js += "}";
  prefs.end();
  return js;
}

// MARK: - Human Presence GPIO Input
// LD2410 UART radar parsing was intentionally removed in v1.8.7.
// The LD2410 OUT pin on GPIO21 remains the source of presence_detected and presence_cleared events.

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
  String ip = "";
  String ssid = "";
  int rssi = 0;
  if (WiFi.status() == WL_CONNECTED) { ip = WiFi.localIP().toString(); ssid = WiFi.SSID(); rssi = WiFi.RSSI(); }
  int motionState = digitalRead(PIR_PIN);
  int presenceState = digitalRead(PRESENCE_PIN);
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
  payload += "\"presenceState\":" + String(presenceState == PRESENCE_ACTIVE_STATE ? "true" : "false") + ",";
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
  if (bleClientConnected) bleStatusCharacteristic->notify();
}

void blinkIdentifyLed(unsigned long durationMs) {
  unsigned long startedAt = millis();
  bool ledOn = false;
  logSensor("Identify LED started.");
  while (millis() - startedAt < durationMs) { ledOn = !ledOn; setLed(ledOn); delay(150); }
  setLed(false);
  logSensor("Identify LED finished.");
}

class GoodShepherdBleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) { bleClientConnected = true; Serial.println("BLE client connected."); updateBleStatus(); }
  void onDisconnect(BLEServer* server) { bleClientConnected = false; Serial.println("BLE client disconnected."); delay(250); BLEDevice::startAdvertising(); }
};

class GoodShepherdBleCommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) {
    String value = characteristic->getValue();
    if (value.length() == 0) return;
    pendingBleCommandPayload = value;
    pendingBleCommand = true;
    Serial.println("BLE command received: " + pendingBleCommandPayload);
  }
};

void startBleManagement() {
  if (bleStarted) return;
  generateHardwareIds();
  String bleName = "GoodShepherd-" + setupId;
  BLEDevice::init(bleName.c_str());
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new GoodShepherdBleServerCallbacks());
  BLEService* service = bleServer->createService(BLE_SERVICE_UUID);
  bleStatusCharacteristic = service->createCharacteristic(BLE_STATUS_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  bleStatusCharacteristic->addDescriptor(new BLE2902());
  bleCommandCharacteristic = service->createCharacteristic(BLE_COMMAND_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  bleCommandCharacteristic->setCallbacks(new GoodShepherdBleCommandCallbacks());
  bleResultCharacteristic = service->createCharacteristic(BLE_RESULT_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  bleResultCharacteristic->addDescriptor(new BLE2902());
  service->start();
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  updateBleStatus();
  BLEDevice::startAdvertising();
  bleStarted = true;
  Serial.println("BLE management started.");
  Serial.println("BLE name: " + bleName);
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
  prefs.begin("gs-device", false);
  prefs.remove("wifiName"); prefs.remove("wifiPass"); prefs.remove("location"); prefs.remove("resident"); prefs.remove("room"); prefs.remove("deviceName"); prefs.remove("sensorMode"); prefs.remove("nodeId"); prefs.remove("sourceKey");
  prefs.end();
  wifiName = ""; wifiPassword = ""; locationName = ""; residentName = ""; roomName = ""; sensorMode = "motion"; deviceName = defaultDeviceNameForMode(sensorMode);
  nodeRegistered = false; lastWifiFailureReason = ""; lastWifiStatusText = "Assignment cleared. No Wi-Fi configured."; lastConnectedIp = ""; lastConnectedSsid = "";
  generateHardwareIds();
  Serial.println("Assignment settings cleared. Saved Wi-Fi history was kept.");
}

void factoryClearEverything() {
  prefs.begin("gs-device", false); prefs.clear(); prefs.end();
  wifiName = ""; wifiPassword = ""; locationName = ""; residentName = ""; roomName = ""; sensorMode = "motion"; deviceName = defaultDeviceNameForMode(sensorMode);
  nodeRegistered = false; lastWifiFailureReason = ""; lastWifiStatusText = "Factory reset complete. No Wi-Fi configured."; lastConnectedIp = ""; lastConnectedSsid = "";
  generateHardwareIds(); ensureSetupId();
  Serial.println("Factory settings cleared, including saved Wi-Fi history. New setup ID generated.");
}

void clearSettingsAndRestart() { logSensor("Reconfigure requested. Clearing assignment settings and restarting..."); clearAssignmentSettingsOnly(); delay(1000); ESP.restart(); }

void markServerSuccess(String context) {
  lastServerSuccessTime = millis(); consecutiveServerFailureCount = 0; lastWifiFailureReason = "";
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

void reconnectWifiNow(String reason) {
  if (wifiName.length() == 0 || wifiPassword.length() == 0 || firmwareUpdateInProgress || wifiConnectInProgress) return;
  Serial.println(sensorLabel() + " | Self-heal Wi-Fi reconnect: " + reason);
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
  if (consecutiveServerFailureCount >= SERVER_FAILURE_REBOOT_THRESHOLD) { Serial.println(sensorLabel() + " | Rebooting after repeated server failures."); delay(500); ESP.restart(); }
  if (consecutiveServerFailureCount >= SERVER_FAILURE_RECONNECT_THRESHOLD) { reconnectWifiNow("server failures reached " + String(consecutiveServerFailureCount)); return; }
  if (lastServerSuccessTime > 0 && millis() - lastServerSuccessTime >= SERVER_SILENCE_REBOOT_MS) { Serial.println(sensorLabel() + " | Rebooting after prolonged server silence."); delay(500); ESP.restart(); }
}

// MARK: - Arduino
void setup() {
  Serial.begin(115200); delay(1000);
  pinMode(PIR_PIN, INPUT); pinMode(PRESENCE_PIN, INPUT_PULLDOWN); pinMode(LED_PIN, OUTPUT);
  setLed(false);
  Serial.println();
  Serial.println("Good Shepherd ESP32 Multi-Sensor v1.8.8 GPIO presence + noncritical command polling");
  generateHardwareIds(); ensureSetupId(); loadSettings();
  startBleManagement();
  lastServerSuccessTime = millis();
  if (wifiName.length() > 0) connectToSavedWifi(); else startSetupMode();
}

void loop() {
  if (firmwareUpdateInProgress) { delay(100); return; }
  if (setupModeStarted) server.handleClient();
  if (pendingBleCommand) { String command = pendingBleCommandPayload; pendingBleCommandPayload = ""; pendingBleCommand = false; handleBleCommand(command); }

  // Keep local sensing and BLE alive. Presence is read directly from GPIO21.
  handleWifiReconnect();

  if (WiFi.status() == WL_CONNECTED) {
    if (!nodeRegistered) {
      if (millis() - lastRegistrationAttemptTime >= REGISTRATION_RETRY_INTERVAL_MS) {
        lastRegistrationAttemptTime = millis();
        nodeRegistered = registerNode();
        if (nodeRegistered) {
          consecutiveServerFailureCount = 0;
          sendHeartbeat();
          lastHeartbeatTime = millis();
          logSensor("Sensor online.");
        } else {
          logSensor("Cloud registration pending, but Wi-Fi is connected. Will retry in loop.");
        }
      }
    } else {
      handleSelfHealing();

      if (pendingFirmwareStatusCheck) {
        pendingFirmwareStatusCheck = false;
        if (millis() - lastFirmwareStatusCheckTime >= FIRMWARE_STATUS_CHECK_COOLDOWN_MS || lastFirmwareStatusCheckTime == 0) {
          lastFirmwareStatusCheckTime = millis();
          checkLatestFirmwareStatusOnly();
        }
      }

      if (millis() - lastCommandPollTime >= COMMAND_POLL_INTERVAL_MS) {
        pollSensorCommands();
        lastCommandPollTime = millis();
      }
      if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL_MS) {
        sendHeartbeat();
        lastHeartbeatTime = millis();
      }
      if (modeUsesMotion()) handleMotionSensor();
      if (modeUsesPresence()) handlePresenceSensor();
    }
  }

  if (millis() - lastBleStatusUpdateTime >= BLE_STATUS_INTERVAL_MS) { lastBleStatusUpdateTime = millis(); updateBleStatus(); }
  delay(25);
}

// MARK: - Wi-Fi
void startSetupMode() {
  if (setupModeStarted) return;
  setupModeStarted = true; nodeRegistered = false; wifiConnectInProgress = false;
  WiFi.disconnect(true); delay(250); WiFi.mode(WIFI_AP); WiFi.softAP("GoodShepherd-Setup");
  lastWifiStatusText = "Setup hotspot active: GoodShepherd-Setup";
  server.on("/", HTTP_GET, handleRoot); server.on("/save", HTTP_POST, handleSave); server.on("/reset", HTTP_GET, handleReset); server.on("/factory-reset", HTTP_GET, handleFactoryReset); server.begin();
  Serial.println("Setup mode started."); Serial.println("Connect to Wi-Fi: GoodShepherd-Setup"); Serial.println("Open: http://192.168.4.1"); updateBleStatus();
}

void stopSetupMode() {
  if (!setupModeStarted) return;
  Serial.println("Stopping setup mode..."); server.close(); WiFi.softAPdisconnect(true); setupModeStarted = false; delay(250); updateBleStatus();
}

void connectToSavedWifi() {
  String cleanSsid = wifiName; cleanSsid.trim();
  if (cleanSsid.length() == 0 || wifiPassword.length() == 0) { lastWifiFailureReason = "Missing saved Wi-Fi SSID or password."; lastWifiStatusText = lastWifiFailureReason; publishBleResult("failed", lastWifiFailureReason); startSetupMode(); return; }
  Serial.println("Connecting to Wi-Fi..."); Serial.println("Target SSID: " + cleanSsid);
  stopSetupMode();
  wifiConnectInProgress = true; wifiConnectStartedAt = millis(); lastWifiFailureReason = ""; lastWifiStatusText = "Connecting to Wi-Fi: " + cleanSsid;
  updateBleStatus(); publishBleResult("running", lastWifiStatusText);
  WiFi.disconnect(true); delay(500); WiFi.mode(WIFI_STA); WiFi.setSleep(false); delay(250); WiFi.begin(cleanSsid.c_str(), wifiPassword.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) { delay(500); Serial.print("."); attempts++; updateCachedWifiStatusText(); updateBleStatus(); }
  Serial.println(); wifiConnectInProgress = false;
  if (WiFi.status() == WL_CONNECTED) {
    wifiName = cleanSsid; rememberWifiCredential(wifiName, wifiPassword);
    lastConnectedIp = WiFi.localIP().toString(); lastConnectedSsid = WiFi.SSID(); lastWifiFailureReason = ""; lastWifiStatusText = "Wi-Fi connected: " + lastConnectedSsid + " / " + lastConnectedIp;
    Serial.println("Wi-Fi connected."); Serial.print("IP Address: "); Serial.println(WiFi.localIP()); Serial.print("RSSI: "); Serial.println(WiFi.RSSI());
    lastCommandPollTime = 0; lastRegistrationAttemptTime = 0;
    nodeRegistered = registerNode();
    if (nodeRegistered) { sendHeartbeat(); lastHeartbeatTime = millis(); logSensor("Sensor online."); } else { logSensor("Registration failed, but Wi-Fi is connected. Will retry in loop."); }
    pendingFirmwareStatusCheck = true; updateBleStatus(); publishBleResult("success", "Wi-Fi connected: " + lastConnectedIp);
  } else {
    String statusText = wifiStatusCodeText(WiFi.status());
    lastWifiFailureReason = "Wi-Fi connection failed for " + cleanSsid + ". Status: " + statusText + ". Check SSID/password and 2.4GHz coverage.";
    lastWifiStatusText = lastWifiFailureReason; Serial.println(lastWifiFailureReason); nodeRegistered = false; publishBleResult("failed", lastWifiFailureReason); updateBleStatus(); startSetupMode();
  }
}

void connectToNewWifiFromBle(String ssid, String password, String newLocation, String newResident, String newRoom, String newDeviceName, String newSensorMode, bool shouldRestartAfterSave) {
  ssid.trim();
  if (ssid.length() == 0 || password.length() == 0) { publishBleResult("failed", "Missing Wi-Fi SSID or password."); return; }
  publishBleResult("running", "Saving Wi-Fi and sync settings."); lastWifiStatusText = "Saving BLE Wi-Fi configuration for " + ssid + "."; updateBleStatus();
  wifiName = ssid; wifiPassword = password;
  if (newLocation.length() > 0) locationName = newLocation;
  if (newResident.length() > 0) residentName = newResident;
  if (newRoom.length() > 0) roomName = newRoom;
  String combinedModeHint = newSensorMode + " " + newDeviceName + " " + newRoom; combinedModeHint.toLowerCase();
  Serial.println("BLE requested sensorMode: " + newSensorMode); Serial.println("BLE requested deviceName: " + newDeviceName); Serial.println("BLE requested roomName: " + newRoom);
  String parsedSensorMode = normalizedSensorMode(newSensorMode);
  if (parsedSensorMode.length() > 0) sensorMode = parsedSensorMode;
  else if (combinedModeHint.indexOf("presence") >= 0 || combinedModeHint.indexOf("human") >= 0 || combinedModeHint.indexOf("ld2410") >= 0) sensorMode = "human_presence";
  else if (combinedModeHint.indexOf("motion") >= 0 || combinedModeHint.indexOf("pir") >= 0) sensorMode = "motion";
  else sensorMode = "motion";
  if (newDeviceName.length() > 0) deviceName = newDeviceName; else deviceName = defaultDeviceNameForMode(sensorMode);
  Serial.println("Parsed sensorMode: " + parsedSensorMode); Serial.println("Final saved sensorMode: " + sensorMode); Serial.println("Final saved deviceName: " + deviceName);
  saveSettings(); rememberWifiCredential(wifiName, wifiPassword);
  if (shouldRestartAfterSave) { publishBleResult("success", "Wi-Fi saved. Restarting sensor."); delay(750); ESP.restart(); return; }
  publishBleResult("running", "Connecting to Wi-Fi: " + wifiName); updateBleStatus(); connectToSavedWifi();
  if (WiFi.status() == WL_CONNECTED) publishBleResult("success", "Wi-Fi connected: " + WiFi.localIP().toString() + ". Firmware status check queued. Auto-install is disabled.");
  else publishBleResult("failed", lastWifiFailureReason.length() > 0 ? lastWifiFailureReason : "Wi-Fi connection failed. Setup hotspot fallback is active.");
  updateBleStatus();
}

void handleWifiReconnect() {
  if (wifiName.length() == 0 || setupModeStarted || firmwareUpdateInProgress || wifiConnectInProgress) return;
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastReconnectAttempt >= RECONNECT_INTERVAL_MS) {
    lastReconnectAttempt = millis(); nodeRegistered = false;
    Serial.println("Wi-Fi disconnected. Reconnecting..."); lastWifiStatusText = "Wi-Fi disconnected. Reconnecting to " + wifiName + "."; updateBleStatus();
    WiFi.disconnect(); WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.begin(wifiName.c_str(), wifiPassword.c_str());
  }
}

// MARK: - Render API
bool registerNode() {
  if (WiFi.status() != WL_CONNECTED) return false;
  generateHardwareIds(); logSensor("Registering node...");
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
  payload += "\"presenceInput\":\"GPIO21\"";
  payload += "}";
  int responseCode = http.POST(payload); Serial.print(sensorLabel() + " | Registration response code: "); Serial.println(responseCode);
  http.end(); client.stop(); delay(50);
  if (responseCode >= 200 && responseCode < 300) { markServerSuccess("registerNode"); return true; }
  markSetupCloudFailure("registerNode", responseCode); return false;
}

bool sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return false;
  generateHardwareIds();
  int motionState = digitalRead(PIR_PIN); int presenceState = digitalRead(PRESENCE_PIN);
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(HTTP_TIMEOUT_MS); http.setReuse(false); http.begin(client, HEARTBEAT_URL); http.addHeader("Content-Type", "application/json"); http.addHeader("x-webhook-secret", WEBHOOK_SECRET);
  String payload = "{";
  payload += "\"nodeId\":\"" + nodeId + "\",";
  payload += "\"nodeName\":\"" + jsonEscape(deviceName + " - " + roomName) + "\",";
  payload += "\"locationName\":\"" + jsonEscape(locationName) + "\",";
  payload += "\"localIp\":\"" + WiFi.localIP().toString() + "\",";
  payload += "\"localConfigPort\":80,";
  payload += "\"cameraCount\":0,";
  payload += "\"cameraSummary\":[],";
  payload += "\"wifiRssi\":" + String(WiFi.RSSI()) + ",";
  payload += "\"wifiSsid\":\"" + jsonEscape(WiFi.SSID()) + "\",";
  payload += "\"setupId\":\"" + setupId + "\",";
  payload += "\"assignmentState\":\"" + assignmentState() + "\",";
  payload += "\"uptimeSeconds\":" + String(millis() / 1000) + ",";
  payload += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  payload += "\"softwareVersion\":\"" + String(SOFTWARE_VERSION) + "\",";
  payload += "\"firmwareUpdateInProgress\":" + String(firmwareUpdateInProgress ? "true" : "false") + ",";
  payload += "\"bleManagement\":\"Enabled\",";
  payload += "\"autoFirmwareInstallOnBoot\":false,";
  payload += "\"monitorStatus\":\"Online\",";
  payload += "\"ffmpegStatus\":\"Not Applicable\",";
  payload += "\"platform\":\"ESP32\",";
  payload += "\"hostname\":\"" + nodeId + "\",";
  payload += "\"activeMonitorCount\":" + String(activeMonitorCountForMode()) + ",";
  payload += "\"sensorMode\":\"" + jsonEscape(sensorMode) + "\",";
  payload += "\"sourceKey\":\"" + jsonEscape(sourceKey) + "\",";
  payload += "\"motionState\":" + String(motionState == MOTION_ACTIVE_STATE ? "true" : "false") + ",";
  payload += "\"presenceState\":" + String(presenceState == PRESENCE_ACTIVE_STATE ? "true" : "false") + ",";
  payload += "\"diagnostics\":{";
  payload += "\"sourceKey\":\"" + sourceKey + "\",";
  payload += "\"setupId\":\"" + setupId + "\",";
  payload += "\"assignmentState\":\"" + assignmentState() + "\",";
  payload += "\"wifiSsid\":\"" + jsonEscape(WiFi.SSID()) + "\",";
  payload += "\"deviceName\":\"" + jsonEscape(deviceName) + "\",";
  payload += "\"sensorMode\":\"" + jsonEscape(sensorMode) + "\",";
  payload += "\"roomName\":\"" + jsonEscape(roomName) + "\",";
  payload += "\"residentName\":\"" + jsonEscape(residentName) + "\",";
  payload += "\"locationName\":\"" + jsonEscape(locationName) + "\",";
  payload += "\"wifiRssi\":" + String(WiFi.RSSI()) + ",";
  payload += "\"localIp\":\"" + WiFi.localIP().toString() + "\",";
  payload += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  payload += "\"serverFailureCount\":" + String(consecutiveServerFailureCount) + ",";
  payload += "\"lastMotionWebhookSuccessSeconds\":" + String(lastMotionWebhookSuccessTime == 0 ? 0 : (lastMotionWebhookSuccessTime / 1000)) + ",";
  payload += "\"lastPresenceWebhookSuccessSeconds\":" + String(lastPresenceWebhookSuccessTime == 0 ? 0 : (lastPresenceWebhookSuccessTime / 1000)) + ",";
  payload += "\"motionState\":" + String(motionState == MOTION_ACTIVE_STATE ? "true" : "false") + ",";
  payload += "\"presenceState\":" + String(presenceState == PRESENCE_ACTIVE_STATE ? "true" : "false") + ",";
  payload += "\"wifiStatus\":\"connected\",";
  payload += "\"bleManagement\":\"Enabled\",";
  payload += "\"autoFirmwareInstallOnBoot\":false";
  payload += "}}";
  int responseCode = http.POST(payload); Serial.print(sensorLabel() + " | Heartbeat response code: "); Serial.println(responseCode);
  http.end(); client.stop(); delay(50);
  if (responseCode >= 200 && responseCode < 300) { markServerSuccess("sendHeartbeat"); lastConnectedIp = WiFi.localIP().toString(); lastConnectedSsid = WiFi.SSID(); lastWifiStatusText = "Wi-Fi connected: " + lastConnectedSsid + " / " + lastConnectedIp; updateBleStatus(); return true; }
  markServerFailure("sendHeartbeat", responseCode); return false;
}

void pollSensorCommands() {
  if (WiFi.status() != WL_CONNECTED) return;
  generateHardwareIds();
  String url = String(BASE_URL) + "/sensor-commands/" + nodeId + "/pending";
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.setTimeout(5000);
  http.setReuse(false);
  http.begin(client, url);
  http.addHeader("x-webhook-secret", WEBHOOK_SECRET);
  int responseCode = http.GET();

  if (responseCode != 200) {
    // Command polling is optional remote-control traffic. It must never make
    // an otherwise healthy sensor look offline, and it must never drive
    // self-heal reconnects or reboot loops. Heartbeats and priority motion /
    // presence events remain the source of truth for critical cloud health.
    Serial.print(sensorLabel() + " | Command poll response code: ");
    Serial.println(responseCode);
    Serial.println(sensorLabel() + " | Command poll skipped as noncritical. " + httpErrorDescription(http, responseCode));
    http.end();
    client.stop();
    delay(50);
    return;
  }

  String response = http.getString();
  http.end();
  client.stop();
  delay(50);

  if (response.indexOf("\"count\":0") >= 0) return;
  markServerSuccess("pollSensorCommands_with_pending_command");
  logSensor("Command response received.");
  String commandId = extractJsonString(response, "commandId"); String commandType = extractJsonString(response, "commandType");
  logSensor("Command type: " + commandType);
  if (commandType == "reconfigure") { reportCommandResult(commandId, "success", "ESP32 reconfigure started."); clearSettingsAndRestart(); return; }
  if (commandType == "reboot" || commandType == "restart") { reportCommandResult(commandId, "success", "ESP32 rebooting."); delay(1000); ESP.restart(); return; }
  if (commandType == "factory_reset") { reportCommandResult(commandId, "success", "ESP32 factory reset started."); factoryClearEverything(); delay(1000); ESP.restart(); return; }
  if (commandType == "update_firmware") { String firmwareUrl = extractJsonString(response, "firmwareUrl"); if (firmwareUrl.length() == 0) firmwareUrl = extractJsonString(response, "url"); if (firmwareUrl.length() == 0) { reportCommandResult(commandId, "failed", "Firmware update failed: missing firmwareUrl in command payload."); return; } performFirmwareUpdate(commandId, firmwareUrl); return; }
  if (commandType == "identify" || commandType == "locate") { reportCommandResult(commandId, "running", "ESP32 identify LED started."); blinkIdentifyLed(12000); delay(250); reportCommandResult(commandId, "success", "ESP32 identify LED completed."); return; }
  if (commandType == "check_firmware") { bool ok = checkLatestFirmwareStatusOnly(); reportCommandResult(commandId, ok ? "success" : "failed", ok ? "Firmware status check completed. Auto-install disabled." : "Firmware status check failed."); return; }
  if (commandType == "ping") { reportCommandResult(commandId, "success", "Ping received by ESP32."); return; }
  reportCommandResult(commandId, "failed", "Unknown command type for this ESP32 firmware.");
}

void reportCommandResult(String commandId, String status, String message) {
  if (commandId.length() == 0) return;
  generateHardwareIds();
  String url = String(BASE_URL) + "/sensor-commands/" + commandId + "/result";
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(HTTP_TIMEOUT_MS); http.setReuse(false); http.begin(client, url); http.addHeader("Content-Type", "application/json"); http.addHeader("x-webhook-secret", WEBHOOK_SECRET);
  String payload = "{";
  payload += "\"status\":\"" + status + "\",";
  payload += "\"result\":{";
  payload += "\"message\":\"" + jsonEscape(message) + "\",";
  payload += "\"nodeId\":\"" + nodeId + "\",";
  payload += "\"setupId\":\"" + setupId + "\",";
  payload += "\"sourceKey\":\"" + jsonEscape(sourceKey) + "\",";
  payload += "\"sensorMode\":\"" + jsonEscape(sensorMode) + "\",";
  payload += "\"softwareVersion\":\"" + String(SOFTWARE_VERSION) + "\"";
  payload += "}}";
  int responseCode = http.POST(payload); Serial.print(sensorLabel() + " | Command result response code: "); Serial.println(responseCode);
  if (responseCode >= 200 && responseCode < 300) markServerSuccess("reportCommandResult"); else markServerFailure("reportCommandResult", responseCode);
  http.end(); client.stop(); delay(50);
}

// MARK: - Firmware
bool checkLatestFirmwareStatusOnly() {
  if (WiFi.status() != WL_CONNECTED) { publishBleResult("failed", "Firmware check failed: Wi-Fi offline."); return false; }
  logSensor("Checking latest firmware status only. Auto-install disabled.");
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(HTTP_TIMEOUT_MS); http.setReuse(false);
  if (!http.begin(client, LATEST_FIRMWARE_URL)) { publishBleResult("failed", "Could not open latest firmware endpoint."); return false; }
  http.addHeader("x-webhook-secret", WEBHOOK_SECRET);
  int responseCode = http.GET();
  if (responseCode < 200 || responseCode >= 300) { String message = "Firmware check failed. HTTP " + String(responseCode) + "."; http.end(); client.stop(); logSensor(message); publishBleResult("failed", message); return false; }
  String response = http.getString(); http.end(); client.stop(); delay(50);
  String latestVersion = extractJsonString(response, "firmwareVersion"); String firmwareUrl = extractJsonString(response, "firmwareUrl");
  if (latestVersion.length() == 0 || firmwareUrl.length() == 0) { publishBleResult("failed", "Latest firmware response missing fields."); return false; }
  String currentVersion = String(SOFTWARE_VERSION);
  logSensor("Current firmware: " + currentVersion); logSensor("Server latest released firmware: " + latestVersion);
  if (latestVersion == currentVersion) publishBleResult("success", "Firmware matches server release: " + latestVersion);
  else publishBleResult("success", "Firmware status checked. Running " + currentVersion + "; server release is " + latestVersion + ". Auto-install disabled.");
  return true;
}

String httpErrorDescription(HTTPClient& http, int responseCode) { return responseCode > 0 ? "HTTP " + String(responseCode) : "HTTP " + String(responseCode) + " (" + http.errorToString(responseCode) + ")"; }

void stopBleForFirmwareUpdate() {
  if (!bleStarted) return;
  Serial.println(sensorLabel() + " | Stopping BLE before OTA to free heap and radio resources.");
  if (bleResultCharacteristic != nullptr && bleClientConnected) { bleResultCharacteristic->setValue("{\"status\":\"running\",\"message\":\"Firmware update starting. BLE will disconnect during download.\"}"); bleResultCharacteristic->notify(); delay(350); }
  BLEDevice::stopAdvertising(); delay(250); BLEDevice::deinit(true); delay(1000);
  bleServer = nullptr; bleStatusCharacteristic = nullptr; bleCommandCharacteristic = nullptr; bleResultCharacteristic = nullptr; bleStarted = false; bleClientConnected = false;
  Serial.print(sensorLabel() + " | Free heap after BLE stop: "); Serial.println(ESP.getFreeHeap());
}

void finishFirmwareUpdateFailure(String commandId, String message) {
  Serial.println(sensorLabel() + " | " + message); firmwareUpdateInProgress = false;
  if (commandId.length() > 0) reportCommandResult(commandId, "failed", message);
  if (!bleStarted) { startBleManagement(); delay(500); }
  publishBleResult("failed", message); updateBleStatus();
}

void performFirmwareUpdate(String commandId, String firmwareUrl) {
  if (WiFi.status() != WL_CONNECTED) { finishFirmwareUpdateFailure(commandId, "Firmware update failed: Wi-Fi offline."); return; }
  firmwareUpdateInProgress = true;
  logSensor("Firmware update requested."); logSensor("Firmware URL: " + firmwareUrl);
  Serial.print(sensorLabel() + " | Free heap before OTA prep: "); Serial.println(ESP.getFreeHeap());
  if (setupModeStarted) { server.close(); WiFi.softAPdisconnect(true); setupModeStarted = false; delay(250); } else { server.close(); delay(100); }
  stopBleForFirmwareUpdate();
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); delay(750);
  if (WiFi.status() != WL_CONNECTED) { finishFirmwareUpdateFailure(commandId, "Firmware update failed: Wi-Fi disconnected before download."); return; }
  Serial.print(sensorLabel() + " | Free heap before HTTPS begin: "); Serial.println(ESP.getFreeHeap());
  WiFiClientSecure client; client.setInsecure(); client.setTimeout(120000);
  HTTPClient http; http.setTimeout(180000); http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); http.setReuse(false);
  if (!http.begin(client, firmwareUrl)) { finishFirmwareUpdateFailure(commandId, "Firmware update failed: could not open firmware URL."); return; }
  http.addHeader("User-Agent", "GoodShepherd-ESP32-OTA/1.8.4");
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
  http.end(); client.stop();
  if (commandId.length() > 0) reportCommandResult(commandId, "success", "Firmware update installed. ESP32 rebooting.");
  Serial.println(sensorLabel() + " | Firmware update complete. Rebooting..."); delay(1500); ESP.restart();
}

// MARK: - BLE Commands
void handleBleCommand(String payload) {
  String command = extractJsonString(payload, "command"); if (command.length() == 0) command = extractJsonString(payload, "action"); command.toLowerCase();
  if (command.length() == 0) { publishBleResult("failed", "Missing BLE command."); return; }
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
  if (command == "check_firmware") { checkLatestFirmwareStatusOnly(); return; }
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
  if (motion == MOTION_ACTIVE_STATE && !motionAlreadyReported) {
    if (lastMotionWebhookAttemptTime == 0 || millis() - lastMotionWebhookAttemptTime >= MOTION_RETRY_INTERVAL_MS) {
      lastMotionWebhookAttemptTime = millis(); logSensor("Motion detected. Sending webhook...");
      if (sendMotionEvent()) { motionAlreadyReported = true; lastMotionWebhookSuccessTime = millis(); lastNoMotionTime = 0; } else logSensor("Motion webhook failed. Will retry while motion remains active.");
    }
  }
  if (motion != MOTION_ACTIVE_STATE && motionAlreadyReported) {
    if (lastNoMotionTime == 0) lastNoMotionTime = millis();
    if (millis() - lastNoMotionTime > RESET_AFTER_NO_MOTION_MS) { logSensor("Motion reset."); motionAlreadyReported = false; lastNoMotionTime = 0; }
  }
  if (motion == MOTION_ACTIVE_STATE) lastNoMotionTime = 0;
}

bool sendMotionEvent() {
  if (WiFi.status() != WL_CONNECTED) { logSensor("Wi-Fi offline. Motion event skipped."); markServerFailure("sendMotionEvent_offline", 0); return false; }
  generateHardwareIds();
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(HTTP_TIMEOUT_MS); http.setReuse(false); http.begin(client, WEBHOOK_URL); http.addHeader("Content-Type", "application/json"); http.addHeader("x-webhook-secret", WEBHOOK_SECRET);
  String fullSourceName = deviceName + " - " + roomName; String message = "Motion detected from " + fullSourceName;
  String payload = "{";
  payload += "\"nodeId\":\"" + nodeId + "\",";
  payload += "\"locationName\":\"" + jsonEscape(locationName) + "\",";
  payload += "\"sourceKey\":\"" + sourceKey + "\",";
  payload += "\"sourceName\":\"" + jsonEscape(fullSourceName) + "\",";
  payload += "\"residentName\":\"" + jsonEscape(residentName) + "\",";
  payload += "\"message\":\"" + jsonEscape(message) + "\",";
  payload += "\"alertLevel\":\"Normal\",";
  payload += "\"timeText\":\"ESP32 Motion Event\",";
  payload += "\"sensorMode\":\"" + jsonEscape(sensorMode) + "\",";
  payload += "\"sensorType\":\"motion\",";
  payload += "\"source\":\"pir\",";
  payload += "\"eventType\":\"motion_detected\",";
  payload += "\"motion\":true";
  payload += "}";
  lastPriorityHttpAttemptTime = millis();
  int responseCode = http.POST(payload); Serial.print(sensorLabel() + " | Motion webhook response code: "); Serial.println(responseCode);
  http.end(); client.stop(); delay(50);
  if (responseCode >= 200 && responseCode < 300) { markServerSuccess("sendMotionEvent"); return true; }
  markServerFailure("sendMotionEvent", responseCode); return false;
}

// MARK: - Human Presence
void handlePresenceSensor() {
  int presence = digitalRead(PRESENCE_PIN);
  if (presence != lastPresenceState) { lastPresenceState = presence; logSensor("LD2410 presence changed to " + String(presence)); }

  if (presence == PRESENCE_ACTIVE_STATE) {
    pendingPresenceClearedEvent = false;
    lastNoPresenceTime = 0;

    if (presenceHeldActiveStartedAt == 0) presenceHeldActiveStartedAt = millis();
    else if (presenceAlreadyReported && millis() - presenceHeldActiveStartedAt >= PRESENCE_HELD_ACTIVE_RESET_MS) {
      logSensor("Presence held active too long. Resetting reported state so future presence activity is not suppressed.");
      presenceAlreadyReported = false;
      presenceHeldActiveStartedAt = millis();
    }

    if (!presenceAlreadyReported) pendingPresenceDetectedEvent = true;

    if (pendingPresenceDetectedEvent && (lastPresenceWebhookAttemptTime == 0 || millis() - lastPresenceWebhookAttemptTime >= PRESENCE_RETRY_INTERVAL_MS)) {
      lastPresenceWebhookAttemptTime = millis();
      logSensor("Priority presence_detected pending. Sending/retrying webhook...");
      if (sendPresenceStateEvent(true)) {
        pendingPresenceDetectedEvent = false;
        presenceAlreadyReported = true;
        lastPresenceWebhookSuccessTime = millis();
      } else {
        logSensor("Priority presence_detected failed. Will retry while presence remains active.");
      }
    }

    return;
  }

  presenceHeldActiveStartedAt = 0;
  pendingPresenceDetectedEvent = false;

  if (presenceAlreadyReported || pendingPresenceClearedEvent) {
    if (lastNoPresenceTime == 0) lastNoPresenceTime = millis();

    if (millis() - lastNoPresenceTime > RESET_AFTER_NO_PRESENCE_MS) {
      pendingPresenceClearedEvent = true;

      if (lastPresenceWebhookAttemptTime == 0 || millis() - lastPresenceWebhookAttemptTime >= PRESENCE_RETRY_INTERVAL_MS) {
        lastPresenceWebhookAttemptTime = millis();
        logSensor("Priority presence_cleared pending. Sending/retrying webhook...");
        if (sendPresenceStateEvent(false)) {
          pendingPresenceClearedEvent = false;
          presenceAlreadyReported = false;
          lastPresenceWebhookSuccessTime = millis();
          lastNoPresenceTime = 0;
        } else {
          logSensor("Priority presence_cleared failed. Will retry while room remains clear.");
        }
      }
    }
  }
}

bool sendPresenceStateEvent(bool presenceActive) {
  if (WiFi.status() != WL_CONNECTED) { logSensor(presenceActive ? "Wi-Fi offline. Presence detected event skipped." : "Wi-Fi offline. Presence cleared event skipped."); markServerFailure(presenceActive ? "sendPresenceDetectedEvent_offline" : "sendPresenceClearedEvent_offline", 0); return false; }
  generateHardwareIds();
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(HTTP_TIMEOUT_MS); http.setReuse(false); http.begin(client, WEBHOOK_URL); http.addHeader("Content-Type", "application/json"); http.addHeader("x-webhook-secret", WEBHOOK_SECRET);
  String fullSourceName = deviceName + " - " + roomName;
  String eventType = presenceActive ? "presence_detected" : "presence_cleared";
  String message = presenceActive ? "Human presence detected from " + fullSourceName : "Human presence cleared from " + fullSourceName;
  String timeText = presenceActive ? "ESP32 Human Presence Detected Event" : "ESP32 Human Presence Cleared Event";
  String payload = "{";
  payload += "\"nodeId\":\"" + nodeId + "\",";
  payload += "\"locationName\":\"" + jsonEscape(locationName) + "\",";
  payload += "\"sourceKey\":\"" + sourceKey + "\",";
  payload += "\"sourceName\":\"" + jsonEscape(fullSourceName) + "\",";
  payload += "\"residentName\":\"" + jsonEscape(residentName) + "\",";
  payload += "\"message\":\"" + jsonEscape(message) + "\",";
  payload += "\"alertLevel\":\"Normal\",";
  payload += "\"timeText\":\"" + timeText + "\",";
  payload += "\"sensorMode\":\"" + jsonEscape(sensorMode) + "\",";
  payload += "\"sensorType\":\"human_presence\",";
  payload += "\"source\":\"ld2410\",";
  payload += "\"eventType\":\"" + eventType + "\",";
  payload += "\"presence\":" + String(presenceActive ? "true" : "false");
  payload += "}";
  lastPriorityHttpAttemptTime = millis();
  int responseCode = http.POST(payload); Serial.print(sensorLabel() + " | Presence state webhook response code: "); Serial.println(responseCode);
  http.end(); client.stop(); delay(50);
  if (responseCode >= 200 && responseCode < 300) { markServerSuccess(presenceActive ? "sendPresenceDetectedEvent" : "sendPresenceClearedEvent"); return true; }
  markServerFailure(presenceActive ? "sendPresenceDetectedEvent" : "sendPresenceClearedEvent", responseCode); return false;
}

// MARK: - Fallback Web Setup
void handleRoot() {
  int networkCount = WiFi.scanNetworks(); String knownWifiMap = savedWifiJavaScriptMap(); String html = "";
  html += "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><title>Good Shepherd Sensor Activation</title><style>";
  html += "body{font-family:-apple-system,BlinkMacSystemFont,Arial,sans-serif;background:#f4f7fb;margin:0;padding:20px;color:#142033}.card{max-width:560px;margin:0 auto;background:white;border-radius:18px;padding:20px;box-shadow:0 8px 24px rgba(20,32,51,.12)}h1{margin:0 0 8px;font-size:26px}p{color:#526173;line-height:1.35}label{display:block;font-weight:700;margin-top:14px;margin-bottom:6px}input,select{box-sizing:border-box;width:100%;font-size:20px;padding:12px;border:1px solid #ccd6e0;border-radius:10px;background:white}button{width:100%;font-size:20px;padding:14px;margin-top:18px;background:#168cff;color:white;border:0;border-radius:12px;font-weight:700}.meta{font-size:13px;background:#eef4fb;padding:10px;border-radius:10px;color:#526173}.hint{font-size:13px;color:#526173}a{color:#168cff;text-decoration:none;font-weight:700}";
  html += "</style></head><body><div class='card'><h1>Good Shepherd Sensor Activation</h1><p>Activate this Good Shepherd sensor.</p>";
  html += "<div class='meta'>";
  html += "<b>Setup ID:</b><br>" + htmlEscape(setupId);
  html += "<br><br><b>Hardware ID:</b><br>" + htmlEscape(nodeId);
  html += "<br><br><b>Source Key:</b><br>" + htmlEscape(sourceKey);
  html += "<br><br><b>Sensor Mode:</b><br>" + htmlEscape(sensorMode);
  html += "<br><br><b>Firmware:</b><br>" + String(SOFTWARE_VERSION);
  html += "<br><br><b>Wi-Fi Status:</b><br>" + htmlEscape(lastWifiStatusText);
  html += "<br><br><b>Presence Input:</b><br>GPIO21 OUT pin";
  html += "<br><br><b>BLE:</b><br>Enabled";
  html += "<br><br><b>Auto firmware install:</b><br>Disabled";
  html += "</div>";
  html += "<form method='POST' action='/save'>";
  html += "<label>Sensor Type</label><select id='sensorMode' name='sensorMode'>";
  html += "<option value='motion'" + String(sensorMode == "motion" ? " selected" : "") + ">Motion Sensor</option>";
  html += "<option value='human_presence'" + String(sensorMode == "human_presence" ? " selected" : "") + ">Human Presence Sensor</option>";
  html += "</select>";
  html += "<label>Wi-Fi Network</label><select id='wifiName' name='wifiName'>";
  if (networkCount <= 0) html += "<option value=''>No Wi-Fi networks found</option>";
  else for (int i = 0; i < networkCount; i++) { String ssid = WiFi.SSID(i); int rssi = WiFi.RSSI(i); String selected = ssid == wifiName ? " selected" : ""; html += "<option value='" + htmlEscape(ssid) + "'" + selected + ">" + htmlEscape(ssid) + " (" + String(rssi) + " dBm)</option>"; }
  html += "</select>";
  html += "<label>Wi-Fi Password</label><input id='wifiPassword' name='wifiPassword' type='password' value='" + htmlEscape(savedWifiPasswordFor(wifiName)) + "' required>";
  html += "<div class='hint'>If this Wi-Fi was used before, the password will fill automatically when selected.</div>";
  html += "<label>Home / Location Name</label><input name='locationName' value='" + htmlEscape(locationName) + "' required>";
  html += "<label>Resident Name</label><input name='residentName' value='" + htmlEscape(residentName) + "' required>";
  html += "<label>Room Name</label><input name='roomName' value='" + htmlEscape(roomName) + "' required>";
  html += "<label>Device Name</label><input id='deviceName' name='deviceName' value='" + htmlEscape(deviceName) + "' required>";
  html += "<button type='submit'>Activate Sensor</button></form>";
  html += "<p class='hint'><a href='/'>Refresh Wi-Fi List</a> &nbsp; | &nbsp; <a href='/reset'>Clear assignment</a></p>";
  html += "<p class='hint'><a href='/factory-reset'>Factory reset saved Wi-Fi history</a></p>";
  html += "<script>";
  html += "var knownWifiPasswords=" + knownWifiMap + ";";
  html += "var wifiSelect=document.getElementById('wifiName');var passwordField=document.getElementById('wifiPassword');";
  html += "function autofillWifiPassword(){var ssid=wifiSelect.value;if(knownWifiPasswords[ssid]){passwordField.value=knownWifiPasswords[ssid];}}";
  html += "wifiSelect.addEventListener('change',autofillWifiPassword);autofillWifiPassword();";
  html += "var sensorSelect=document.getElementById('sensorMode');var deviceNameField=document.getElementById('deviceName');";
  html += "function defaultDeviceNameForMode(mode){if(mode==='human_presence')return 'Human Presence Sensor';return 'Motion Sensor';}";
  html += "sensorSelect.addEventListener('change',function(){deviceNameField.value=defaultDeviceNameForMode(sensorSelect.value);});";
  html += "</script></div></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  wifiName = server.arg("wifiName"); wifiPassword = server.arg("wifiPassword"); locationName = server.arg("locationName"); residentName = server.arg("residentName"); roomName = server.arg("roomName"); sensorMode = safeSensorModeOrMotion(server.arg("sensorMode")); deviceName = server.arg("deviceName");
  if (deviceName.length() == 0) deviceName = defaultDeviceNameForMode(sensorMode);
  saveSettings();
  server.send(200, "text/html", "<html><body style='font-family:Arial;padding:20px;'><h1>Good Shepherd Setup</h1><p>Sensor activated. Rebooting now.</p></body></html>");
  delay(2000); ESP.restart();
}

void handleReset() { clearAssignmentSettingsOnly(); server.send(200, "text/html", "<html><body style='font-family:Arial;padding:20px;'><h1>Good Shepherd Setup</h1><p>Assignment settings cleared. Saved Wi-Fi passwords were kept. Rebooting...</p></body></html>"); delay(2000); ESP.restart(); }
void handleFactoryReset() { factoryClearEverything(); server.send(200, "text/html", "<html><body style='font-family:Arial;padding:20px;'><h1>Good Shepherd Setup</h1><p>Factory reset complete. Saved Wi-Fi history was also cleared. Rebooting...</p></body></html>"); delay(2000); ESP.restart(); }
