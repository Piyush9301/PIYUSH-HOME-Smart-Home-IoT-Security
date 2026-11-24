/*
 * ============================================================================
 *  PIYUSH HOME — Smart IoT & Automation ESP32 Firmware
 * ============================================================================
 *  Compatible with: piyush_home.html (PIYUSH HOME Web Dashboard)
 *
 *  HARDWARE CONNECTIONS (PINOUT):
 *  ---------------------------------------------------------------------------
 *  Component                  | ESP32 GPIO | Notes
 *  ---------------------------------------------------------------------------
 *  DHT22 (Temp & Humidity)    | GPIO 4     | Use 10k pull-up resistor to 3.3V
 *  MQ-2 (Gas / Smoke Analog)  | GPIO 34    | ADC1 (Input only, WiFi-safe)*
 *  MQ-135 (Air Quality)       | GPIO 35    | ADC1 (Input only, WiFi-safe)*
 *  PIR Motion (HC-SR501)      | GPIO 19    | Digital OUT (3.3V logic)
 *  20x4 I2C LCD (SDA)         | GPIO 21    | Default ESP32 I2C SDA
 *  20x4 I2C LCD (SCL)         | GPIO 22    | Default ESP32 I2C SCL (Addr 0x27)
 *  Switch 1 (Living Light)    | GPIO 26    | Relay Channel 1
 *  Switch 2 (Ceiling Fan)     | GPIO 27    | Relay Channel 2
 *  Switch 3 (Bedroom AC)      | GPIO 14    | Relay Channel 3
 *  Switch 4 (Porch Light)     | GPIO 12    | Relay Channel 4**
 *  Switch 5 (Water Pump)      | GPIO 13    | Relay Channel 5
 *  WiFi Reset Button          | GPIO 0     | Built-in ESP32 BOOT btn (Hold 3s)
 *  ---------------------------------------------------------------------------
 *  * IMPORTANT: MQ-2 and MQ-135 sensors operate at 5V and can output up to 5V
 *    on their AO pin. Use a simple voltage divider (e.g., 10k + 20k ohms) to
 *    keep the analog voltage <= 3.3V on ESP32 GPIO 34 and GPIO 35.
 *  ** NOTE on GPIO 12: GPIO 12 is an ESP32 boot strapping pin. If your relay
 *    board pulls GPIO 12 HIGH during boot and prevents booting, move Switch 4
 *    to GPIO 25 or GPIO 33.
 *
 *  REQUIRED ARDUINO IDE LIBRARIES (Install via Library Manager):
 *    1. "DHT sensor library" by Adafruit (and Adafruit Unified Sensor)
 *    2. "LiquidCrystal I2C" by Frank de Brabander
 *    3. "SinricPro" by Boris Jaeger (also installs WebSockets & ArduinoJson)
 *    (WiFi Provisioning & NVS Save use built-in ESP32 Preferences & DNSServer)
 *
 *  ESP32-CAM CCTV SETUP (SEPARATE BOARD):
 *    Flash the built-in Arduino IDE example:
 *      File -> Examples -> ESP32 -> Camera -> CameraWebServer
 *    Select your camera model (e.g., CAMERA_MODEL_AI_THINKER), enter your WiFi,
 *    and enter its stream URL (http://<CAM-IP>:81/stream) in the dashboard's
 *    "ESP32 Setup" modal.
 * ============================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <SinricPro.h>
#include <SinricProSwitch.h>
#include <SinricProTemperaturesensor.h>
#include <SinricProMotionsensor.h>

// ============================================================================
// 1. USER CONFIGURATION — WIFI PROVISIONING, SINRIC PRO & HARDWARE SETTINGS
// ============================================================================
// SoftAP Hotspot credentials shown when no WiFi is saved or WiFi fails/resets
const char* AP_SSID = "PIYUSH_HOME_SETUP";
const char* AP_PASS = "12345678"; // Minimum 8 characters for WPA2 SoftAP

// --- SINRIC PRO CREDENTIALS (Get these from https://portal.sinric.pro) ---
// Leave as "YOUR_..." to run in Local-Only mode without Sinric Pro Cloud.
#define SINRIC_APP_KEY           "YOUR_APP_KEY_HERE"
#define SINRIC_APP_SECRET        "YOUR_APP_SECRET_HERE"

// 5 Switch Device IDs (Create 5 "Switch" devices in Sinric Pro Portal)
const char* SINRIC_SWITCH_IDS[5] = {
    "YOUR_SWITCH_1_DEVICE_ID",   // Switch 1: Living Light (GPIO 26)
    "YOUR_SWITCH_2_DEVICE_ID",   // Switch 2: Ceiling Fan  (GPIO 27)
    "YOUR_SWITCH_3_DEVICE_ID",   // Switch 3: Bedroom AC   (GPIO 14)
    "YOUR_SWITCH_4_DEVICE_ID",   // Switch 4: Porch Light  (GPIO 12)
    "YOUR_SWITCH_5_DEVICE_ID"    // Switch 5: Water Pump   (GPIO 13)
};

// Optional Sensor Device IDs (Leave as "YOUR_..." if you don't create them)
#define SINRIC_TEMP_SENSOR_ID    "YOUR_TEMP_SENSOR_DEVICE_ID"
#define SINRIC_MOTION_SENSOR_ID  "YOUR_MOTION_SENSOR_DEVICE_ID"

// Hardware WiFi Reset Button (GPIO 0 is the built-in BOOT button on ESP32)
#define WIFI_RESET_BTN_PIN      0
#define WIFI_RESET_HOLD_MS      3000  // Hold button for 3 seconds to erase WiFi
#define WIFI_CONNECT_TIMEOUT_MS 15000 // Switch to Setup AP if WiFi fails in 15s

// Set to true for standard Active-LOW relay modules (LOW = ON, HIGH = OFF)
// Set to false for Active-HIGH relay modules or MOSFETs (HIGH = ON, LOW = OFF)
const bool RELAY_ACTIVE_LOW = true;

// Sensor & Hardware Pin Definitions
#define DHT_PIN       4
#define DHT_TYPE      DHT22   // Change to DHT11 if using a DHT11 sensor
#define MQ2_PIN       34
#define MQ135_PIN     35
#define PIR_PIN       19

// I2C 20x4 LCD Settings (Try 0x3F if 0x27 does not show text)
#define LCD_I2C_ADDR  0x27
#define LCD_COLS      20
#define LCD_ROWS      4

// 5-Channel Relay GPIO Pins (Index 0 = Switch 1 ... Index 4 = Switch 5)
const uint8_t RELAY_PINS[5] = { 26, 27, 14, 12, 13 };
const char*   SWITCH_NAMES[5] = {
    "Living Light",
    "Ceiling Fan",
    "Bedroom AC",
    "Porch Light",
    "Water Pump"
};

// Optional: Uncomment below to assign a Static IP (e.g., 192.168.1.100)
// #define USE_STATIC_IP
#ifdef USE_STATIC_IP
IPAddress local_IP(192, 168, 1, 100);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
#endif

// ============================================================================
// 2. HARDWARE OBJECTS & GLOBAL STATE
// ============================================================================
WebServer server(80);
DNSServer dnsServer;
Preferences preferences;
DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);

const byte DNS_PORT = 53;

// Saved WiFi Credentials (loaded from ESP32 NVS Flash)
String savedSsid = "";
String savedPass = "";
bool   isProvisioningMode = false;

// Hardware Reset Button State (Non-blocking hold detection)
unsigned long resetBtnPressStart = 0;
bool          resetBtnHolding    = false;

// Live Sensor Readings
float temperatureC   = 25.0;
float humidityPct    = 50.0;
int   mq2Ppm         = 120;
int   mq135Ppm       = 400;
bool  motionDetected = false;

// 5-Channel Switch States (Default: Switches 1 & 2 ON, others OFF)
bool switchStates[5] = { false, false, false, false, false };

// 20x4 LCD Display State
bool   lcdCustomMode     = false;
String lcdCustomLines[4] = {
    "==== PIYUSH HOME ===",
    "  SMART IOT READY   ",
    "  WAITING FOR DATA  ",
    "                    "
};
uint8_t lcdLivePageIndex = 0;

// Non-blocking Timing Trackers
unsigned long lastSensorReadMs = 0;
unsigned long lastLcdUpdateMs  = 0;
unsigned long lastWifiCheckMs  = 0;

// Sinric Pro Cloud State & Rate-Limit Trackers
bool          sinricEnabled         = false;
bool          sinricConnected       = false;
unsigned long lastSinricTempMs      = 0;
unsigned long lastSinricMotionMs    = 0;
bool          lastSinricMotionState = false;

const unsigned long SENSOR_INTERVAL_MS        = 2000;  // Read sensors every 2 seconds
const unsigned long LCD_CYCLE_MS              = 4000;  // Cycle live LCD pages every 4 seconds
const unsigned long WIFI_CHECK_MS             = 10000;
const unsigned long SINRIC_TEMP_INTERVAL_MS   = 60000; // Push Temp/Hum to Sinric Pro every 60s
const unsigned long SINRIC_MOTION_COOLDOWN_MS = 5000;  // Min 5s gap between Sinric motion events

// ============================================================================
// 3. HARDWARE HELPER FUNCTIONS
// ============================================================================

/**
 * Safely apply relay state (handles both Active-LOW and Active-HIGH relay boards).
 */
