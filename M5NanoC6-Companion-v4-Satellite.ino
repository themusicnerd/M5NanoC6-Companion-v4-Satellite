/*
 * M5NanoC6 Companion v4 Satellite
 * Version 0.1.5
 *
 * Wi-Fi Companion satellite, button, full-range WS2812 RGB tally and NEC IR.
 * The ESP32-C6 802.15.4 capabilities are reported by the REST API so future
 * Zigbee/Thread/Matter firmware profiles can retain the same control surface.
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <Adafruit_NeoPixel.h>
#include <esp_mac.h>
#include <esp32-hal-rmt.h>

#define FIRMWARE_VERSION "0.1.5"
#define BUTTON_PIN 9
#define IR_TX_PIN 3
#define RGB_POWER_PIN 19
#define RGB_DATA_PIN 20

Preferences preferences;
WiFiManager wifiManager;
WiFiClient companionClient;
WebServer server(9999);
Adafruit_NeoPixel rgb(1, RGB_DATA_PIN, NEO_GRB + NEO_KHZ800);

char companionHost[64] = "Companion IP";
char companionPort[6] = "16622";
String deviceID;
String updatePassword;
int brightness = 100;
uint8_t tallyR = 0, tallyG = 0, tallyB = 0;
bool tallyActive = false;
bool companionConnected = false;
bool buttonDown = false;
bool configPortalActive = false;
bool mdnsStarted = false;
unsigned long lastConnectTry = 0;
unsigned long lastPing = 0;
unsigned long lastLedFrame = 0;
unsigned long setupButtonPressedAt = 0;
const unsigned long setupGestureWindowMs = 60000;
const unsigned long setupGestureHoldMs = 5000;
bool setupGestureHandled = false;
String receiveLine;
WiFiManagerParameter *portalHost = nullptr;
WiFiManagerParameter *portalPort = nullptr;

static uint8_t scaleChannel(uint8_t value) {
  return (uint16_t(value) * constrain(brightness, 0, 100)) / 100;
}

static void showRgb(uint8_t r, uint8_t g, uint8_t b) {
  rgb.setPixelColor(0, rgb.Color(scaleChannel(r), scaleChannel(g), scaleChannel(b)));
  rgb.show();
}

static void renderLed() {
  if (configPortalActive) return;
  if (WiFi.status() != WL_CONNECTED || !companionClient.connected()) {
    if ((millis() / 500) & 1) showRgb(255, 0, 0);
    else showRgb(0, 0, 255);
  } else if (tallyActive) showRgb(tallyR, tallyG, tallyB);
  else showRgb(0, 180, 0);
}

static void updateLedAnimation() {
  const unsigned long frameInterval = configPortalActive ? 100 : 40;
  if (millis() - lastLedFrame < frameInterval) return;
  lastLedFrame = millis();
  if (!configPortalActive) {
    renderLed();
    return;
  }

  if ((millis() / 100) & 1) showRgb(0, 0, 255);
  else showRgb(0, 0, 0);
}

static String jsonValue(const String &body, const char *key) {
  const String marker = "\"" + String(key) + "\"";
  int p = body.indexOf(marker);
  if (p < 0 || (p = body.indexOf(':', p + marker.length())) < 0) return "";
  p++;
  while (p < (int)body.length() && isspace((unsigned char)body[p])) p++;
  if (body[p] == '"') {
    int end = body.indexOf('"', ++p);
    return end < 0 ? "" : body.substring(p, end);
  }
  int end = p;
  while (end < (int)body.length() && body[end] != ',' && body[end] != '}') end++;
  String value = body.substring(p, end);
  value.trim();
  return value;
}

static bool requireAuth() {
  if (!updatePassword.length() || server.authenticate("admin", updatePassword.c_str())) return true;
  server.requestAuthentication();
  return false;
}

static void saveSettings() {
  preferences.begin("companion", false);
  preferences.putString("host", companionHost);
  preferences.putString("port", companionPort);
  preferences.putInt("brightness", brightness);
  preferences.end();
}

static void sendSettings() {
  String body = "{\"device\":\"M5NanoC6\",\"firmware\":\"" FIRMWARE_VERSION "\",";
  body += "\"brightness\":" + String(brightness) + ",";
  body += "\"ir\":{\"supported\":true,\"protocols\":[\"NEC\"]},";
  body += "\"radio\":{\"hardware\":\"802.15.4\",\"zigbee\":\"profile-required\",";
  body += "\"thread\":\"profile-required\",\"matter\":\"profile-required\"}}";
  server.send(200, "application/json", body);
}

static void postSettings() {
  String value = jsonValue(server.arg("plain"), "brightness");
  if (value.length()) {
    brightness = constrain(value.toInt(), 0, 100);
    saveSettings();
    renderLed();
  }
  sendSettings();
}

static String requestValue(const char *key) {
  String body = server.arg("plain");
  body.trim();
  if (body.startsWith("{")) return jsonValue(body, key);
  return body;
}

static void getHost() {
  server.send(200, "text/plain", companionHost);
}

static void getPort() {
  server.send(200, "text/plain", companionPort);
}

static void getConfig() {
  server.send(200, "application/json",
    "{\"host\":\"" + String(companionHost) + "\",\"port\":" + String(companionPort) + "}");
}

static void postHost() {
  String value = requestValue("host");
  value.trim();
  if (!value.length() || value.length() >= sizeof(companionHost)) {
    server.send(400, "text/plain", "Invalid host");
    return;
  }
  strlcpy(companionHost, value.c_str(), sizeof(companionHost));
  if (portalHost) portalHost->setValue(companionHost, sizeof(companionHost));
  saveSettings();
  companionClient.stop();
  server.send(200, "text/plain", "OK");
}

static void postPort() {
  String value = requestValue("port");
  value.trim();
  const long port = value.toInt();
  if (port < 1 || port > 65535) {
    server.send(400, "text/plain", "Invalid port");
    return;
  }
  snprintf(companionPort, sizeof(companionPort), "%ld", port);
  if (portalPort) portalPort->setValue(companionPort, sizeof(companionPort));
  saveSettings();
  companionClient.stop();
  server.send(200, "text/plain", "OK");
}

static void initIr() {
  rmtInit(IR_TX_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_2, 1000000);
  rmtSetCarrier(IR_TX_PIN, true, LOW, 38000, 0.33);
}

static void sendNec(uint16_t address, uint16_t command, uint8_t repeats) {
  rmt_data_t data[34];
  data[0] = {9000, 1, 4500, 0};
  uint32_t payload = uint32_t(address) | (uint32_t(command) << 16);
  for (uint8_t i = 0; i < 32; i++)
    data[i + 1] = {560, 1, uint16_t((payload & (1UL << i)) ? 1690 : 560), 0};
  data[33] = {560, 1, 10000, 0};
  rmtWrite(IR_TX_PIN, data, 34, RMT_WAIT_FOR_EVER);
  for (uint8_t i = 0; i < repeats; i++) {
    delay(40);
    rmt_data_t repeat[2] = {{9000, 1, 2250, 0}, {560, 1, 10000, 0}};
    rmtWrite(IR_TX_PIN, repeat, 2, RMT_WAIT_FOR_EVER);
  }
}

static uint32_t parseNumber(String value, bool &ok) {
  value.trim();
  char *end = nullptr;
  uint32_t result = strtoul(value.c_str(), &end, 0);
  ok = end != value.c_str() && *end == '\0';
  return result;
}

static void postIrNec() {
  const String body = server.arg("plain");
  bool addressOk = false, commandOk = false;
  uint32_t address = parseNumber(jsonValue(body, "address"), addressOk);
  uint32_t command = parseNumber(jsonValue(body, "command"), commandOk);
  int repeats = constrain(jsonValue(body, "repeats").toInt(), 0, 10);
  if (!addressOk || !commandOk || address > 0xffff || command > 0xffff) {
    server.send(400, "application/json", "{\"error\":\"address and command must be 0..65535 (decimal or 0xhex)\"}");
    return;
  }
  sendNec(address, command, repeats);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void postRadio() {
  server.send(409, "application/json",
    "{\"error\":\"Wi-Fi satellite profile cannot commission an 802.15.4 stack\","
    "\"profiles\":[\"zigbee\",\"thread-matter\"]}");
}

static void updatePage() {
  if (!requireAuth()) return;
  server.send(200, "text/html",
    "<!doctype html><meta name=viewport content='width=device-width'><h1>M5NanoC6 v" FIRMWARE_VERSION "</h1>"
    "<form method=POST enctype=multipart/form-data><input type=file name=firmware accept=.bin required>"
    "<button>Install upgrade</button></form>");
}

static void updateUpload() {
  if (updatePassword.length() && !server.authenticate("admin", updatePassword.c_str())) return;
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
  else if (upload.status == UPLOAD_FILE_WRITE) Update.write(upload.buf, upload.currentSize);
  else if (upload.status == UPLOAD_FILE_END) Update.end(true);
  else if (upload.status == UPLOAD_FILE_ABORTED) Update.abort();
}

static void updateResult() {
  if (!requireAuth()) return;
  bool ok = !Update.hasError();
  server.send(ok ? 200 : 500, "text/plain", ok ? "Update complete; rebooting." : "Update failed.");
  if (ok) { delay(500); ESP.restart(); }
}

static void configPage() {
  server.send(200, "text/html",
    "<!doctype html><meta name=viewport content='width=device-width'><title>M5NanoC6</title>"
    "<h1>M5NanoC6 Companion Satellite</h1><p>Device ID: <code>" + deviceID + "</code></p>"
    "<p>Firmware v" FIRMWARE_VERSION "</p><h3>Live troubleshooting status</h3><div id=s>Loading...</div>"
    "<p>Incoming text: <code id=t>(not supported)</code></p>"
    "<p>Incoming colour: <span id=w style='display:inline-block;width:2em;height:1em;border:1px solid'></span> <code id=c>-</code></p>"
    "<p><a href=/update>Firmware update</a></p><pre id=j></pre><script>async function u(){try{let x=await(await fetch('/api/status')).json();"
    "s.textContent=(x.networkConnected?'Network connected':'Network disconnected')+' | '+(x.companionConnected?'Companion connected':'Companion disconnected')+' | '+x.ip;"
    "let q=x.color;c.textContent=`rgb(${q.r}, ${q.g}, ${q.b})`;w.style.background=`rgb(${q.r},${q.g},${q.b})`;j.textContent=JSON.stringify(x,null,2)"
    "}catch(e){s.textContent='Status unavailable'}}u();setInterval(u,2000)</script>");
}

static void sendStatus() {
  String body = "{\"deviceName\":\"M5NanoC6\",\"deviceId\":\"" + deviceID + "\",\"firmware\":\"" FIRMWARE_VERSION "\",";
  body += "\"network\":\"wifi\",\"networkConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  body += "\"ssid\":\"" + WiFi.SSID() + "\",\"ip\":\"" + WiFi.localIP().toString() + "\",";
  body += "\"companionConnected\":" + String(companionClient.connected() ? "true" : "false") + ",";
  body += "\"companion\":\"" + String(companionHost) + ":" + companionPort + "\",\"text\":\"\",";
  body += "\"brightness\":" + String(brightness) + ",\"color\":{\"r\":" + String(tallyR) +
    ",\"g\":" + String(tallyG) + ",\"b\":" + String(tallyB) + "},";
  body += "\"buttonPressed\":" + String(buttonDown ? "true" : "false") +
    ",\"setupMode\":" + String(configPortalActive ? "true" : "false") +
    ",\"uptimeSeconds\":" + String(millis() / 1000) + "}";
  server.send(200, "application/json", body);
}

static void setupServer() {
  server.on("/", HTTP_GET, configPage);
  server.on("/api/host", HTTP_GET, getHost);
  server.on("/api/host", HTTP_POST, postHost);
  server.on("/api/port", HTTP_GET, getPort);
  server.on("/api/port", HTTP_POST, postPort);
  server.on("/api/config", HTTP_GET, getConfig);
  server.on("/api/settings", HTTP_GET, sendSettings);
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/settings", HTTP_POST, postSettings);
  server.on("/api/ir/nec", HTTP_POST, postIrNec);
  server.on("/api/radio", HTTP_GET, sendSettings);
  server.on("/api/radio", HTTP_POST, postRadio);
  server.on("/update", HTTP_GET, updatePage);
  server.on("/update", HTTP_POST, updateResult, updateUpload);
  server.begin();
}

static void savePortalSettings() {
  if (portalHost) strlcpy(companionHost, portalHost->getValue(), sizeof(companionHost));
  if (portalPort) strlcpy(companionPort, portalPort->getValue(), sizeof(companionPort));
  saveSettings();
}

static void startConfigPortal() {
  if (configPortalActive) return;
  Serial.println("[Setup] Starting Wi-Fi configuration AP: " + deviceID);
  companionClient.stop();
  tallyActive = false;
  if (!portalHost) {
    portalHost = new WiFiManagerParameter("companionIP", "Companion IP", companionHost, 63);
    portalPort = new WiFiManagerParameter("companionPort", "Satellite port", companionPort, 5);
    wifiManager.addParameter(portalHost);
    wifiManager.addParameter(portalPort);
    wifiManager.setSaveParamsCallback(savePortalSettings);
  }
  wifiManager.setConfigPortalBlocking(false);
  configPortalActive = true;
  const bool started = wifiManager.startConfigPortal(deviceID.c_str());
  configPortalActive = wifiManager.getConfigPortalActive();
  Serial.printf("[Setup] AP start returned %s; portal active: %s\n",
    started ? "true" : "false", configPortalActive ? "true" : "false");
  updateLedAnimation();
}

static String companionSurfaceID() {
  return "m5nano-c6:" + deviceID.substring(deviceID.length() - 5);
}

static void ensureMDNS() {
  if (mdnsStarted || WiFi.status() != WL_CONNECTED) return;

  const String shortId = deviceID.substring(deviceID.length() - 5);
  String hostname = "m5nano-c6-" + shortId;
  hostname.toLowerCase();
  const String instanceName = "m5nano-c6:" + shortId;

  if (!MDNS.begin(hostname.c_str())) {
    Serial.println("[mDNS] Failed to start responder");
    return;
  }

  MDNS.setInstanceName(instanceName);
  if (!MDNS.addService("companion-satellite", "tcp", 9999)) {
    Serial.println("[mDNS] Failed to advertise companion-satellite service");
    MDNS.end();
    return;
  }

  MDNS.addServiceTxt("companion-satellite", "tcp", "restEnabled", "true");
  MDNS.addServiceTxt("companion-satellite", "tcp", "deviceId", shortId.c_str());
  MDNS.addServiceTxt("companion-satellite", "tcp", "prefix", "m5nano-c6");
  MDNS.addServiceTxt("companion-satellite", "tcp", "productName", "M5NanoC6");
  MDNS.addServiceTxt("companion-satellite", "tcp", "apiVersion", "4");
  mdnsStarted = true;
  Serial.printf("[mDNS] Discovery ready: %s.local (%s)\n",
    hostname.c_str(), instanceName.c_str());
}

static void sendDeviceAdd() {
  companionClient.println("ADD-DEVICE DEVICEID=" + companionSurfaceID() +
    " PRODUCT_NAME=\"M5NanoC6\" KEYS_TOTAL=1 KEYS_PER_ROW=1 "
    "BITMAPS=0 COLORS=rgb TEXT=false");
}

static void handleKeyState(const String &line) {
  int p = line.indexOf("COLOR=");
  if (p >= 0) {
    int start = line.indexOf("rgba(", p);
    int end = line.indexOf(')', start);
    if (start >= 0 && end > start) {
      String value = line.substring(start + 5, end);
      int a = value.indexOf(','), b = value.indexOf(',', a + 1);
      if (a > 0 && b > a) {
        tallyR = constrain(value.substring(0, a).toInt(), 0, 255);
        tallyG = constrain(value.substring(a + 1, b).toInt(), 0, 255);
        tallyB = constrain(value.substring(b + 1).toInt(), 0, 255);
        tallyActive = true;
        renderLed();
      }
    }
  }
}

static void handleApiLine(String line) {
  line.trim();
  if (line.startsWith("KEY-STATE")) handleKeyState(line);
  else if (line.startsWith("COLOR")) {
    int first = line.indexOf(' ');
    if (first >= 0) handleKeyState("KEY-STATE COLOR=" + line.substring(first + 1));
  } else if (line.startsWith("BRIGHTNESS")) {
    int equals = line.indexOf('=');
    brightness = constrain((equals >= 0 ? line.substring(equals + 1) : line.substring(10)).toInt(), 0, 100);
    saveSettings();
    renderLed();
  } else if (line == "PING") companionClient.print("PONG\n");
}

static void connectCompanion() {
  if (WiFi.status() != WL_CONNECTED || companionClient.connected()) return;
  unsigned long now = millis();
  if (now - lastConnectTry < 5000) return;
  lastConnectTry = now;
  if (companionClient.connect(companionHost, atoi(companionPort))) {
    companionConnected = true;
    sendDeviceAdd();
    renderLed();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(RGB_POWER_PIN, OUTPUT);
  digitalWrite(RGB_POWER_PIN, HIGH);
  rgb.begin();
  rgb.clear();
  rgb.show();
  initIr();

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char id[32];
  snprintf(id, sizeof(id), "M5NANOC6_%02X%02X%02X%02X%02X%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  deviceID = id;

  preferences.begin("companion", true);
  strlcpy(companionHost, preferences.getString("host", "Companion IP").c_str(), sizeof(companionHost));
  strlcpy(companionPort, preferences.getString("port", "16622").c_str(), sizeof(companionPort));
  brightness = preferences.getInt("brightness", 100);
  updatePassword = preferences.getString("updatepass", "");
  preferences.end();

  WiFi.mode(WIFI_STA);
  WiFi.begin();
  renderLed();

  ArduinoOTA.setHostname(deviceID.c_str());
  ArduinoOTA.setPassword("companion-satellite");
  // Discovery mDNS is started after Wi-Fi connects by ensureMDNS().
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.begin();
  setupServer();
  renderLed();
}

void loop() {
  if (configPortalActive) {
    wifiManager.process();
    if (!wifiManager.getConfigPortalActive()) {
      configPortalActive = false;
      savePortalSettings();
    }
  }
  server.handleClient();
  ArduinoOTA.handle();
  ensureMDNS();
  connectCompanion();
  updateLedAnimation();

  if (companionClient.connected()) {
    while (companionClient.available()) {
      char c = companionClient.read();
      if (c == '\n') { handleApiLine(receiveLine); receiveLine = ""; }
      else if (c != '\r' && receiveLine.length() < 1024) receiveLine += c;
    }
    if (millis() - lastPing > 2000) {
      lastPing = millis();
      companionClient.print("PING\n");
    }
  } else if (companionConnected) {
    companionConnected = false;
    tallyActive = false;
    renderLed();
  }

  bool pressed = digitalRead(BUTTON_PIN) == LOW;
  const unsigned long now = millis();
  const bool setupWindowOpen = now <= setupGestureWindowMs;

  if (!configPortalActive && !setupGestureHandled && setupWindowOpen) {
    if (pressed) {
      if (setupButtonPressedAt == 0) setupButtonPressedAt = now;
      if (now - setupButtonPressedAt >= setupGestureHoldMs) {
        setupGestureHandled = true;
        if (buttonDown && companionClient.connected()) {
          companionClient.println(
            "KEY-PRESS DEVICEID=" + companionSurfaceID() + " KEY=0 PRESSED=false");
        }
        buttonDown = false;
        startConfigPortal();
        delay(2);
        return;
      }
    } else {
      setupButtonPressedAt = 0;
    }
  }

  if (configPortalActive) {
    buttonDown = pressed;
    delay(2);
    return;
  }

  if (pressed && !buttonDown) {
    buttonDown = true;
    if (companionClient.connected()) companionClient.println(
      "KEY-PRESS DEVICEID=" + companionSurfaceID() + " KEY=0 PRESSED=true");
  }
  if (!pressed && buttonDown) {
    buttonDown = false;
    if (companionClient.connected())
      companionClient.println(
        "KEY-PRESS DEVICEID=" + companionSurfaceID() + " KEY=0 PRESSED=false");
  }
  delay(2);
}
