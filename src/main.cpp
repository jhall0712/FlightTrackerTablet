#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <math.h>
#include "i2c.h"
#include "io_extension.h"
#include "rgb_lcd_port.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID_DEFAULT ""
#define WIFI_PASSWORD_DEFAULT ""
#define HOME_LAT_DEFAULT 0.0
#define HOME_LON_DEFAULT 0.0
#define TRACK_RADIUS_NM_DEFAULT 15
#endif

#ifndef WIFI_SSID_DEFAULT
#define WIFI_SSID_DEFAULT WIFI_SSID
#endif

#ifndef WIFI_PASSWORD_DEFAULT
#define WIFI_PASSWORD_DEFAULT WIFI_PASSWORD
#endif

#ifndef HOME_LAT_DEFAULT
#define HOME_LAT_DEFAULT HOME_LAT
#endif

#ifndef HOME_LON_DEFAULT
#define HOME_LON_DEFAULT HOME_LON
#endif

#ifndef TRACK_RADIUS_NM_DEFAULT
#define TRACK_RADIUS_NM_DEFAULT TRACK_RADIUS_NM
#endif

namespace {
constexpr int kScreenW = EXAMPLE_LCD_H_RES;
constexpr int kScreenH = EXAMPLE_LCD_V_RES;
constexpr int kMapW = 724;
constexpr int kSidebarX = kMapW;
constexpr uint32_t kFetchEveryMs = 15000;
constexpr size_t kMaxAircraft = 36;
constexpr uint8_t kTouchResetExio = 1;
constexpr uint8_t kLcdBacklightExio = 2;
constexpr uint8_t kLcdResetExio = 3;
constexpr uint8_t kLcdVddEnableExio = 6;
constexpr const char *kSetupApSsid = "FlightTrackerTablet";
constexpr const char *kSetupApPassword = "flighttablet";

struct Aircraft {
    char flight[12] = "";
    char reg[12] = "";
    char type[10] = "";
    double lat = 0;
    double lon = 0;
    int altitudeFt = 0;
    float speedKt = 0;
    float trackDeg = 0;
    float distNm = 0;
    float bearingDeg = 0;
    bool valid = false;
};

struct AppConfig {
    String ssid;
    String password;
    double homeLat = HOME_LAT_DEFAULT;
    double homeLon = HOME_LON_DEFAULT;
    int radiusNm = TRACK_RADIUS_NM_DEFAULT;
};

class PsramCanvas16 : public Adafruit_GFX {
public:
    PsramCanvas16(int16_t w, int16_t h, uint16_t *buffer) : Adafruit_GFX(w, h), buffer_(buffer) {}

    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        if (!buffer_ || x < 0 || y < 0 || x >= width() || y >= height()) return;
        buffer_[y * WIDTH + x] = color;
    }

    uint16_t *getBuffer() {
        return buffer_;
    }

private:
    uint16_t *buffer_ = nullptr;
};

PsramCanvas16 *canvas = nullptr;
uint16_t *frameBuffer = nullptr;
esp_lcd_panel_handle_t lcd = nullptr;
Preferences preferences;
WebServer server(80);
AppConfig config;
Aircraft aircraft[kMaxAircraft];
size_t aircraftCount = 0;
uint32_t lastFetchMs = 0;
String statusLine = "Booting";
String lastUpdated = "never";
bool setupPortalRunning = false;

uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

String trimCopy(const char *value) {
    if (!value) return "";
    String out(value);
    out.trim();
    return out;
}

bool configured() {
    return config.ssid.length() > 0 && fabs(config.homeLat) > 0.001 && fabs(config.homeLon) > 0.001;
}

String htmlEscaped(const String &value) {
    String out;
    out.reserve(value.length() + 8);
    for (char c : value) {
        if (c == '&') out += F("&amp;");
        else if (c == '<') out += F("&lt;");
        else if (c == '>') out += F("&gt;");
        else if (c == '"') out += F("&quot;");
        else out += c;
    }
    return out;
}

void loadConfig() {
    preferences.begin("flight", false);
    config.ssid = preferences.getString("ssid", WIFI_SSID_DEFAULT);
    config.password = preferences.getString("pass", WIFI_PASSWORD_DEFAULT);
    config.homeLat = preferences.getDouble("lat", HOME_LAT_DEFAULT);
    config.homeLon = preferences.getDouble("lon", HOME_LON_DEFAULT);
    config.radiusNm = preferences.getInt("radius", TRACK_RADIUS_NM_DEFAULT);
    config.radiusNm = constrain(config.radiusNm, 1, 250);
}

