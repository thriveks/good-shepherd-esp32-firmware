#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>

#define PIR_PIN 27
#define MOTION_ACTIVE_STATE HIGH
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
String setupId = "";

const char* BASE_URL = "https://good-shepherd-server-j06f.onrender.com";
const char* WEBHOOK_URL = "https://good-shepherd-server-j06f.onrender.com/webhook";
const char* REGISTER_URL = "https://good-shepherd-server-j06f.onrender.com/nodes/register";
const char* HEARTBEAT_URL = "https://good-shepherd-server-j06f.onrender.com/node-health";
const char* WEBHOOK_SECRET = "7e9c767aa079423227163be90943d7d2";

const char* SOFTWARE_VERSION = "esp32-motion-v1.6.6-identify-led-proof";

bool setupModeStarted = false;
bool nodeRegistered = false;
bool motionAlreadyReported = false;
bool firmwareUpdateInProgress = false;

int lastPirState = -1;

unsigned long lastNoMotionTime = 0;
unsigned long lastHeartbeatTime = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long lastCommandPollTime = 0;
unsigned long lastRegistrationAttemptTime = 0;

const unsigned long RESET_AFTER_NO_MOTION_MS = 10000;
const unsigned long HEARTBEAT_INTERVAL_MS = 60000;
const unsigned long RECONNECT_INTERVAL_MS = 10000;
const unsigned long COMMAND_POLL_INTERVAL_MS = 15000;
const unsigned long REGISTRATION_RETRY_INTERVAL_MS = 15000;

const int MAX_SAVED_WIFI = 8;