void applyRelayState(uint8_t index, bool turnOn) {
    if (index >= 5) return;
    switchStates[index] = turnOn;
    if (RELAY_ACTIVE_LOW) {
        digitalWrite(RELAY_PINS[index], turnOn ? LOW : HIGH);
    } else {
        digitalWrite(RELAY_PINS[index], turnOn ? HIGH : LOW);
    }
}

/**
 * Read an analog pin with 10-sample averaging to minimize ESP32 ADC jitter.
 */
int readSmoothedADC(uint8_t pin) {
    long total = 0;
    const int samples = 10;
    for (int i = 0; i < samples; i++) {
        total += analogRead(pin);
        delayMicroseconds(500);
    }
    return (int)(total / samples);
}

/**
 * Strictly pad or trim a String to exactly 20 characters for flicker-free LCD updates.
 */
String pad20(String text) {
    if (text.length() > 20) {
        return text.substring(0, 20);
    }
    while (text.length() < 20) {
        text += ' ';
    }
    return text;
}

/**
 * Write 4 lines of 20 characters to the physical 20x4 I2C LCD.
 */
void writeLcdLines(const String& l0, const String& l1, const String& l2, const String& l3) {
    lcd.setCursor(0, 0); lcd.print(pad20(l0));
    lcd.setCursor(0, 1); lcd.print(pad20(l1));
    lcd.setCursor(0, 2); lcd.print(pad20(l2));
    lcd.setCursor(0, 3); lcd.print(pad20(l3));
}

/**
 * Attach CORS headers so piyush_home.html can communicate from any browser/origin.
 */
void sendCorsHeaders() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

/**
 * Handle HTTP OPTIONS preflight requests for POST /api/lcd.
 */
