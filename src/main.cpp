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
#include "gt911.h"
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
constexpr int kMapCx = kMapW / 2;
constexpr int kMapCy = kScreenH / 2;
constexpr int kMapRadiusPx = 258;

struct Aircraft {
    char hex[12] = "";
    char flight[12] = "";
    char reg[12] = "";
    char type[10] = "";
    double lat = 0;
    double lon = 0;
    double prevLat = 0;
    double prevLon = 0;
    int altitudeFt = 0;
    float speedKt = 0;
    float trackDeg = 0;
    float distNm = 0;
    float bearingDeg = 0;
    float seenSec = 0;
    bool hasPrev = false;
    bool valid = false;
};

struct AppConfig {
    String ssid;
    String password;
    double homeLat = HOME_LAT_DEFAULT;
    double homeLon = HOME_LON_DEFAULT;
    int radiusNm = TRACK_RADIUS_NM_DEFAULT;
    int alertRadiusNm = 2;
    int alertAltitudeFt = 10000;
    int refreshSeconds = 15;
    int brightness = 95;
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
esp_lcd_touch_handle_t touch = nullptr;
Preferences preferences;
WebServer server(80);
AppConfig config;
Aircraft aircraft[kMaxAircraft];
size_t aircraftCount = 0;
uint32_t lastFetchMs = 0;
uint32_t lastRenderMs = 0;
uint32_t lastTouchMs = 0;
uint32_t fetchEveryMs = kFetchEveryMs;
String statusLine = "Booting";
String lastUpdated = "never";
bool setupPortalRunning = false;
bool touchReady = false;
bool touchWasDown = false;
bool showSettings = false;
int selectedAircraft = -1;

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
    config.alertRadiusNm = constrain(preferences.getInt("alertNm", 2), 1, 50);
    config.alertAltitudeFt = constrain(preferences.getInt("alertAlt", 10000), 0, 60000);
    config.refreshSeconds = constrain(preferences.getInt("refresh", 15), 5, 120);
    config.brightness = constrain(preferences.getInt("bright", 95), 15, 95);
    fetchEveryMs = static_cast<uint32_t>(config.refreshSeconds) * 1000UL;
}