String getChipId() {
  uint64_t chipid = ESP.getEfuseMac();
  char id[13];
  snprintf(id, sizeof(id), "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
  String result = String(id);
  result.toLowerCase();
  return result;
}

void generateHardwareIds() {
  String chipId = getChipId();
  nodeId = "esp32-" + chipId;
  sourceKey = "motion-" + chipId;
}

String generateSetupId() {
  const char* alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  String result = "";

  uint32_t seed = (uint32_t)ESP.getEfuseMac() ^ micros();
  randomSeed(seed);

  for (int i = 0; i < 6; i++) {
    int index = random(0, 32);
    result += alphabet[index];
  }

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

void setLed(bool isOn) {
  digitalWrite(LED_PIN, isOn ? LED_ACTIVE_STATE : LED_INACTIVE_STATE);
}

void blinkIdentifyLed(unsigned long durationMs) {
  unsigned long startedAt = millis();
  bool ledOn = false;

  logSensor("Identify LED started.");

  while (millis() - startedAt < durationMs) {
    ledOn = !ledOn;
    setLed(ledOn);
    delay(150);
  }

  setLed(false);
  logSensor("Identify LED finished.");
}

String assignmentState() {
  if (locationName.length() == 0 || residentName.length() == 0 || roomName.length() == 0) {
    return "Unassigned";
  }

  return "Assigned";
}

String sensorLabel() {
  String room = roomName;
  room.trim();

  if (room.length() == 0) {
    room = "No Room";
  }

  return nodeId + " | " + room;
}

void logSensor(String message) {
  Serial.println(sensorLabel() + " | " + message);
}

String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  return value;
}

String jsEscape(String value) {
  value.replace("\\", "\\\\");
  value.replace("'", "\\'");
  value.replace("\"", "\\\"");
  value.replace("\n", "\\n");
  value.replace("\r", "");
  return value;
}

String jsonEscape(String value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  value.replace("\n", "\\n");
  value.replace("\r", "");
  return value;
}

String wifiSsidKey(int index) {
  return "ssid" + String(index);
}

String wifiPassKey(int index) {
  return "pass" + String(index);
}

void rememberWifiCredential(String ssid, String password) {
  ssid.trim();

  if (ssid.length() == 0 || password.length() == 0) {
    return;
  }

  prefs.begin("gs-device", false);

  int existingIndex = -1;
  int emptyIndex = -1;

  for (int i = 0; i < MAX_SAVED_WIFI; i++) {
    String savedSsid = prefs.getString(wifiSsidKey(i).c_str(), "");

    if (savedSsid == ssid) {
      existingIndex = i;
      break;
    }

    if (savedSsid.length() == 0 && emptyIndex < 0) {
      emptyIndex = i;
    }
  }

  int targetIndex = existingIndex >= 0 ? existingIndex : emptyIndex;

  if (targetIndex < 0) {
    targetIndex = 0;
  }

  prefs.putString(wifiSsidKey(targetIndex).c_str(), ssid);
  prefs.putString(wifiPassKey(targetIndex).c_str(), password);

  prefs.end();

  Serial.println("Saved Wi-Fi credential for: " + ssid);
}

String savedWifiPasswordFor(String ssid) {
  ssid.trim();

  if (ssid.length() == 0) {
    return "";
  }

  prefs.begin("gs-device", true);

  String result = "";

  for (int i = 0; i < MAX_SAVED_WIFI; i++) {
    String savedSsid = prefs.getString(wifiSsidKey(i).c_str(), "");

    if (savedSsid == ssid) {
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
      if (!first) {
        js += ",";
      }

      js += "\"" + jsEscape(savedSsid) + "\":\"" + jsEscape(savedPass) + "\"";
      first = false;
    }
  }

  js += "}";

  prefs.end();

  return js;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PIR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  setLed(false);

  Serial.println();
  Serial.println("Good Shepherd ESP32 Motion v1.6.6");

  generateHardwareIds();
  ensureSetupId();
  loadSettings();

  if (wifiName.length() > 0) {
    connectToSavedWifi();
  } else {
    startSetupMode();
  }
}

void loop() {
  if (firmwareUpdateInProgress) {
    delay(100);
    return;
  }

  if (setupModeStarted) {
    server.handleClient();
  }

  handleWifiReconnect();

  if (WiFi.status() == WL_CONNECTED) {
    if (millis() - lastCommandPollTime >= COMMAND_POLL_INTERVAL_MS) {
      pollSensorCommands();
      lastCommandPollTime = millis();
    }

    if (!nodeRegistered && millis() - lastRegistrationAttemptTime >= REGISTRATION_RETRY_INTERVAL_MS) {
      lastRegistrationAttemptTime = millis();
      nodeRegistered = registerNode();

      if (nodeRegistered) {
        sendHeartbeat();
        lastHeartbeatTime = millis();
        logSensor("Sensor online.");
      }
    }

    if (nodeRegistered && millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL_MS) {
      sendHeartbeat();
      lastHeartbeatTime = millis();
    }

    if (nodeRegistered) {
      handleMotionSensor();
    }
  }

  delay(100);
}

void loadSettings() {
  prefs.begin("gs-device", true);

  wifiName = prefs.getString("wifiName", "");
  wifiPassword = prefs.getString("wifiPass", "");

  locationName = prefs.getString("location", "");
  residentName = prefs.getString("resident", "");
  roomName = prefs.getString("room", "");
  deviceName = prefs.getString("deviceName", "Motion Sensor");

  prefs.end();

  generateHardwareIds();

  Serial.println("Settings loaded.");
  Serial.println("Hardware ID: " + nodeId);
  Serial.println("Source Key: " + sourceKey);
  Serial.println("Setup ID: " + setupId);
  Serial.println("Wi-Fi Name: " + wifiName);
  Serial.println("Room Name: " + roomName);
}

void saveSettings() {
  prefs.begin("gs-device", false);

  prefs.putString("wifiName", wifiName);
  prefs.putString("wifiPass", wifiPassword);

  prefs.remove("nodeId");
  prefs.remove("sourceKey");

  prefs.putString("location", locationName);
  prefs.putString("resident", residentName);
  prefs.putString("room", roomName);
  prefs.putString("deviceName", deviceName);

  prefs.end();

  generateHardwareIds();

  Serial.println("Settings saved.");
}

void clearAssignmentSettingsOnly() {
  prefs.begin("gs-device", false);

  prefs.remove("wifiName");
  prefs.remove("wifiPass");
  prefs.remove("location");
  prefs.remove("resident");
  prefs.remove("room");
  prefs.remove("deviceName");
  prefs.remove("nodeId");
  prefs.remove("sourceKey");

  prefs.end();

  wifiName = "";
  wifiPassword = "";
  locationName = "";
  residentName = "";
  roomName = "";
  deviceName = "Motion Sensor";

  generateHardwareIds();

  Serial.println("Assignment settings cleared. Saved Wi-Fi history was kept.");
}

void factoryClearEverything() {
  prefs.begin("gs-device", false);
  prefs.clear();
  prefs.end();

  wifiName = "";
  wifiPassword = "";
  locationName = "";
  residentName = "";
  roomName = "";
  deviceName = "Motion Sensor";

  generateHardwareIds();
  ensureSetupId();

  Serial.println("Factory settings cleared, including saved Wi-Fi history. New setup ID generated.");
}

void clearSettingsAndRestart() {
  logSensor("Reconfigure requested. Clearing assignment settings and restarting...");
  clearAssignmentSettingsOnly();
  delay(1000);
  ESP.restart();
}

void startSetupMode() {
  if (setupModeStarted) return;

  setupModeStarted = true;
  nodeRegistered = false;

  WiFi.disconnect(true);
  delay(250);

  WiFi.mode(WIFI_AP);
  WiFi.softAP("GoodShepherd-Setup");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reset", HTTP_GET, handleReset);
  server.on("/factory-reset", HTTP_GET, handleFactoryReset);

  server.begin();

  Serial.println("Setup mode started.");
  Serial.println("Connect to Wi-Fi: GoodShepherd-Setup");
  Serial.println("Open: http://192.168.4.1");
}

void stopSetupMode() {
  if (!setupModeStarted) return;

  Serial.println("Stopping setup mode...");

  server.close();
  WiFi.softAPdisconnect(true);
  setupModeStarted = false;

  delay(250);
}

void connectToSavedWifi() {
  Serial.println("Connecting to Wi-Fi...");

  stopSetupMode();

  WiFi.disconnect(true);
  delay(500);

  WiFi.mode(WIFI_STA);
  delay(250);

  WiFi.begin(wifiName.c_str(), wifiPassword.c_str());

  int attempts = 0;

  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    rememberWifiCredential(wifiName, wifiPassword);

    Serial.println("Wi-Fi connected.");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    lastCommandPollTime = 0;
    lastRegistrationAttemptTime = 0;

    nodeRegistered = registerNode();

    if (nodeRegistered) {
      sendHeartbeat();
      lastHeartbeatTime = millis();
      logSensor("Sensor online.");
    } else {
      logSensor("Registration failed, but Wi-Fi is connected. Will retry in loop.");
    }
  } else {
    Serial.println("Wi-Fi failed. Returning to setup mode.");
    clearAssignmentSettingsOnly();
    delay(1000);
    ESP.restart();
  }
}