void handleOptions() {
    sendCorsHeaders();
    server.send(204);
}

// ============================================================================
// 4. SENSOR ACQUISITION & LCD RENDERING
// ============================================================================

/**
 * Poll DHT22, MQ-2, MQ-135, and PIR motion sensors.
 */
void readAllSensors() {
    // --- DHT22 Temperature & Humidity ---
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    // Only update if valid (ignore occasional DHT NaN read errors)
    if (!isnan(t) && !isnan(h)) {
        temperatureC = t;
        humidityPct  = h;
    } else {
        Serial.println("[WARN] Failed to read from DHT22 sensor!");
    }

    // --- MQ-2 Combustible Gas & Smoke (Mapped 12-bit ADC -> Approx PPM) ---
    int rawMq2 = readSmoothedADC(MQ2_PIN);
    // Adjust map range after calibrating your MQ-2 in clean air
    mq2Ppm = map(rawMq2, 0, 4095, 80, 1000);

    // --- MQ-135 Air Quality CO2/VOC (Mapped 12-bit ADC -> Approx PPM) ---
    int rawMq135 = readSmoothedADC(MQ135_PIN);
    // Adjust map range after calibrating your MQ-135 in fresh air
    mq135Ppm = map(rawMq135, 0, 4095, 320, 1800);

    // --- PIR Motion Sensor ---
    motionDetected = (digitalRead(PIR_PIN) == HIGH);
}

/**
 * Refresh the 20x4 I2C LCD with either Live Telemetry or Custom Text.
 */
void updateLcdDisplay() {
    if (lcdCustomMode) {
        writeLcdLines(
            lcdCustomLines[0],
            lcdCustomLines[1],
            lcdCustomLines[2],
            lcdCustomLines[3]
        );
        return;
    }

    // Count active switches
    int activeCount = 0;
    for (int i = 0; i < 5; i++) {
        if (switchStates[i]) activeCount++;
    }

    // Cycle between 3 informative live telemetry pages
    uint8_t page = lcdLivePageIndex % 3;

    if (page == 0) {
        // Page 0: Overview (Matches piyush_home.html Overview screen)
        String l0 = "==== PIYUSH HOME ===";
        String l1 = "T:" + String(temperatureC, 1) + "C   H:" + String((int)humidityPct) + "%";
        String l2 = "MQ2:" + String(mq2Ppm) + "  MQ135:" + String(mq135Ppm);
        String l3 = "MOT:" + String(motionDetected ? "DETECT!" : "CLEAR  ") + " SW:" + String(activeCount) + "/5";
        writeLcdLines(l0, l1, l2, l3);
    } else if (page == 1) {
        // Page 1: Detailed Gas & Air Safety
        String mq2Stat = (mq2Ppm >= 400) ? "DANGER" : (mq2Ppm >= 280 ? "WARN" : "SAFE");
        String airStat = (mq135Ppm >= 1000) ? "POOR" : (mq135Ppm >= 650 ? "MOD" : "FRESH");
        String l0 = "== AIR & GAS DATA ==";
        String l1 = "MQ-2  :" + String(mq2Ppm) + "ppm " + mq2Stat;
        String l2 = "MQ-135:" + String(mq135Ppm) + "ppm " + airStat;
        String l3 = "IP:" + WiFi.localIP().toString();
        writeLcdLines(l0, l1, l2, l3);
    } else {
        // Page 2: 5-Switch Relay Status
        String l0 = "== 5 SWITCH STATE ==";
        String l1 = "S1:" + String(switchStates[0] ? "ON " : "OFF") +
                    " S2:" + String(switchStates[1] ? "ON " : "OFF") +
                    " S3:" + String(switchStates[2] ? "ON " : "OFF");
        String l2 = "S4:" + String(switchStates[3] ? "ON " : "OFF") +
                    " S5:" + String(switchStates[4] ? "ON " : "OFF");
        String l3 = "ACTIVE LOADS: " + String(activeCount) + "/5 ON";
        writeLcdLines(l0, l1, l2, l3);
    }
}

// ============================================================================
// 5. REST API HANDLERS (PAIRED WITH piyush_home.html)
// ============================================================================

/**
 * GET /
 * Basic health check page so you can verify the ESP32 is online in a browser.
 */
void handleRoot() {
    sendCorsHeaders();
    String sinricStatus = !sinricEnabled ? "Not Configured" : (sinricConnected ? "Connected" : "Connecting...");
    String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>PIYUSH HOME ESP32</title></head>"
                  "<body style='font-family:sans-serif;background:#0f172a;color:#e2e8f0;padding:24px;max-width:600px;margin:auto;'>"
                  "<h1 style='color:#38bdf8'>PIYUSH HOME — ESP32 Controller Online</h1>"
                  "<p>Connected WiFi: <b>" + savedSsid + "</b></p>"
                  "<p>IP Address: <b>" + WiFi.localIP().toString() + "</b></p>"
                  "<p>Sinric Pro Cloud: <b>" + sinricStatus + "</b></p>"
                  "<ul>"
                  "<li><a style='color:#38bdf8' href='/api/sensors'>GET /api/sensors</a></li>"
                  "<li>GET /api/switch?id=1&amp;state=1</li>"
                  "<li>POST /api/lcd (JSON: {\"lines\":[\"L1\",\"L2\",\"L3\",\"L4\"]})</li>"
                  "<li><a style='color:#38bdf8' href='/api/lcd/live'>GET /api/lcd/live</a> (Return LCD to Live Sensor Mode)</li>"
                  "</ul>"
                  "<hr style='border-color:#334155;margin:20px 0;'>"
                  "<form action='/api/wifi/reset' method='POST'>"
                  "<button type='submit' style='background:#e11d48;color:#fff;border:none;padding:10px 18px;border-radius:8px;font-weight:bold;cursor:pointer;'>"
                  "Reset Saved WiFi Credentials</button></form>"
                  "<p style='font-size:12px;color:#94a3b8;margin-top:8px;'>Or hold the ESP32 BOOT button (GPIO 0) for 3 seconds.</p>"
                  "</body></html>";
    server.send(200, "text/html", html);
}