void saveConfig() {
    preferences.putString("ssid", config.ssid);
    preferences.putString("pass", config.password);
    preferences.putDouble("lat", config.homeLat);
    preferences.putDouble("lon", config.homeLon);
    preferences.putInt("radius", config.radiusNm);
}

String setupPageHtml() {
    String html;
    html.reserve(2400);
    html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
    html += F("<title>Flight Tracker Tablet</title><style>");
    html += F("body{margin:0;font-family:system-ui,-apple-system,Segoe UI,sans-serif;background:#07111d;color:#eef8fb}");
    html += F("main{max-width:680px;margin:0 auto;padding:28px}label{display:block;margin:16px 0 6px;color:#9fc3ce}");
    html += F("input{box-sizing:border-box;width:100%;padding:12px;border:1px solid #31586a;background:#102231;color:#fff;border-radius:6px;font-size:16px}");
    html += F("button{margin-top:20px;padding:12px 18px;border:0;border-radius:6px;background:#ffba49;color:#111;font-weight:700;font-size:16px}");
    html += F(".row{display:grid;grid-template-columns:1fr 1fr;gap:12px}.muted{color:#9fc3ce}.card{background:#0d1d2a;border:1px solid #25485a;padding:18px;border-radius:8px}");
    html += F("</style></head><body><main><h1>Flight Tracker Tablet</h1><p class='muted'>Configure Wi-Fi and your map center. Use coordinates, not a street address.</p>");
    html += F("<section class='card'><form method='post' action='/save'>");
    html += F("<label>Wi-Fi SSID</label><input name='ssid' maxlength='63' value='");
    html += htmlEscaped(config.ssid);
    html += F("'><label>Wi-Fi Password</label><input name='pass' type='password' maxlength='63' value='");
    html += htmlEscaped(config.password);
    html += F("'><div class='row'><div><label>Latitude</label><input name='lat' inputmode='decimal' value='");
    html += String(config.homeLat, 6);
    html += F("'></div><div><label>Longitude</label><input name='lon' inputmode='decimal' value='");
    html += String(config.homeLon, 6);
    html += F("'></div></div><label>Radius, nautical miles</label><input name='radius' type='number' min='1' max='250' value='");
    html += config.radiusNm;
    html += F("'><button>Save and Reboot</button></form></section>");
    html += F("<p class='muted'>Setup AP: ");
    html += kSetupApSsid;
    html += F(" / password: ");
    html += kSetupApPassword;
    html += F("</p></main></body></html>");
    return html;
}

void startSetupPortal() {
    if (setupPortalRunning) return;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(kSetupApSsid, kSetupApPassword);
    server.on("/", HTTP_GET, []() {
        server.send(200, "text/html", setupPageHtml());
    });
    server.on("/save", HTTP_POST, []() {
        config.ssid = server.arg("ssid");
        config.password = server.arg("pass");
        config.homeLat = server.arg("lat").toDouble();
        config.homeLon = server.arg("lon").toDouble();
        config.radiusNm = constrain(server.arg("radius").toInt(), 1, 250);
        saveConfig();
        server.send(200, "text/html", "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'><body style='font-family:system-ui;background:#07111d;color:#eef8fb;padding:28px'><h1>Saved</h1><p>Rebooting the flight tracker.</p></body>");
        delay(500);
        ESP.restart();
    });
    server.begin();
    setupPortalRunning = true;
    statusLine = "Setup AP " + WiFi.softAPIP().toString();
}

void drawText(int x, int y, const String &text, uint16_t color, uint8_t size = 1) {
    canvas->setTextColor(color);
    canvas->setTextSize(size);
    canvas->setCursor(x, y);
    canvas->print(text);
}

void flushDisplay() {
    if (canvas && canvas->getBuffer()) {
        wavesahre_rgb_lcd_display(reinterpret_cast<uint8_t *>(canvas->getBuffer()));
    }
}

void drawAircraftMarker(const Aircraft &a, int x, int y, uint16_t color) {
    float rad = (a.trackDeg - 90.0f) * DEG_TO_RAD;
    int noseX = x + cosf(rad) * 13;
    int noseY = y + sinf(rad) * 13;
    int leftX = x + cosf(rad + 2.45f) * 9;
    int leftY = y + sinf(rad + 2.45f) * 9;
    int rightX = x + cosf(rad - 2.45f) * 9;
    int rightY = y + sinf(rad - 2.45f) * 9;
    canvas->fillTriangle(noseX, noseY, leftX, leftY, rightX, rightY, color);
    canvas->drawCircle(x, y, 15, color);
}