void handleWifiReconnect() {
  if (wifiName.length() == 0) return;
  if (setupModeStarted) return;
  if (firmwareUpdateInProgress) return;

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (millis() - lastReconnectAttempt >= RECONNECT_INTERVAL_MS) {
    lastReconnectAttempt = millis();
    nodeRegistered = false;

    Serial.println("Wi-Fi disconnected. Reconnecting...");
    WiFi.disconnect();
    WiFi.begin(wifiName.c_str(), wifiPassword.c_str());
  }
}

bool registerNode() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  generateHardwareIds();

  logSensor("Registering node...");

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(30000);
  http.begin(client, REGISTER_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-webhook-secret", WEBHOOK_SECRET);

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
  payload += "\"softwareVersion\":\"" + String(SOFTWARE_VERSION) + "\"";
  payload += "}";

  int responseCode = http.POST(payload);

  Serial.print(sensorLabel() + " | Registration response code: ");
  Serial.println(responseCode);

  http.end();

  return responseCode >= 200 && responseCode < 300;
}

bool sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  generateHardwareIds();

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(30000);
  http.begin(client, HEARTBEAT_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-webhook-secret", WEBHOOK_SECRET);

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
  payload += "\"softwareVersion\":\"" + String(SOFTWARE_VERSION) + "\",";
  payload += "\"monitorStatus\":\"Online\",";
  payload += "\"ffmpegStatus\":\"Not Applicable\",";
  payload += "\"platform\":\"ESP32\",";
  payload += "\"hostname\":\"" + nodeId + "\",";
  payload += "\"activeMonitorCount\":1,";
  payload += "\"diagnostics\":{";
  payload += "\"sourceKey\":\"" + sourceKey + "\",";
  payload += "\"setupId\":\"" + setupId + "\",";
  payload += "\"assignmentState\":\"" + assignmentState() + "\",";
  payload += "\"wifiSsid\":\"" + jsonEscape(WiFi.SSID()) + "\",";
  payload += "\"deviceName\":\"" + jsonEscape(deviceName) + "\",";
  payload += "\"roomName\":\"" + jsonEscape(roomName) + "\",";
  payload += "\"residentName\":\"" + jsonEscape(residentName) + "\",";
  payload += "\"wifiRssi\":" + String(WiFi.RSSI());
  payload += "}";
  payload += "}";

  int responseCode = http.POST(payload);

  Serial.print(sensorLabel() + " | Heartbeat response code: ");
  Serial.println(responseCode);

  http.end();

  return responseCode >= 200 && responseCode < 300;
}