/**
 * GET /api/sensors
 * Returns all telemetry readings & switch states as JSON.
 */
void handleGetSensors() {
    sendCorsHeaders();

    // Also do an instant PIR check so motion triggers feel snappy
    motionDetected = (digitalRead(PIR_PIN) == HIGH);

    String json = "{";
    json += "\"temp\":"     + String(temperatureC, 1) + ",";
    json += "\"humidity\":" + String(humidityPct, 1)  + ",";
    json += "\"mq2\":"      + String(mq2Ppm)          + ",";
    json += "\"mq135\":"    + String(mq135Ppm)        + ",";
    json += "\"motion\":"   + String(motionDetected ? "true" : "false") + ",";
    json += "\"switches\":[";
    for (int i = 0; i < 5; i++) {
        json += switchStates[i] ? "true" : "false";
        if (i < 4) json += ",";
    }
    json += "]}";

    server.send(200, "application/json", json);
}

// Forward declaration for Sinric Pro switch synchronization
void syncSwitchToSinric(uint8_t index, bool turnOn);

/**
 * GET /api/switch?id=1&state=1
 * Controls relay switches 1 through 5.
 */
void handleSwitchControl() {
    sendCorsHeaders();

    if (!server.hasArg("id") || !server.hasArg("state")) {
        server.send(400, "application/json", "{\"error\":\"Missing id or state parameter\"}");
        return;
    }

    int id    = server.arg("id").toInt();
    int state = server.arg("state").toInt();

    if (id < 1 || id > 5) {
        server.send(400, "application/json", "{\"error\":\"Switch id must be 1 to 5\"}");
        return;
    }

    uint8_t idx = (uint8_t)(id - 1);
    bool turnOn = (state == 1);
    applyRelayState(idx, turnOn);

    // Sync local web dashboard switch change back to Sinric Pro / Alexa / Google Home
    syncSwitchToSinric(idx, turnOn);

    Serial.printf("[SWITCH] SW-%d (%s) -> %s\n", id, SWITCH_NAMES[idx], turnOn ? "ON" : "OFF");

    // Refresh LCD immediately if displaying live switch status
    if (!lcdCustomMode) {
        updateLcdDisplay();
    }

    String resp = "{\"ok\":true,\"id\":" + String(id) + ",\"state\":" + String(turnOn ? "true" : "false") + "}";
    server.send(200, "application/json", resp);
}

/**
 * Lightweight zero-dependency parser to extract up to 4 quoted strings
 * from JSON payload: {"lines":["Row 1","Row 2","Row 3","Row 4"]}
 */
bool parseLcdLinesJson(const String& body, String outLines[4]) {
    int arrStart = body.indexOf('[');
    int arrEnd   = body.lastIndexOf(']');
    if (arrStart == -1 || arrEnd == -1 || arrEnd <= arrStart) {
        return false;
    }

    int lineIdx = 0;
    bool inQuotes = false;
    bool escaped  = false;
    String current = "";

    for (int i = arrStart + 1; i < arrEnd && lineIdx < 4; i++) {
        char c = body.charAt(i);
        if (inQuotes) {
            if (escaped) {
                current += c;
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                outLines[lineIdx++] = pad20(current);
                current = "";
                inQuotes = false;
            } else {
                current += c;
            }
        } else {
            if (c == '"') {
                inQuotes = true;
            }
        }
    }

    // Fill any omitted lines with spaces
    while (lineIdx < 4) {
        outLines[lineIdx++] = pad20("");
    }
    return true;
}

/**
 * POST /api/lcd
 * Displays custom 20x4 text sent from the dashboard onto the physical LCD.
 */
void handlePostLcd() {
    sendCorsHeaders();

    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Empty request body\"}");
        return;
    }

    String body = server.arg("plain");
    String parsed[4];

    if (!parseLcdLinesJson(body, parsed)) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON format. Expected {\\\"lines\\\":[...]}\"}");
        return;
    }

    // Check if all 4 lines are blank -> return to Live Sensor Data Mode
    bool allEmpty = true;
    for (int i = 0; i < 4; i++) {
        String trimmed = parsed[i];
        trimmed.trim();
        if (trimmed.length() > 0) allEmpty = false;
        lcdCustomLines[i] = parsed[i];
    }

    lcdCustomMode = !allEmpty;
    updateLcdDisplay();

    Serial.println(lcdCustomMode ? "[LCD] Custom 20x4 message displayed." : "[LCD] Cleared -> Returned to Live Mode.");
    server.send(200, "application/json", "{\"ok\":true,\"mode\":\"" + String(lcdCustomMode ? "custom" : "live") + "\"}");
}

/**
 * GET /api/lcd/live
 * Explicitly switches the physical 20x4 LCD back to Live Telemetry Mode.
 */
void handleLcdLiveMode() {
    sendCorsHeaders();
    lcdCustomMode = false;
    updateLcdDisplay();
    server.send(200, "application/json", "{\"ok\":true,\"mode\":\"live\"}");
}