void saveConfig() {
    preferences.putString("ssid", config.ssid);
    preferences.putString("pass", config.password);
    preferences.putDouble("lat", config.homeLat);
    preferences.putDouble("lon", config.homeLon);
    preferences.putInt("radius", config.radiusNm);
    preferences.putInt("alertNm", config.alertRadiusNm);
    preferences.putInt("alertAlt", config.alertAltitudeFt);
    preferences.putInt("refresh", config.refreshSeconds);
    preferences.putInt("bright", config.brightness);
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
    html += F("'><div class='row'><div><label>Alert radius, nm</label><input name='alertNm' type='number' min='1' max='50' value='");
    html += config.alertRadiusNm;
    html += F("'></div><div><label>Alert below altitude, ft</label><input name='alertAlt' type='number' min='0' max='60000' step='500' value='");
    html += config.alertAltitudeFt;
    html += F("'></div></div><div class='row'><div><label>Refresh seconds</label><input name='refresh' type='number' min='5' max='120' value='");
    html += config.refreshSeconds;
    html += F("'></div><div><label>Brightness %</label><input name='bright' type='number' min='15' max='95' value='");
    html += config.brightness;
    html += F("'></div></div><button>Save and Reboot</button></form></section>");
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
        config.alertRadiusNm = constrain(server.arg("alertNm").toInt(), 1, 50);
        config.alertAltitudeFt = constrain(server.arg("alertAlt").toInt(), 0, 60000);
        config.refreshSeconds = constrain(server.arg("refresh").toInt(), 5, 120);
        config.brightness = constrain(server.arg("bright").toInt(), 15, 95);
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

void applyBrightness() {
    IO_EXTENSION_Pwm_Output(constrain(config.brightness, 15, 95));
}

String aircraftLabel(const Aircraft &a) {
    String label = trimCopy(a.flight);
    if (label.length() == 0) label = trimCopy(a.reg);
    if (label.length() == 0) label = trimCopy(a.type);
    if (label.length() == 0) label = trimCopy(a.hex);
    return label;
}

uint16_t altitudeColor(const Aircraft &a) {
    if (a.distNm <= config.alertRadiusNm && (config.alertAltitudeFt == 0 || a.altitudeFt <= config.alertAltitudeFt)) {
        return color565(255, 186, 73);
    }
    if (a.altitudeFt <= 2500) return color565(255, 99, 99);
    if (a.altitudeFt <= 10000) return color565(95, 231, 177);
    if (a.altitudeFt <= 25000) return color565(89, 211, 255);
    return color565(173, 154, 255);
}

bool mapPoint(double lat, double lon, int &x, int &y) {
    if (config.radiusNm <= 0) return false;
    double dLatNm = (lat - config.homeLat) * 60.0;
    double dLonNm = (lon - config.homeLon) * 60.0 * cos(config.homeLat * DEG_TO_RAD);
    x = kMapCx + (int)(dLonNm / config.radiusNm * kMapRadiusPx);
    y = kMapCy - (int)(dLatNm / config.radiusNm * kMapRadiusPx);
    return x >= 20 && x <= kMapW - 20 && y >= 20 && y <= kScreenH - 20;
}

int nearestAircraftIndex() {
    int best = -1;
    float bestDist = 99999.0f;
    for (size_t i = 0; i < aircraftCount; ++i) {
        if (!aircraft[i].valid) continue;
        if (aircraft[i].distNm < bestDist) {
            bestDist = aircraft[i].distNm;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int findAircraftByHex(const char *hex, Aircraft *list, size_t count) {
    if (!hex || !hex[0]) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(list[i].hex, hex) == 0) return static_cast<int>(i);
    }
    return -1;
}

void sortAircraftByDistance() {
    for (size_t i = 0; i < aircraftCount; ++i) {
        for (size_t j = i + 1; j < aircraftCount; ++j) {
            if (aircraft[j].distNm < aircraft[i].distNm) {
                Aircraft tmp = aircraft[i];
                aircraft[i] = aircraft[j];
                aircraft[j] = tmp;
            }
        }
    }
    if (selectedAircraft >= static_cast<int>(aircraftCount)) {
        selectedAircraft = -1;
    }
}

int displayAircraftIndex() {
    if (selectedAircraft >= 0 && selectedAircraft < static_cast<int>(aircraftCount) && aircraft[selectedAircraft].valid) {
        return selectedAircraft;
    }
    return nearestAircraftIndex();
}

bool aircraftAlerts(const Aircraft &a) {
    return a.valid && a.distNm <= config.alertRadiusNm &&
           (config.alertAltitudeFt == 0 || a.altitudeFt <= config.alertAltitudeFt);
}

void drawButton(int x, int y, int w, int h, const String &label, uint16_t border, uint16_t fill, uint16_t text) {
    canvas->fillRect(x, y, w, h, fill);
    canvas->drawRect(x, y, w, h, border);
    canvas->drawRect(x + 1, y + 1, w - 2, h - 2, border);
    int textX = x + 10;
    if (label.length() <= 3) textX = x + (w - label.length() * 12) / 2;
    drawText(textX, y + (h - 14) / 2, label, text, 2);
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

    for (int i = 1; i <= 4; ++i) {
        int r = (kMapRadiusPx * i) / 4;
        int nm = (config.radiusNm * i) / 4;
        canvas->drawCircle(kMapCx, kMapCy, r, grid);
        drawText(kMapCx + r + 8, kMapCy - 5, String(nm) + " nm", muted, 1);
    }

    for (int deg = 0; deg < 360; deg += 45) {
        float rad = (deg - 90) * DEG_TO_RAD;
        int x = kMapCx + cosf(rad) * kMapRadiusPx;
        int y = kMapCy + sinf(rad) * kMapRadiusPx;
        canvas->drawLine(kMapCx, kMapCy, x, y, grid);
    }
    canvas->drawFastHLine(44, kMapCy, kMapW - 88, grid);
    canvas->drawFastVLine(kMapCx, 44, kScreenH - 88, grid);
    canvas->fillCircle(kMapCx, kMapCy, 6, accent);
    canvas->drawCircle(kMapCx, kMapCy, 12, accent);
    drawText(kMapCx - 18, kMapCy + 16, "HOME", accent, 1);

    drawText(28, 24, "OVERHEAD FLIGHTS", white, 2);
    drawText(28, 50, String(config.radiusNm) + " nm radius  refresh " + String(config.refreshSeconds) + "s", muted, 1);
    drawText(kMapCx - 4, 20, "N", soft, 2);
    drawText(kMapCx - 4, kScreenH - 38, "S", soft, 2);
    drawText(22, kMapCy - 7, "W", soft, 2);
    drawText(kMapW - 34, kMapCy - 7, "E", soft, 2);

    drawText(kSidebarX + 24, 20, "Selected", white, 2);
    drawText(kSidebarX + 24, 550, statusLine, muted, 1);
    drawText(kSidebarX + 24, 568, "Updated " + lastUpdated, muted, 1);

    uint16_t fill = color565(16, 34, 48);
    uint16_t border = color565(53, 85, 101);
    drawButton(kSidebarX + 22, 500, 58, 36, "R-", border, fill, white);
    drawButton(kSidebarX + 86, 500, 58, 36, "R+", border, fill, white);
    drawButton(kSidebarX + 150, 500, 58, 36, "DIM", border, fill, white);
    drawButton(kSidebarX + 214, 500, 58, 36, "SET", showSettings ? accent : border, fill, white);
}

void drawAircraft() {
    uint16_t white = color565(234, 246, 250);
    uint16_t muted = color565(134, 157, 167);
    uint16_t divider = color565(40, 63, 76);
    uint16_t panel = color565(16, 34, 48);
    uint16_t alert = color565(255, 186, 73);
    int shownIndex = displayAircraftIndex();
    int alertIndex = -1;

    for (size_t i = 0; i < aircraftCount; ++i) {
        Aircraft &a = aircraft[i];
        if (!a.valid) continue;
        int x, y;
        if (!mapPoint(a.lat, a.lon, x, y)) continue;
        uint16_t color = altitudeColor(a);
        if (a.hasPrev) {
            int px, py;
            if (mapPoint(a.prevLat, a.prevLon, px, py)) {
                canvas->drawLine(px, py, x, y, color565(54, 94, 112));
                canvas->fillCircle(px, py, 3, color565(54, 94, 112));
            }
        }
        if (aircraftAlerts(a) && alertIndex < 0) alertIndex = static_cast<int>(i);
        if (static_cast<int>(i) == shownIndex) {
            canvas->drawCircle(x, y, 22, white);
            canvas->drawCircle(x, y, 24, color);
        }
        drawAircraftMarker(a, x, y, color);
        String label = aircraftLabel(a);
        if (label.length() > 0) drawText(x + 16, y - 4, label, white, 1);
    }

    if (alertIndex >= 0) {
        Aircraft &a = aircraft[alertIndex];
        canvas->fillRect(28, 78, kMapW - 56, 44, alert);
        drawText(46, 92, "OVERHEAD ALERT  " + aircraftLabel(a) + "  " + String(a.distNm, 1) + " nm  " + String(a.altitudeFt) + " ft", color565(10, 15, 20), 2);
    }

    if (shownIndex >= 0) {
        Aircraft &a = aircraft[shownIndex];
        canvas->fillRect(kSidebarX + 20, 58, 260, 148, panel);
        canvas->drawRect(kSidebarX + 20, 58, 260, 148, altitudeColor(a));
        drawText(kSidebarX + 34, 74, aircraftLabel(a), white, 2);
        drawText(kSidebarX + 34, 103, trimCopy(a.reg) + "  " + trimCopy(a.type), muted, 1);
        drawText(kSidebarX + 34, 126, String(a.distNm, 1) + " nm  " + String((int)a.bearingDeg) + " deg", altitudeColor(a), 2);
        drawText(kSidebarX + 34, 157, String(a.altitudeFt) + " ft  " + String((int)a.speedKt) + " kt", white, 1);
        drawText(kSidebarX + 34, 178, "Track " + String((int)a.trackDeg) + "  seen " + String(a.seenSec, 1) + "s", muted, 1);
    }

    int rowY = 226;
    size_t rows = min(aircraftCount, (size_t)9);
    for (size_t i = 0; i < rows; ++i) {
        Aircraft &a = aircraft[i];
        int y = rowY + i * 30;
        if (static_cast<int>(i) == shownIndex) {
            canvas->fillRect(kSidebarX + 18, y - 3, 264, 27, color565(25, 53, 68));
        }
        canvas->drawFastHLine(kSidebarX + 24, y + 24, 252, divider);
        String label = aircraftLabel(a);
        drawText(kSidebarX + 24, y, label, white, 2);
        drawText(kSidebarX + 178, y + 3, String(a.distNm, 1) + " nm", altitudeColor(a), 1);
    }

    if (aircraftCount == 0) {
        drawText(kSidebarX + 24, 82, "No aircraft in range", muted, 2);
    }

    if (showSettings) {
        canvas->fillRect(76, 136, 572, 328, color565(12, 22, 32));
        canvas->drawRect(76, 136, 572, 328, color565(89, 211, 255));
        drawText(106, 166, "TRACKER SETTINGS", white, 3);
        drawText(106, 218, "Radius: " + String(config.radiusNm) + " nm", muted, 2);
        drawText(106, 254, "Alert: " + String(config.alertRadiusNm) + " nm below " + String(config.alertAltitudeFt) + " ft", muted, 2);
        drawText(106, 290, "Refresh: " + String(config.refreshSeconds) + " seconds", muted, 2);
        drawText(106, 326, "Brightness: " + String(config.brightness) + "%", muted, 2);
        drawButton(106, 380, 92, 46, "R-", divider, panel, white);
        drawButton(214, 380, 92, 46, "R+", divider, panel, white);
        drawButton(322, 380, 92, 46, "DIM", divider, panel, white);
        drawButton(430, 380, 92, 46, "REF", divider, panel, white);
        drawText(106, 438, "Tap aircraft list to select. SET closes this panel.", muted, 1);
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

void adjustRadius(int delta) {
    config.radiusNm = constrain(config.radiusNm + delta, 1, 250);
    saveConfig();
    render();
}

void adjustBrightness(int delta) {
    config.brightness = constrain(config.brightness + delta, 15, 95);
    applyBrightness();
    saveConfig();
    render();
}

void adjustRefresh() {
    if (config.refreshSeconds < 10) config.refreshSeconds = 10;
    else if (config.refreshSeconds < 15) config.refreshSeconds = 15;
    else if (config.refreshSeconds < 30) config.refreshSeconds = 30;
    else if (config.refreshSeconds < 60) config.refreshSeconds = 60;
    else config.refreshSeconds = 5;
    fetchEveryMs = static_cast<uint32_t>(config.refreshSeconds) * 1000UL;
    saveConfig();
    render();
}

bool inRect(uint16_t x, uint16_t y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

void handleTap(uint16_t x, uint16_t y) {
    if (showSettings) {
        if (inRect(x, y, 106, 380, 92, 46)) {
            adjustRadius(-1);
            return;
        }
        if (inRect(x, y, 214, 380, 92, 46)) {
            adjustRadius(1);
            return;
        }
        if (inRect(x, y, 322, 380, 92, 46)) {
            adjustBrightness(config.brightness > 35 ? -20 : 60);
            return;
        }
        if (inRect(x, y, 430, 380, 92, 46)) {
            adjustRefresh();
            return;
        }
    }

    if (inRect(x, y, kSidebarX + 22, 500, 58, 36)) {
        adjustRadius(-1);
        return;
    }
    if (inRect(x, y, kSidebarX + 86, 500, 58, 36)) {
        adjustRadius(1);
        return;
    }
    if (inRect(x, y, kSidebarX + 150, 500, 58, 36)) {
        adjustBrightness(config.brightness > 35 ? -20 : 60);
        return;
    }
    if (inRect(x, y, kSidebarX + 214, 500, 58, 36)) {
        showSettings = !showSettings;
        render();
        return;
    }

    if (x >= kSidebarX && y >= 226 && y < 496) {
        int row = (y - 226) / 30;
        if (row >= 0 && row < static_cast<int>(aircraftCount)) {
            selectedAircraft = row;
            showSettings = false;
            render();
        }
        return;
    }

    for (size_t i = 0; i < aircraftCount; ++i) {
        int ax, ay;
        if (!mapPoint(aircraft[i].lat, aircraft[i].lon, ax, ay)) continue;
        int dx = static_cast<int>(x) - ax;
        int dy = static_cast<int>(y) - ay;
        if (dx * dx + dy * dy <= 30 * 30) {
            selectedAircraft = static_cast<int>(i);
            showSettings = false;
            render();
            return;
        }
    }
}

void pollTouch() {
    if (!touchReady) return;
    touch_gt911_point_t point = touch_gt911_read_point(1);
    bool down = point.cnt > 0;
    if (down && !touchWasDown && millis() - lastTouchMs > 220) {
        lastTouchMs = millis();
        handleTap(point.x[0], point.y[0]);
    }
    touchWasDown = down;
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

    Aircraft oldAircraft[kMaxAircraft];
    size_t oldCount = aircraftCount;
    memcpy(oldAircraft, aircraft, sizeof(oldAircraft));

    aircraftCount = 0;
    for (JsonObject ac : doc["ac"].as<JsonArray>()) {
        if (aircraftCount >= kMaxAircraft) break;
        if (!ac["lat"].is<double>() || !ac["lon"].is<double>()) continue;
        Aircraft &a = aircraft[aircraftCount++];
        memset(&a, 0, sizeof(Aircraft));
        strlcpy(a.hex, ac["hex"] | "", sizeof(a.hex));
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
        a.seenSec = ac["seen"] | 0.0;
        int prev = findAircraftByHex(a.hex, oldAircraft, oldCount);
        if (prev >= 0) {
            a.prevLat = oldAircraft[prev].lat;
            a.prevLon = oldAircraft[prev].lon;
            a.hasPrev = true;
        }
        a.valid = true;
    }

    sortAircraftByDistance();
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
    touch = touch_gt911_init();
    touchReady = touch != nullptr;

    frameBuffer = static_cast<uint16_t *>(heap_caps_malloc(kScreenW * kScreenH * sizeof(uint16_t),
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    canvas = new PsramCanvas16(kScreenW, kScreenH, frameBuffer);
    if (!canvas || !frameBuffer) {
        Serial.println("Failed to allocate canvas");
        while (true) delay(1000);
    }

    loadConfig();
    applyBrightness();
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
    pollTouch();

    if (setupPortalRunning) {
        server.handleClient();
    }

    if (configured() && WiFi.status() != WL_CONNECTED) {
        connectWifi();
    }

    if (millis() - lastFetchMs >= fetchEveryMs) {
        lastFetchMs = millis();
        fetchAircraft();
        render();
    }

    delay(50);
}