void pollSensorCommands() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  generateHardwareIds();

  String url = String(BASE_URL) + "/sensor-commands/" + nodeId + "/pending";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(30000);
  http.begin(client, url);
  http.addHeader("x-webhook-secret", WEBHOOK_SECRET);

  int responseCode = http.GET();

  if (responseCode != 200) {
    Serial.print(sensorLabel() + " | Command poll response code: ");
    Serial.println(responseCode);
    http.end();
    return;
  }

  String response = http.getString();
  http.end();

  if (response.indexOf("\"count\":0") >= 0) {
    return;
  }

  logSensor("Command response received.");

  String commandId = extractJsonString(response, "commandId");
  String commandType = extractJsonString(response, "commandType");

  logSensor("Command type: " + commandType);

  if (commandType == "reconfigure") {
    reportCommandResult(commandId, "success", "ESP32 reconfigure started.");
    clearSettingsAndRestart();
    return;
  }

  if (commandType == "update_firmware") {
    String firmwareUrl = extractJsonString(response, "firmwareUrl");

    if (firmwareUrl.length() == 0) {
      firmwareUrl = extractJsonString(response, "url");
    }

    if (firmwareUrl.length() == 0) {
      reportCommandResult(commandId, "failed", "Firmware update failed: missing firmwareUrl in command payload.");
      return;
    }

    performFirmwareUpdate(commandId, firmwareUrl);
    return;
  }

  if (commandType == "identify" || commandType == "locate") {
    reportCommandResult(commandId, "running", "ESP32 identify LED started.");
    blinkIdentifyLed(12000);
    reportCommandResult(commandId, "success", "ESP32 identify LED completed.");
    return;
  }

  if (commandType == "ping") {
    reportCommandResult(commandId, "success", "Ping received by ESP32.");
    return;
  }

  reportCommandResult(commandId, "failed", "Unknown command type for this ESP32 firmware.");
}

String extractJsonString(String json, String key) {
  String pattern = "\"" + key + "\":\"";
  int start = json.indexOf(pattern);

  if (start < 0) {
    return "";
  }

  start += pattern.length();

  int end = json.indexOf("\"", start);

  if (end < 0) {
    return "";
  }

  return json.substring(start, end);
}