// Forward declaration for API reset handler
void resetWifiCredentials();

/**
 * GET / POST /api/wifi/reset
 * Remotely clears saved WiFi credentials and reboots into Provisioning AP Mode.
 */
void handleWifiResetApi() {
    sendCorsHeaders();
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"WiFi credentials erased. Rebooting into PIYUSH_HOME_SETUP AP...\"}");
    delay(600);
    resetWifiCredentials();
}

// ============================================================================
// 6. WIFI PROVISIONING, NVS SAVE, CAPTIVE PORTAL & RESET SYSTEM
// ============================================================================

/**
 * Load saved WiFi SSID & Password from ESP32 non-volatile flash memory (NVS).
 */
void loadWifiCredentials() {
    preferences.begin("piyush_wifi", true); // Read-only mode
    savedSsid = preferences.getString("ssid", "");
    savedPass = preferences.getString("pass", "");
    preferences.end();

    if (savedSsid.length() > 0) {
        Serial.println("[NVS] Loaded saved WiFi SSID: " + savedSsid);
    } else {
        Serial.println("[NVS] No saved WiFi credentials found.");
    }
}

/**
 * Save WiFi SSID & Password persistently to ESP32 NVS Flash.
 */
void saveWifiCredentials(const String& ssid, const String& pass) {
    preferences.begin("piyush_wifi", false); // Read-Write mode
    preferences.putString("ssid", ssid);
    preferences.putString("pass", pass);
    preferences.end();

    savedSsid = ssid;
    savedPass = pass;
    Serial.println("[NVS] WiFi credentials saved for SSID: " + ssid);
}

/**
 * Erase saved WiFi credentials from NVS Flash and reboot into Setup AP Mode.
 */
void resetWifiCredentials() {
    Serial.println("[WIFI] Erasing saved WiFi credentials...");
    preferences.begin("piyush_wifi", false);
    preferences.clear();
    preferences.end();

    writeLcdLines(
        "==== PIYUSH HOME ===",
        "  WIFI CREDENTIALS  ",
        "      ERASED!       ",
        "   Rebooting AP...  "
    );
    delay(1800);
    ESP.restart();
}

/**
 * GET /scan (Used by Captive Portal to list nearby WiFi networks as JSON)
 */
void handleWifiScan() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        String ssid = WiFi.SSID(i);
        ssid.replace("\\", "\\\\");
        ssid.replace("\"", "\\\"");
        json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
                ",\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    json += "]";
    WiFi.scanDelete();
    server.send(200, "application/json", json);
}

/**
 * GET / (Provisioning Captive Portal Web Page)
 */