void drawStaticFrame() {
    uint16_t bg = color565(6, 13, 22);
    uint16_t grid = color565(33, 68, 87);
    uint16_t soft = color565(82, 151, 174);
    uint16_t white = color565(234, 246, 250);
    uint16_t muted = color565(134, 157, 167);
    uint16_t accent = color565(255, 186, 73);

    canvas->fillScreen(bg);
    canvas->fillRect(kSidebarX, 0, kScreenW - kSidebarX, kScreenH, color565(12, 22, 32));
    canvas->drawFastVLine(kSidebarX, 0, kScreenH, color565(53, 85, 101));

    int cx = kMapW / 2;
    int cy = kScreenH / 2;
    for (int r = 90; r <= 270; r += 90) {
        canvas->drawCircle(cx, cy, r, grid);
    }
    canvas->drawFastHLine(44, cy, kMapW - 88, grid);
    canvas->drawFastVLine(cx, 44, kScreenH - 88, grid);
    canvas->fillCircle(cx, cy, 6, accent);
    drawText(cx - 18, cy + 12, "HOME", accent, 1);

    drawText(28, 24, "OVERHEAD FLIGHTS", white, 2);
    drawText(28, 50, String(config.radiusNm) + " nm radius", muted, 1);
    drawText(cx - 4, 20, "N", soft, 2);
    drawText(cx - 4, kScreenH - 38, "S", soft, 2);
    drawText(22, cy - 7, "W", soft, 2);
    drawText(kMapW - 34, cy - 7, "E", soft, 2);

    drawText(kSidebarX + 24, 24, "Nearest", white, 2);
    drawText(kSidebarX + 24, 554, statusLine, muted, 1);
    drawText(kSidebarX + 24, 572, "Updated " + lastUpdated, muted, 1);
}

void drawAircraft() {
    uint16_t plane = color565(89, 211, 255);
    uint16_t nearPlane = color565(255, 186, 73);
    uint16_t white = color565(234, 246, 250);
    uint16_t muted = color565(134, 157, 167);
    uint16_t divider = color565(40, 63, 76);
    int cx = kMapW / 2;
    int cy = kScreenH / 2;
    float radiusPx = min(kMapW, kScreenH) * 0.43f;

    for (size_t i = 0; i < aircraftCount; ++i) {
        Aircraft &a = aircraft[i];
        if (!a.valid) continue;
        double dLatNm = (a.lat - config.homeLat) * 60.0;
        double dLonNm = (a.lon - config.homeLon) * 60.0 * cos(config.homeLat * DEG_TO_RAD);
        int x = cx + (int)(dLonNm / config.radiusNm * radiusPx);
        int y = cy - (int)(dLatNm / config.radiusNm * radiusPx);
        if (x < 20 || x > kMapW - 20 || y < 20 || y > kScreenH - 20) continue;
        drawAircraftMarker(a, x, y, a.distNm < 3.0f ? nearPlane : plane);
        String label = trimCopy(a.flight);
        if (label.length() == 0) label = trimCopy(a.reg);
        if (label.length() > 0) drawText(x + 16, y - 4, label, white, 1);
    }

    int rowY = 66;
    size_t rows = min(aircraftCount, (size_t)9);
    for (size_t i = 0; i < rows; ++i) {
        Aircraft &a = aircraft[i];
        int y = rowY + i * 52;
        canvas->drawFastHLine(kSidebarX + 24, y + 42, 252, divider);
        String label = trimCopy(a.flight);
        if (label.length() == 0) label = trimCopy(a.reg);
        if (label.length() == 0) label = a.type;
        drawText(kSidebarX + 24, y, label, white, 2);
        drawText(kSidebarX + 170, y + 2, String(a.distNm, 1) + " nm", nearPlane, 1);
        drawText(kSidebarX + 24, y + 23, String(a.altitudeFt) + " ft  " + String((int)a.speedKt) + " kt  " + a.type, muted, 1);
    }

    if (aircraftCount == 0) {
        drawText(kSidebarX + 24, 82, "No aircraft in range", muted, 2);
    }
}