void reportCommandResult(String commandId, String status, String message) {
  if (commandId.length() == 0) {
    return;
  }

  generateHardwareIds();

  String url = String(BASE_URL) + "/sensor-commands/" + commandId + "/result";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(30000);
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-webhook-secret", WEBHOOK_SECRET);

  String payload = "{";
  payload += "\"status\":\"" + status + "\",";
  payload += "\"result\":{";
  payload += "\"message\":\"" + jsonEscape(message) + "\",";
  payload += "\"nodeId\":\"" + nodeId + "\",";
  payload += "\"setupId\":\"" + setupId + "\",";
  payload += "\"softwareVersion\":\"" + String(SOFTWARE_VERSION) + "\"";
  payload += "}";
  payload += "}";

  int responseCode = http.POST(payload);

  Serial.print(sensorLabel() + " | Command result response code: ");
  Serial.println(responseCode);

  http.end();
}

void performFirmwareUpdate(String commandId, String firmwareUrl) {
  if (WiFi.status() != WL_CONNECTED) {
    reportCommandResult(commandId, "failed", "Firmware update failed: Wi-Fi is offline.");
    return;
  }

  firmwareUpdateInProgress = true;

  logSensor("Firmware update requested.");
  logSensor("Firmware URL: " + firmwareUrl);


  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(120000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  bool beginOk = http.begin(client, firmwareUrl);

  if (!beginOk) {
    firmwareUpdateInProgress = false;
    reportCommandResult(commandId, "failed", "Firmware update failed: could not open firmware URL.");
    return;
  }

  int responseCode = http.GET();

  if (responseCode != HTTP_CODE_OK) {
    String message = "Firmware update failed: download returned HTTP " + String(responseCode) + ".";
    http.end();
    firmwareUpdateInProgress = false;
    reportCommandResult(commandId, "failed", message);
    return;
  }

  int contentLength = http.getSize();

  if (contentLength == 0) {
    http.end();
    firmwareUpdateInProgress = false;
    reportCommandResult(commandId, "failed", "Firmware update failed: firmware file was empty.");
    return;
  }

  Serial.print(sensorLabel() + " | Firmware size: ");
  Serial.println(contentLength);

  bool canBegin = false;

  if (contentLength > 0) {
    canBegin = Update.begin(contentLength);
  } else {
    canBegin = Update.begin(UPDATE_SIZE_UNKNOWN);
  }

  if (!canBegin) {
    String message = "Firmware update failed: not enough OTA space. Error " + String(Update.getError()) + ".";
    http.end();
    firmwareUpdateInProgress = false;
    reportCommandResult(commandId, "failed", message);
    return;
  }

  WiFiClient* stream = http.getStreamPtr();

  size_t written = Update.writeStream(*stream);

  Serial.print(sensorLabel() + " | Firmware bytes written: ");
  Serial.println(written);

  if (contentLength > 0 && written != (size_t)contentLength) {
    String message = "Firmware update failed: wrote " + String(written) + " of " + String(contentLength) + " bytes.";
    Update.abort();
    http.end();
    firmwareUpdateInProgress = false;
    reportCommandResult(commandId, "failed", message);
    return;
  }

  bool updateEnded = Update.end();

  if (!updateEnded) {
    String message = "Firmware update failed during finalization. Error " + String(Update.getError()) + ".";
    http.end();
    firmwareUpdateInProgress = false;
    reportCommandResult(commandId, "failed", message);
    return;
  }

  if (!Update.isFinished()) {
    http.end();
    firmwareUpdateInProgress = false;
    reportCommandResult(commandId, "failed", "Firmware update failed: update did not finish.");
    return;
  }

  http.end();

  reportCommandResult(commandId, "success", "Firmware update installed. ESP32 rebooting.");

  Serial.println(sensorLabel() + " | Firmware update complete. Rebooting...");
  delay(2000);
  ESP.restart();
}

void handleMotionSensor() {
  int motion = digitalRead(PIR_PIN);

  if (motion != lastPirState) {
    lastPirState = motion;
    logSensor("PIR changed to " + String(motion));
  }

  if (motion == MOTION_ACTIVE_STATE && !motionAlreadyReported) {
    logSensor("Motion detected. Sending webhook...");
    sendMotionEvent();
    motionAlreadyReported = true;
    lastNoMotionTime = 0;
  }

  if (motion != MOTION_ACTIVE_STATE && motionAlreadyReported) {
    if (lastNoMotionTime == 0) {
      lastNoMotionTime = millis();
    }

    if (millis() - lastNoMotionTime > RESET_AFTER_NO_MOTION_MS) {
      logSensor("Motion reset.");
      motionAlreadyReported = false;
      lastNoMotionTime = 0;
    }
  }

  if (motion == MOTION_ACTIVE_STATE) {
    lastNoMotionTime = 0;
  }
}

void sendMotionEvent() {
  if (WiFi.status() != WL_CONNECTED) {
    logSensor("Wi-Fi offline. Motion event skipped.");
    return;
  }

  generateHardwareIds();

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(30000);
  http.begin(client, WEBHOOK_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-webhook-secret", WEBHOOK_SECRET);

  String fullSourceName = deviceName + " - " + roomName;
  String message = "Motion detected from " + fullSourceName;

  String payload = "{";
  payload += "\"nodeId\":\"" + nodeId + "\",";
  payload += "\"locationName\":\"" + jsonEscape(locationName) + "\",";
  payload += "\"sourceKey\":\"" + sourceKey + "\",";
  payload += "\"sourceName\":\"" + jsonEscape(fullSourceName) + "\",";
  payload += "\"residentName\":\"" + jsonEscape(residentName) + "\",";
  payload += "\"message\":\"" + jsonEscape(message) + "\",";
  payload += "\"alertLevel\":\"Normal\",";
  payload += "\"timeText\":\"ESP32 Motion Event\"";
  payload += "}";

  int responseCode = http.POST(payload);

  Serial.print(sensorLabel() + " | Webhook response code: ");
  Serial.println(responseCode);

  http.end();
}

void handleRoot() {
  int networkCount = WiFi.scanNetworks();
  String knownWifiMap = savedWifiJavaScriptMap();

  String html = "";
  html += "<html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Good Shepherd Sensor Activation</title>";
  html += "<style>";
  html += "body{font-family:-apple-system,BlinkMacSystemFont,Arial,sans-serif;background:#f4f7fb;margin:0;padding:20px;color:#142033;}";
  html += ".card{max-width:560px;margin:0 auto;background:white;border-radius:18px;padding:20px;box-shadow:0 8px 24px rgba(20,32,51,.12);}";
  html += "h1{margin:0 0 8px;font-size:26px;}";
  html += "p{color:#526173;line-height:1.35;}";
  html += "label{display:block;font-weight:700;margin-top:14px;margin-bottom:6px;}";
  html += "input,select{box-sizing:border-box;width:100%;font-size:20px;padding:12px;border:1px solid #ccd6e0;border-radius:10px;background:white;}";
  html += "button{width:100%;font-size:20px;padding:14px;margin-top:18px;background:#168cff;color:white;border:0;border-radius:12px;font-weight:700;}";
  html += ".meta{font-size:13px;background:#eef4fb;padding:10px;border-radius:10px;color:#526173;}";
  html += ".hint{font-size:13px;color:#526173;}";
  html += "a{color:#168cff;text-decoration:none;font-weight:700;}";
  html += "</style>";
  html += "</head>";
  html += "<body><div class='card'>";
  html += "<h1>Good Shepherd Sensor Activation</h1>";
  html += "<p>Activate this motion sensor.</p>";
  html += "<div class='meta'><b>Setup ID:</b><br>" + htmlEscape(setupId) + "<br><br><b>Hardware ID:</b><br>" + htmlEscape(nodeId) + "<br><br><b>Firmware:</b><br>" + String(SOFTWARE_VERSION) + "</div>";

  html += "<form method='POST' action='/save'>";

  html += "<label>Wi-Fi Network</label>";
  html += "<select id='wifiName' name='wifiName'>";

  if (networkCount <= 0) {
    html += "<option value=''>No Wi-Fi networks found</option>";
  } else {
    for (int i = 0; i < networkCount; i++) {
      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      String selected = ssid == wifiName ? " selected" : "";
      html += "<option value='" + htmlEscape(ssid) + "'" + selected + ">" + htmlEscape(ssid) + " (" + String(rssi) + " dBm)</option>";
    }
  }

  html += "</select>";

  html += "<label>Wi-Fi Password</label>";
  html += "<input id='wifiPassword' name='wifiPassword' type='password' value='" + htmlEscape(savedWifiPasswordFor(wifiName)) + "' required>";
  html += "<div class='hint'>If this Wi-Fi was used before, the password will fill automatically when selected.</div>";

  html += "<label>Home / Location Name</label>";
  html += "<input name='locationName' value='" + htmlEscape(locationName) + "' required>";

  html += "<label>Resident Name</label>";
  html += "<input name='residentName' value='" + htmlEscape(residentName) + "' required>";

  html += "<label>Room Name</label>";
  html += "<input name='roomName' value='" + htmlEscape(roomName) + "' required>";

  html += "<label>Device Name</label>";
  html += "<input name='deviceName' value='" + htmlEscape(deviceName) + "' required>";

  html += "<button type='submit'>Activate Sensor</button>";

  html += "</form>";
  html += "<p class='hint'><a href='/'>Refresh Wi-Fi List</a> &nbsp; | &nbsp; <a href='/reset'>Clear assignment</a></p>";
  html += "<p class='hint'><a href='/factory-reset'>Factory reset saved Wi-Fi history</a></p>";

  html += "<script>";
  html += "var knownWifiPasswords=" + knownWifiMap + ";";
  html += "var wifiSelect=document.getElementById('wifiName');";
  html += "var passwordField=document.getElementById('wifiPassword');";
  html += "function autofillWifiPassword(){var ssid=wifiSelect.value;if(knownWifiPasswords[ssid]){passwordField.value=knownWifiPasswords[ssid];}}";
  html += "wifiSelect.addEventListener('change',autofillWifiPassword);";
  html += "autofillWifiPassword();";
  html += "</script>";

  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

void handleSave() {
  wifiName = server.arg("wifiName");
  wifiPassword = server.arg("wifiPassword");

  locationName = server.arg("locationName");
  residentName = server.arg("residentName");
  roomName = server.arg("roomName");
  deviceName = server.arg("deviceName");

  saveSettings();

  String html = "";
  html += "<html><body style='font-family:Arial;padding:20px;'>";
  html += "<h1>Good Shepherd Setup</h1>";
  html += "<p>Sensor activated. Rebooting now.</p>";
  html += "</body></html>";

  server.send(200, "text/html", html);

  delay(2000);
  ESP.restart();
}

void handleReset() {
  clearAssignmentSettingsOnly();

  String html = "";
  html += "<html><body style='font-family:Arial;padding:20px;'>";
  html += "<h1>Good Shepherd Setup</h1>";
  html += "<p>Assignment settings cleared. Saved Wi-Fi passwords were kept. Rebooting...</p>";
  html += "</body></html>";

  server.send(200, "text/html", html);

  delay(2000);
  ESP.restart();
}

void handleFactoryReset() {
  factoryClearEverything();

  String html = "";
  html += "<html><body style='font-family:Arial;padding:20px;'>";
  html += "<h1>Good Shepherd Setup</h1>";
  html += "<p>Factory reset complete. Saved Wi-Fi history was also cleared. Rebooting...</p>";
  html += "</body></html>";

  server.send(200, "text/html", html);

  delay(2000);
  ESP.restart();
}