void handleWifiSetupPage() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PIYUSH HOME — WiFi Setup</title>
<style>
  body{font-family:system-ui,-apple-system,sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:20px;display:flex;justify-content:center;align-items:center;min-height:100vh;box-sizing:border-box;}
  .card{background:#1e293b;border:1px solid #334155;border-radius:16px;padding:24px;width:100%;max-width:400px;box-shadow:0 10px 30px rgba(0,0,0,0.4);}
  h1{margin:0 0 6px;font-size:22px;color:#38bdf8;text-align:center;}
  p.sub{margin:0 0 20px;font-size:13px;color:#94a3b8;text-align:center;}
  label{display:block;font-size:12px;font-weight:700;text-transform:uppercase;color:#94a3b8;margin-bottom:6px;}
  input,select{width:100%;padding:11px 12px;border-radius:10px;border:1px solid #475569;background:#0f172a;color:#fff;font-size:14px;box-sizing:border-box;margin-bottom:14px;}
  button{width:100%;padding:12px;border:none;border-radius:10px;font-size:14px;font-weight:700;cursor:pointer;margin-top:4px;}
  .btn-primary{background:#0284c7;color:#fff;}
  .btn-scan{background:#334155;color:#38bdf8;margin-bottom:14px;}
  .btn-danger{background:#9f1239;color:#fff;margin-top:10px;font-size:12px;padding:9px;}
  .row{display:flex;align-items:center;gap:8px;margin-bottom:14px;font-size:13px;color:#cbd5e1;}
  .row input{width:auto;margin:0;}
  #status{font-size:12px;color:#38bdf8;text-align:center;margin-bottom:10px;}
</style>
</head>
<body>
<div class="card">
  <h1>PIYUSH HOME</h1>
  <p class="sub">ESP32 WiFi Provisioning Portal</p>
  <button type="button" class="btn-scan" onclick="scanWifi()">Scan Nearby WiFi Networks</button>
  <div id="status"></div>
  <select id="netList" style="display:none" onchange="document.getElementById('ssid').value=this.value;"></select>
  <form action="/save" method="POST">
    <label for="ssid">WiFi Name (SSID)</label>
    <input id="ssid" name="ssid" type="text" placeholder="Enter your WiFi SSID" required>
    <label for="pass">WiFi Password</label>
    <input id="pass" name="pass" type="password" placeholder="Leave blank if open WiFi">
    <div class="row">
      <input type="checkbox" id="showPw" onclick="document.getElementById('pass').type=this.checked?'text':'password'">
      <label for="showPw" style="margin:0;text-transform:none;font-weight:500;">Show Password</label>
    </div>
    <button type="submit" class="btn-primary">Save &amp; Connect WiFi</button>
  </form>
  <form action="/api/wifi/reset" method="POST">
    <button type="submit" class="btn-danger">Clear Saved WiFi &amp; Reboot</button>
  </form>
</div>
<script>
async function scanWifi(){
  const st=document.getElementById('status');
  const sel=document.getElementById('netList');
  st.textContent='Scanning networks...';
  try{
    const res=await fetch('/scan');
    const nets=await res.json();
    sel.innerHTML='<option value="">-- Select a WiFi Network --</option>';
    nets.forEach(n=>{
      const opt=document.createElement('option');
      opt.value=n.ssid;
      opt.textContent=n.ssid+' ('+n.rssi+' dBm)'+(n.open?' [OPEN]':'');
      sel.appendChild(opt);
    });
    sel.style.display='block';
    st.textContent='Found '+nets.length+' networks.';
  }catch(e){
    st.textContent='Scan failed. Enter SSID manually.';
  }
}
</script>
</body>
</html>
)rawliteral";
    server.send(200, "text/html", html);
}

/**
 * POST /save (Save WiFi Credentials to NVS Flash and Reboot)
 */
void handleWifiSave() {
    String newSsid = server.arg("ssid");
    String newPass = server.arg("pass");
    newSsid.trim();

    if (newSsid.length() == 0) {
        server.send(400, "text/html", "<h3>SSID cannot be empty. <a href='/'>Go Back</a></h3>");
        return;
    }

    saveWifiCredentials(newSsid, newPass);

    String resp = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head>"
                  "<body style='font-family:sans-serif;background:#0f172a;color:#e2e8f0;text-align:center;padding:40px;'>"
                  "<h2 style='color:#34d399;'>WiFi Saved Successfully!</h2>"
                  "<p>SSID: <b>" + newSsid + "</b></p>"
                  "<p>PIYUSH HOME ESP32 is rebooting and connecting to your WiFi.</p>"
                  "<p style='color:#94a3b8;font-size:13px;'>Check the 20x4 LCD screen for the new IP address.</p>"
                  "</body></html>";
    server.send(200, "text/html", resp);

    writeLcdLines(
        "==== PIYUSH HOME ===",
        "   WIFI SAVED OK!   ",
        newSsid.substring(0, 20),
        "   Rebooting now... "
    );
    delay(2000);
    ESP.restart();
}

/**
 * Launch SoftAP + Captive Portal when no WiFi is configured or connection fails.
 */
void startProvisioningAP() {
    isProvisioningMode = true;
    WiFi.disconnect(true);
    delay(200);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);

    IPAddress apIP = WiFi.softAPIP();
    dnsServer.start(DNS_PORT, "*", apIP);

    Serial.println("\n[PROV] ========================================");
    Serial.println("[PROV] WiFi Provisioning Mode ACTIVE!");
    Serial.print  ("[PROV] Connect phone/PC to WiFi: "); Serial.println(AP_SSID);
    Serial.print  ("[PROV] Password: "); Serial.println(AP_PASS);
    Serial.print  ("[PROV] Open browser to: http://"); Serial.println(apIP);
    Serial.println("[PROV] ========================================");

    writeLcdLines(
        "== WIFI SETUP AP ===",
        "AP:" + String(AP_SSID).substring(0, 17),
        "PW:" + String(AP_PASS).substring(0, 17),
        "IP:" + apIP.toString()
    );

    // Captive Portal & Provisioning Routes
    server.on("/",               HTTP_GET,  handleWifiSetupPage);
    server.on("/scan",           HTTP_GET,  handleWifiScan);
    server.on("/save",           HTTP_POST, handleWifiSave);
    server.on("/api/wifi/reset", HTTP_POST, handleWifiResetApi);
    server.on("/api/wifi/reset", HTTP_GET,  handleWifiResetApi);

    // Redirect all unknown OS captive-portal URLs (Android/iOS/Windows) to setup page
    server.onNotFound([]() {
        server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
        server.send(302, "text/plain", "");
    });

    server.begin();
}

/**
 * Attempt to connect using saved NVS WiFi credentials.
 * Returns true on success, false if missing credentials or connection timed out.
 */
bool connectWiFi() {
    if (savedSsid.length() == 0) {
        return false;
    }

    WiFi.mode(WIFI_STA);

    #ifdef USE_STATIC_IP
    if (!WiFi.config(local_IP, gateway, subnet)) {
        Serial.println("[WARN] Static IP configuration failed!");
    }
    #endif

    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    Serial.print("[WIFI] Connecting to ");
    Serial.println(savedSsid);

    writeLcdLines(
        "==== PIYUSH HOME ===",
        "Connecting WiFi...  ",
        savedSsid.substring(0, 20),
        "Hold BOOT 3s=Reset  "
    );

    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_CONNECT_TIMEOUT_MS) {
        // Allow user to hold BOOT button even while attempting WiFi connection
        if (digitalRead(WIFI_RESET_BTN_PIN) == LOW) {
            delay(50);
            unsigned long holdStart = millis();
            while (digitalRead(WIFI_RESET_BTN_PIN) == LOW) {
                if (millis() - holdStart >= WIFI_RESET_HOLD_MS) {
                    resetWifiCredentials();
                }
                delay(50);
            }
        }
        delay(250);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WIFI] Connected!");
        Serial.print("[WIFI] ESP32 IP Address: ");
        Serial.println(WiFi.localIP());

        writeLcdLines(
            "==== PIYUSH HOME ===",
            "WiFi Connected!     ",
            "IP:" + WiFi.localIP().toString(),
            "System Online Ready "
        );
        delay(2500); // Briefly show IP on the 20x4 LCD
        return true;
    } else {
        Serial.println("\n[WARN] WiFi connection timed out! Launching Provisioning AP...");
        writeLcdLines(
            "==== PIYUSH HOME ===",
            "WiFi Connect Failed!",
            "Starting Setup AP...",
            "                    "
        );
        delay(1500);
        return false;
    }
}

/**
 * Non-blocking monitor for the Hardware WiFi Reset Button (BOOT / GPIO 0).
 * Hold for 3 seconds at any time to clear saved WiFi credentials and reboot into AP.
 */
void checkResetButton() {
    bool isPressed = (digitalRead(WIFI_RESET_BTN_PIN) == LOW);

    if (isPressed) {
        if (!resetBtnHolding) {
            resetBtnHolding    = true;
            resetBtnPressStart = millis();
        } else {
            unsigned long heldMs = millis() - resetBtnPressStart;
            if (heldMs >= WIFI_RESET_HOLD_MS) {
                resetWifiCredentials();
            } else if (heldMs >= 500) {
                // Show live countdown on 20x4 LCD while button is held
                int secsLeft = (int)((WIFI_RESET_HOLD_MS - heldMs + 999) / 1000);
                writeLcdLines(
                    "==== PIYUSH HOME ===",
                    " HOLD BOOT BUTTON!  ",
                    "Resetting WiFi in " + String(secsLeft) + "s",
                    "Release to cancel   "
                );
            }
        }
    } else if (resetBtnHolding) {
        // Button released before 3 seconds — cancel reset & restore LCD
        resetBtnHolding = false;
        if (isProvisioningMode) {
            writeLcdLines(
                "== WIFI SETUP AP ===",
                "AP:" + String(AP_SSID).substring(0, 17),
                "PW:" + String(AP_PASS).substring(0, 17),
                "IP:" + WiFi.softAPIP().toString()
            );
        } else {
            updateLcdDisplay();
        }
    }
}

// ============================================================================
// 6B. SINRIC PRO CLOUD INTEGRATION (ALEXA / GOOGLE HOME / MOBILE APP)
// ============================================================================

/**
 * Check whether a Sinric Pro credential or Device ID has been configured.
 */
bool isValidSinricId(const char* value) {
    if (value == nullptr || strlen(value) < 8) return false;
    return (strncmp(value, "YOUR_", 5) != 0);
}

/**
 * Callback triggered when Alexa, Google Home, or Sinric Pro App toggles a switch.
 */
bool onPowerState(const String& deviceId, bool& state) {
    for (uint8_t i = 0; i < 5; i++) {
        if (deviceId == SINRIC_SWITCH_IDS[i]) {
            applyRelayState(i, state);
            Serial.printf("[SINRIC] SW-%d (%s) -> %s\n", i + 1, SWITCH_NAMES[i], state ? "ON" : "OFF");

            if (!lcdCustomMode && !resetBtnHolding) {
                updateLcdDisplay();
            }
            return true; // Request handled successfully
        }
    }
    return false;
}

/**
 * Push a locally toggled relay state (from piyush_home.html) to Sinric Pro Cloud.
 */
void syncSwitchToSinric(uint8_t index, bool turnOn) {
    if (!sinricEnabled || !sinricConnected || index >= 5) return;
    if (!isValidSinricId(SINRIC_SWITCH_IDS[index])) return;

    SinricProSwitch& mySwitch = SinricPro[SINRIC_SWITCH_IDS[index]];
    mySwitch.sendPowerStateEvent(turnOn);
}

/**
 * Periodically report DHT22 Temperature/Humidity and PIR Motion events to Sinric Pro.
 */
void syncSensorsToSinric() {
    if (!sinricEnabled || !sinricConnected) return;
    unsigned long now = millis();

    // 1. Report Temperature & Humidity every 60 seconds (respects Sinric Pro rate limits)
    if (isValidSinricId(SINRIC_TEMP_SENSOR_ID) &&
        (lastSinricTempMs == 0 || now - lastSinricTempMs >= SINRIC_TEMP_INTERVAL_MS)) {
        lastSinricTempMs = now;
        SinricProTemperaturesensor& myTempSensor = SinricPro[SINRIC_TEMP_SENSOR_ID];
        myTempSensor.sendTemperatureEvent(temperatureC, humidityPct);
        Serial.printf("[SINRIC] Sent Temp: %.1fC, Hum: %.1f%%\n", temperatureC, humidityPct);
    }

    // 2. Report PIR Motion Sensor changes with a 5-second cooldown
    if (isValidSinricId(SINRIC_MOTION_SENSOR_ID) &&
        motionDetected != lastSinricMotionState &&
        (now - lastSinricMotionMs >= SINRIC_MOTION_COOLDOWN_MS)) {
        lastSinricMotionMs    = now;
        lastSinricMotionState = motionDetected;
        SinricProMotionsensor& myMotionSensor = SinricPro[SINRIC_MOTION_SENSOR_ID];
        myMotionSensor.sendMotionEvent(motionDetected);
        Serial.printf("[SINRIC] Sent Motion: %s\n", motionDetected ? "DETECTED" : "CLEAR");
    }
}

/**
 * Initialize Sinric Pro Cloud devices and callbacks after WiFi is connected.
 */
void setupSinricPro() {
    if (!isValidSinricId(SINRIC_APP_KEY) || !isValidSinricId(SINRIC_APP_SECRET)) {
        Serial.println("[SINRIC] App Key/Secret not set — running in Local Dashboard mode only.");
        sinricEnabled = false;
        return;
    }

    // Register all configured Relay Switches (1 to 5)
    for (uint8_t i = 0; i < 5; i++) {
        if (isValidSinricId(SINRIC_SWITCH_IDS[i])) {
            SinricProSwitch& mySwitch = SinricPro[SINRIC_SWITCH_IDS[i]];
            mySwitch.onPowerState(onPowerState);
            Serial.printf("[SINRIC] Registered Switch %d (%s)\n", i + 1, SWITCH_NAMES[i]);
        }
    }

    // Register optional DHT22 Temperature & Humidity Sensor
    if (isValidSinricId(SINRIC_TEMP_SENSOR_ID)) {
        SinricProTemperaturesensor& myTempSensor = SinricPro[SINRIC_TEMP_SENSOR_ID];
        (void)myTempSensor;
        Serial.println("[SINRIC] Registered DHT22 Temperature/Humidity Sensor.");
    }

    // Register optional PIR Motion Sensor
    if (isValidSinricId(SINRIC_MOTION_SENSOR_ID)) {
        SinricProMotionsensor& myMotionSensor = SinricPro[SINRIC_MOTION_SENSOR_ID];
        (void)myMotionSensor;
        Serial.println("[SINRIC] Registered PIR Motion Sensor.");
    }

    SinricPro.onConnected([]() {
        sinricConnected = true;
        Serial.println("[SINRIC] Connected to Sinric Pro Cloud!");
    });

    SinricPro.onDisconnected([]() {
        sinricConnected = false;
        Serial.println("[SINRIC] Disconnected from Sinric Pro Cloud.");
    });

    SinricPro.begin(SINRIC_APP_KEY, SINRIC_APP_SECRET);
    sinricEnabled = true;
    Serial.println("[SINRIC] Sinric Pro initialized.");
}

// ============================================================================
// 7. ARDUINO SETUP & MAIN LOOP
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== PIYUSH HOME ESP32 BOOTING ===");

    // Initialize Hardware WiFi Reset Button (Built-in BOOT button, Active LOW)
    pinMode(WIFI_RESET_BTN_PIN, INPUT_PULLUP);

    // Initialize 5 Relay Pins (Start all safely OFF)
    for (uint8_t i = 0; i < 5; i++) {
        pinMode(RELAY_PINS[i], OUTPUT);
        applyRelayState(i, false);
    }

    // Initialize PIR Motion & Analog Sensors
    pinMode(PIR_PIN, INPUT);
    analogReadResolution(12); // ESP32 12-bit ADC (0 - 4095)
    dht.begin();

    // Initialize 20x4 I2C LCD
    Wire.begin(21, 22);
    lcd.init();
    lcd.backlight();
    writeLcdLines(
        "==== PIYUSH HOME ===",
        "  SMART IOT SYSTEM  ",
        " Initializing...    ",
        " DHT22 MQ2 MQ135 PIR"
    );
    delay(1200);

    // Load Saved WiFi Credentials from NVS Flash & Connect
    loadWifiCredentials();
    if (!connectWiFi()) {
        // No saved WiFi or failed to connect -> Launch Captive Portal Setup AP
        startProvisioningAP();
        return;
    }

    // Register Normal Smart Home REST API Routes
    server.on("/",               HTTP_GET,     handleRoot);
    server.on("/api/sensors",    HTTP_GET,     handleGetSensors);
    server.on("/api/sensors",    HTTP_OPTIONS, handleOptions);
    server.on("/api/switch",     HTTP_GET,     handleSwitchControl);
    server.on("/api/switch",     HTTP_OPTIONS, handleOptions);
    server.on("/api/lcd",        HTTP_POST,    handlePostLcd);
    server.on("/api/lcd",        HTTP_OPTIONS, handleOptions);
    server.on("/api/lcd/live",   HTTP_GET,     handleLcdLiveMode);
    server.on("/api/wifi/reset", HTTP_POST,    handleWifiResetApi);
    server.on("/api/wifi/reset", HTTP_GET,     handleWifiResetApi);
    server.on("/api/wifi/reset", HTTP_OPTIONS, handleOptions);

    server.begin();
    Serial.println("[HTTP] REST API Server started on port 80.");

    // Initialize Sinric Pro Cloud integration (if credentials are configured)
    setupSinricPro();

    // Initial sensor read & LCD render
    readAllSensors();
    updateLcdDisplay();
}

void loop() {
    // Always monitor the Hardware WiFi Reset Button (BOOT / GPIO 0)
    checkResetButton();

    // Process incoming web requests (either Captive Portal or Smart Home API)
    server.handleClient();

    // In Provisioning Mode, answer DNS requests so the setup page pops up automatically
    if (isProvisioningMode) {
        dnsServer.processNextRequest();
        return;
    }

    // Process Sinric Pro Cloud events (Alexa / Google Home / Mobile App)
    if (sinricEnabled) {
        SinricPro.handle();
    }

    unsigned long now = millis();

    // 1. Periodic Sensor Reading (Every 2 seconds)
    if (now - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
        lastSensorReadMs = now;
        readAllSensors();
        syncSensorsToSinric();

        // Update live LCD immediately when sensor data refreshes (unless holding reset btn)
        if (!lcdCustomMode && !resetBtnHolding) {
            updateLcdDisplay();
        }
    }

    // 2. Cycle Live 20x4 LCD Pages (Every 4 seconds)
    if (!lcdCustomMode && !resetBtnHolding && (now - lastLcdUpdateMs >= LCD_CYCLE_MS)) {
        lastLcdUpdateMs = now;
        lcdLivePageIndex++;
        updateLcdDisplay();
    }

    // 3. Auto-reconnect WiFi if dropped
    if (now - lastWifiCheckMs >= WIFI_CHECK_MS) {
        lastWifiCheckMs = now;
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WIFI] Connection lost. Reconnecting...");
            WiFi.reconnect();
        }
    }
}