void render() {
    drawStaticFrame();
    if (!configured()) {
        drawText(104, 238, "Setup needed", color565(255, 186, 73), 3);
        drawText(104, 282, "Join Wi-Fi: " + String(kSetupApSsid), color565(234, 246, 250), 2);
        drawText(104, 314, "Password: " + String(kSetupApPassword), color565(234, 246, 250), 2);
        drawText(104, 346, "Open http://192.168.4.1", color565(234, 246, 250), 2);
    } else if (WiFi.status() != WL_CONNECTED) {
        drawText(210, 286, "Connecting to Wi-Fi...", color565(255, 186, 73), 2);
    } else {
        drawAircraft();
    }
    flushDisplay();
}

bool fetchAircraft() {
    if (WiFi.status() != WL_CONNECTED || !configured()) return false;

    WiFiClientSecure client;
    client.setInsecure();

    String url = "https://api.adsb.lol/v2/lat/" + String(config.homeLat, 6) +
                 "/lon/" + String(config.homeLon, 6) +
                 "/dist/" + String(config.radiusNm);

    HTTPClient http;
    http.setTimeout(10000);
    if (!http.begin(client, url)) {
        statusLine = "HTTP begin failed";
        return false;
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        statusLine = "ADS-B HTTP " + String(code);
        http.end();
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        statusLine = "JSON parse failed";
        return false;
    }

    aircraftCount = 0;
    for (JsonObject ac : doc["ac"].as<JsonArray>()) {
        if (aircraftCount >= kMaxAircraft) break;
        if (!ac["lat"].is<double>() || !ac["lon"].is<double>()) continue;
        Aircraft &a = aircraft[aircraftCount++];
        memset(&a, 0, sizeof(Aircraft));
        strlcpy(a.flight, ac["flight"] | "", sizeof(a.flight));
        strlcpy(a.reg, ac["r"] | "", sizeof(a.reg));
        strlcpy(a.type, ac["t"] | "", sizeof(a.type));
        a.lat = ac["lat"] | 0.0;
        a.lon = ac["lon"] | 0.0;
        a.altitudeFt = ac["alt_baro"] | 0;
        a.speedKt = ac["gs"] | 0.0;
        a.trackDeg = ac["track"] | 0.0;
        a.distNm = ac["dst"] | 0.0;
        a.bearingDeg = ac["dir"] | 0.0;
        a.valid = true;
    }

    statusLine = String(aircraftCount) + " aircraft tracked";
    lastUpdated = String(millis() / 1000) + "s";
    return true;
}

void connectWifi() {
    if (!configured()) return;
    WiFi.mode(setupPortalRunning ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(config.ssid.c_str(), config.password.c_str());
    statusLine = "Connecting Wi-Fi";
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; ++i) {
        delay(250);
        render();
    }
    statusLine = WiFi.status() == WL_CONNECTED ? "Wi-Fi connected" : "Wi-Fi offline";
    if (WiFi.status() != WL_CONNECTED) {
        startSetupPortal();
    }
}
}

void setup() {
    Serial.begin(115200);
    delay(100);
    if (!psramFound()) {
        Serial.println("PSRAM not found; LCD framebuffer cannot be allocated");
        while (true) delay(1000);
    }

    DEV_I2C_Init();
    IO_EXTENSION_Init();
    IO_EXTENSION_Output(kTouchResetExio, 1);
    IO_EXTENSION_Output(kLcdVddEnableExio, 1);
    delay(20);
    IO_EXTENSION_Output(kLcdResetExio, 1);
    delay(20);
    lcd = waveshare_esp32_s3_rgb_lcd_init();
    wavesahre_rgb_lcd_bl_on();

    frameBuffer = static_cast<uint16_t *>(heap_caps_malloc(kScreenW * kScreenH * sizeof(uint16_t),
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    canvas = new PsramCanvas16(kScreenW, kScreenH, frameBuffer);
    if (!canvas || !frameBuffer) {
        Serial.println("Failed to allocate canvas");
        while (true) delay(1000);
    }

    loadConfig();
    if (!configured()) {
        startSetupPortal();
    }
    render();
    connectWifi();
    fetchAircraft();
    render();
    lastFetchMs = millis();
}

void loop() {
    if (setupPortalRunning) {
        server.handleClient();
    }

    if (configured() && WiFi.status() != WL_CONNECTED) {
        connectWifi();
    }

    if (millis() - lastFetchMs >= kFetchEveryMs) {
        lastFetchMs = millis();
        fetchAircraft();
        render();
    }

    delay(50);
}
