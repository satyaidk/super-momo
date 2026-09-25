/* =============================================================================
   Mochi Terminal  -  eye bot + info terminal in one sketch (ESP32-C3)
   -----------------------------------------------------------------------------
   Two modes, switched with the touch pad:

     EYES      (boot mode)  Mochi's animated eyes: they look around, rest,
                            twitch and blink like real eyes.
                              tap          -> Mochi looks at you and blinks
                              hold 5 s     -> switch to TERMINAL

     TERMINAL               Clock, Weather, Crypto and Notes screens.
                              tap          -> next screen
                              hold 1.5 s   -> (short beep) release to refresh data
                              hold 5 s     -> switch back to EYES

   While the pad is held, a bar fills along the bottom of the screen; when it
   is full (5 s) the mode switches.

   The web page (http://terminal.local) works in both modes. Saving a note
   switches to TERMINAL and shows the Notes screen.

   The eye animation runs as a non-blocking state machine, so the touch pad,
   buzzer and web server stay responsive while the eyes move. Weather and
   crypto are only fetched in TERMINAL mode (a fetch can take a few seconds
   and would freeze the eyes); stale data is refreshed as soon as you switch.

   Board settings:
     Board           : ESP32C3 Dev Module
     USB CDC On Boot : Enabled          <-- important, see note below
     Flash Mode      : QIO (or DIO if your board fails to boot)
     Partition       : Default 4MB with spiffs

   NOTE ON PINS: GPIO20/21 are UART0. With "USB CDC On Boot = Enabled" they are
   free for I2C. If you cannot enable that setting, rewire the OLED to
   SDA=GPIO5, SCL=GPIO6 and change the two defines below.

   Libraries: Adafruit SSD1306, Adafruit GFX, ArduinoJson v7
   Files    : animations.h and anim_eye.h must sit next to this .ino
   ============================================================================= */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <time.h>
#include <vector>
#include "animations.h"

/* ------------------------------ PIN MAP ------------------------------------ */
// Confirmed by the I2C scanner: OLED answers on SDA=20, SCL=21 at 0x3C
#define PIN_SDA        20
#define PIN_SCL        21
#define PIN_TOUCH       1     // confirmed working
#define PIN_BUZZER      2

#define I2C_AUTODETECT  true     // sweep other pins if the ones above fail

/* ------------------------------ SETTINGS ----------------------------------- */
#define WIFI_SSID_DEFAULT  "SRI GAYATHRI 27 2F_2.4G"
#define WIFI_PASS_DEFAULT  "gayathri@123"

#define AP_SSID     "Kitty"
#define AP_PASS     "12345678"
#define MDNS_NAME   "terminal"

#define TZ_STRING   "IST-5:30"          // India. POSIX sign is inverted.
#define UNITS_METRIC   true
#define SOUND_ON       true

#define WEATHER_REFRESH_MS   (15UL * 60UL * 1000UL)
#define CRYPTO_REFRESH_MS    (60UL * 1000UL)
#define NOTE_PAGE_MS         (5UL * 1000UL)

/* ---- Touch timings -------------------------------------------------------- */
#define HOLD_REFRESH_MS   1500   // TERMINAL: hold this long, then release to refresh
#define HOLD_SWITCH_MS    5000   // hold this long to switch between EYES and TERMINAL
#define HOLD_HINT_MS       600   // the progress bar appears after this much holding

/* ---- Eye behaviour (all times in milliseconds) ---------------------------- */
#define OPEN_MIN_MS       10000  // eyes stay open between blinks for a random time
#define OPEN_MAX_MS       15000  //   between these two values
#define BLINK_FRAME_MS    35     // per blink frame: 10 frames ~ 0.35 s, like a real blink
#define DOUBLE_BLINK_PCT  15     // chance (%) a blink is followed by a quick second one
#define DOUBLE_BLINK_GAP  120    // pause between the two blinks of a double blink

#define LOOK_X_PX         6      // how far the eyes move left/right. The art has 4 px spare
                                 //   on each side, so above 4 the outer rims get slightly clipped
#define LOOK_Y_PX         5      // how far the eyes move up/down (there is ~20 px spare)
#define LOOK_STEP_MS      15     // per pixel while moving (fast, like a real eye movement)
#define REST_MIN_MS       1200   // how long the eyes rest on one spot before moving on
#define REST_MAX_MS       3500
#define CENTER_PCT        40     // chance (%) the next spot is straight ahead again
#define TWITCH_MIN_MS     600    // time between tiny 1 px movements while resting
#define TWITCH_MAX_MS     1600   //   (set TWITCH_MIN_MS to 0 to turn them off)
#define BLINK_SHIFT_PCT   45     // chance (%) a blink also moves the gaze (close looking one
                                 //   way, open looking another)

/* ---- Serial guard: do not start Serial if it would steal the I2C pins ----- */
#if ARDUINO_USB_CDC_ON_BOOT
  #define SERIAL_SAFE 1
#else
  #define SERIAL_SAFE ((PIN_SDA != 20) && (PIN_SDA != 21) && (PIN_SCL != 20) && (PIN_SCL != 21))
#endif

#if SERIAL_SAFE
  #define LOG(...)    Serial.printf(__VA_ARGS__)
  #define LOGLN(x)    Serial.println(x)
#else
  #define LOG(...)    do{}while(0)
  #define LOGLN(x)    do{}while(0)
#endif

/* ------------------------------ DISPLAY ------------------------------------ */
#define SCREEN_W 128
#define SCREEN_H 64
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

bool    haveDisplay = false;
uint8_t oledAddr    = 0x3C;
uint8_t usedSda     = PIN_SDA;
uint8_t usedScl     = PIN_SCL;

/* ------------------------------ STATE -------------------------------------- */
enum Mode : uint8_t { MODE_EYES = 0, MODE_TERMINAL };
Mode mode = MODE_EYES;                  // the bot starts with its eyes

enum Screen { SCR_CLOCK = 0, SCR_WEATHER, SCR_CRYPTO, SCR_NOTES, SCR_COUNT };
uint8_t screen = SCR_CLOCK;

WebServer server(80);
Preferences prefs;

String wifiSsid, wifiPass;
String noteTitle = "Notes";
String noteText  = "Tap the pad to change screens. Hold 5s for the eyes.\n\nOpen http://terminal.local\nto write here.";
std::vector<String> noteLines;
uint8_t notePage = 0;

String  wxCity = "";
float   wxTemp = 0, wxMin = 0, wxMax = 0, wxWind = 0;
int     wxHum = 0, wxCode = -1;
bool    wxValid = false;
double  geoLat = 0, geoLon = 0;
bool    geoValid = false;

struct Coin { const char* id; const char* tag; float usd; float chg; bool ok; };
Coin coins[3] = {
  { "bitcoin",  "BTC", 0, 0, false },
  { "ethereum", "ETH", 0, 0, false },
  { "solana",   "SOL", 0, 0, false }
};

bool apMode = false;
unsigned long lastWeather = 0, lastCrypto = 0, lastDraw = 0, lastNoteFlip = 0, lastPoll = 0;
unsigned long beepUntil = 0;

void setMode(Mode m);     // defined further down, used by the web handlers
void drawHoldBar();       // defined further down, used by render()

/* ============================ SOUND ========================================= */
void beep(unsigned int freq, unsigned long ms) {
  if (!SOUND_ON) return;
  tone(PIN_BUZZER, freq);
  beepUntil = millis() + ms;
}
void beepBlocking(unsigned int freq, unsigned long ms) {
  if (!SOUND_ON) { delay(ms); return; }
  tone(PIN_BUZZER, freq); delay(ms); noTone(PIN_BUZZER);
}
void serviceBeep() {
  if (beepUntil && millis() > beepUntil) { noTone(PIN_BUZZER); beepUntil = 0; }
}

/* ============================ TEXT HELPERS ================================== */
void centerText(const String& s, int y, uint8_t size) {
  if (!haveDisplay) return;
  int16_t x1, y1; uint16_t w, h;
  display.setTextSize(size);
  display.getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_W - (int)w) / 2, y);
  display.print(s);
}

void wrapInto(const String& src, uint8_t maxChars, std::vector<String>& out) {
  out.clear();
  int start = 0;
  while (start <= (int)src.length()) {
    int nl = src.indexOf('\n', start);
    String para = (nl < 0) ? src.substring(start) : src.substring(start, nl);
    para.replace("\r", "");
    if (para.length() == 0) out.push_back("");
    while (para.length() > 0) {
      if (para.length() <= maxChars) { out.push_back(para); break; }
      int cut = -1;
      for (int i = maxChars; i > 0; i--) if (para.charAt(i) == ' ') { cut = i; break; }
      if (cut <= 0) cut = maxChars;
      out.push_back(para.substring(0, cut));
      para = para.substring(cut);
      para.trim();
    }
    if (nl < 0) break;
    start = nl + 1;
  }
  if (out.empty()) out.push_back("");
}
void rebuildNoteLines() { wrapInto(noteText, 21, noteLines); notePage = 0; }

/* ============================ DISPLAY BRING-UP ============================== */
bool probe(uint8_t sda, uint8_t scl, uint8_t addr) {
  Wire.end();
  delay(5);
  if (!Wire.begin(sda, scl, 100000)) return false;
  delay(10);
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool startDisplay() {
  const uint8_t addrs[] = {0x3C, 0x3D};

  // 1. the configured pins
  for (uint8_t a : addrs) {
    if (probe(PIN_SDA, PIN_SCL, a)) { usedSda = PIN_SDA; usedScl = PIN_SCL; oledAddr = a; goto found; }
  }
  if (!I2C_AUTODETECT) return false;

  // 2. common alternatives, then a full sweep
  {
    LOGLN("OLED not on configured pins, searching...");
    const uint8_t common[][2] = {{8,9},{9,8},{5,6},{6,5},{4,5},{6,7},{7,6},{2,3},{10,8},{20,21},{21,20}};
    for (auto &p : common)
      for (uint8_t a : addrs)
        if (probe(p[0], p[1], a)) { usedSda = p[0]; usedScl = p[1]; oledAddr = a; goto found; }

    const uint8_t pins[] = {0,1,2,3,4,5,6,7,8,9,10,20,21};
    for (uint8_t i = 0; i < sizeof(pins); i++)
      for (uint8_t j = 0; j < sizeof(pins); j++) {
        if (i == j) continue;
        for (uint8_t a : addrs)
          if (probe(pins[i], pins[j], a)) { usedSda = pins[i]; usedScl = pins[j]; oledAddr = a; goto found; }
      }
  }
  return false;

found:
  LOG("OLED found: SDA=%d SCL=%d addr=0x%02X\n", usedSda, usedScl, oledAddr);
  Wire.end();
  delay(5);
  Wire.begin(usedSda, usedScl, 400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, oledAddr)) {
    LOGLN("SSD1306 responded on I2C but begin() failed");
    return false;
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.display();
  return true;
}

/* ============================ STORAGE ======================================= */
void loadConfig() {
  prefs.begin("terminal", true);
  wifiSsid  = prefs.getString("ssid",  WIFI_SSID_DEFAULT);
  wifiPass  = prefs.getString("pass",  WIFI_PASS_DEFAULT);
  noteTitle = prefs.getString("title", noteTitle);
  noteText  = prefs.getString("text",  noteText);
  prefs.end();
  rebuildNoteLines();
}
void saveNote() {
  prefs.begin("terminal", false);
  prefs.putString("title", noteTitle);
  prefs.putString("text",  noteText);
  prefs.end();
}
void saveWifi(const String& s, const String& p) {
  prefs.begin("terminal", false);
  prefs.putString("ssid", s);
  prefs.putString("pass", p);
  prefs.end();
}

/* ============================ NETWORK ======================================= */
const char* wifiReason(wl_status_t s) {
  switch (s) {
    case WL_NO_SSID_AVAIL: return "network not found (check name, and that it is 2.4GHz)";
    case WL_CONNECT_FAILED: return "wrong password";
    case WL_IDLE_STATUS:   return "idle";
    case WL_DISCONNECTED:  return "disconnected";
    default:               return "timed out";
  }
}

bool connectWifi(uint32_t timeoutMs = 20000) {
  if (wifiSsid.length() == 0 || wifiSsid == "YOUR_WIFI_NAME") {
    LOGLN("No Wi-Fi credentials set in the sketch.");
    return false;
  }
  LOG("Connecting to \"%s\"\n", wifiSsid.c_str());
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(true);
  delay(200);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    if (haveDisplay) {
      display.clearDisplay();
      centerText("Connecting", 18, 1);
      centerText(wifiSsid.substring(0, 20), 32, 1);
      String d = ""; for (int i = 0; i < (int)(((millis() - t0) / 400) % 4); i++) d += ".";
      centerText(d, 46, 1);
      display.display();
    }
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    LOG("Connected. IP: %s  RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  LOG("Wi-Fi failed: %s\n", wifiReason((wl_status_t)WiFi.status()));
  return false;
}

void startAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  LOG("Hotspot started: %s  ->  http://%s/\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

bool fetchJson(const String& url, JsonDocument& doc) {
  if (WiFi.status() != WL_CONNECTED) return false;
  int code = -1;
  String payload;

  if (url.startsWith("https")) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(12000);
    http.setUserAgent("esp32-terminal");
    if (http.begin(client, url)) {
      code = http.GET();
      if (code == HTTP_CODE_OK) payload = http.getString();
      http.end();
    }
  } else {
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(12000);
    if (http.begin(client, url)) {
      code = http.GET();
      if (code == HTTP_CODE_OK) payload = http.getString();
      http.end();
    }
  }
  if (code != HTTP_CODE_OK || payload.length() == 0) {
    LOG("HTTP %d for %s\n", code, url.substring(0, 48).c_str());
    return false;
  }
  DeserializationError e = deserializeJson(doc, payload);
  if (e) { LOG("JSON error: %s\n", e.c_str()); return false; }
  return true;
}

bool fetchLocation() {
  JsonDocument doc;
  if (!fetchJson("http://ip-api.com/json/?fields=status,city,lat,lon", doc)) return false;
  if (String((const char*)(doc["status"] | "")) != "success") return false;
  wxCity   = String((const char*)(doc["city"] | "Here"));
  geoLat   = doc["lat"] | 0.0;
  geoLon   = doc["lon"] | 0.0;
  geoValid = true;
  LOG("Location: %s (%.3f, %.3f)\n", wxCity.c_str(), geoLat, geoLon);
  return true;
}

bool fetchWeather() {
  if (!geoValid && !fetchLocation()) return false;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(geoLat, 4) +
               "&longitude=" + String(geoLon, 4) +
               "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m" +
               "&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto";
  if (!UNITS_METRIC) url += "&temperature_unit=fahrenheit&wind_speed_unit=mph";

  JsonDocument doc;
  if (!fetchJson(url, doc)) return false;
  JsonObject cur = doc["current"];
  if (cur.isNull()) return false;
  wxTemp = cur["temperature_2m"]       | 0.0f;
  wxHum  = cur["relative_humidity_2m"] | 0;
  wxCode = cur["weather_code"]         | -1;
  wxWind = cur["wind_speed_10m"]       | 0.0f;
  wxMax  = doc["daily"]["temperature_2m_max"][0] | wxTemp;
  wxMin  = doc["daily"]["temperature_2m_min"][0] | wxTemp;
  wxValid = true;
  return true;
}

const char* weatherWord(int c) {
  switch (c) {
    case 0:  return "Clear";
    case 1:  return "Mostly clear";
    case 2:  return "Part cloudy";
    case 3:  return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Frz drizzle";
    case 61: case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: case 73: case 75: case 77: return "Snow";
    case 80: case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Storm + hail";
    default: return "--";
  }
}

void drawWeatherIcon(int x, int y, int c) {
  if (c == 0 || c == 1) {
    display.fillCircle(x + 10, y + 10, 5, SSD1306_WHITE);
    for (int a = 0; a < 8; a++) {
      float r = a * PI / 4;
      display.drawLine(x + 10 + cos(r) * 8, y + 10 + sin(r) * 8,
                       x + 10 + cos(r) * 11, y + 10 + sin(r) * 11, SSD1306_WHITE);
    }
    return;
  }
  display.fillCircle(x + 7,  y + 10, 5, SSD1306_WHITE);
  display.fillCircle(x + 14, y + 10, 6, SSD1306_WHITE);
  display.fillRect(x + 4, y + 11, 14, 5, SSD1306_WHITE);
  if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82))
    for (int i = 0; i < 3; i++) display.drawLine(x + 5 + i * 5, y + 18, x + 3 + i * 5, y + 23, SSD1306_WHITE);
  if (c >= 71 && c <= 77)
    for (int i = 0; i < 3; i++) display.fillCircle(x + 6 + i * 5, y + 20, 1, SSD1306_WHITE);
  if (c >= 95) {
    display.drawLine(x + 11, y + 17, x + 7, y + 22, SSD1306_WHITE);
    display.drawLine(x + 7,  y + 22, x + 12, y + 21, SSD1306_WHITE);
    display.drawLine(x + 12, y + 21, x + 8, y + 26, SSD1306_WHITE);
  }
}

bool fetchCrypto() {
  JsonDocument doc;
  const char* url = "https://api.coingecko.com/api/v3/simple/price"
                    "?ids=bitcoin,ethereum,solana&vs_currencies=usd&include_24hr_change=true";
  if (!fetchJson(url, doc)) return false;
  bool any = false;
  for (int i = 0; i < 3; i++) {
    JsonObject o = doc[coins[i].id];
    if (o.isNull()) { coins[i].ok = false; continue; }
    coins[i].usd = o["usd"] | 0.0f;
    coins[i].chg = o["usd_24h_change"] | 0.0f;
    coins[i].ok  = true;
    any = true;
  }
  return any;
}

String money(float v) {
  if (v >= 1000) return String((long)(v + 0.5f));
  if (v >= 1)    return String(v, 2);
  return String(v, 4);
}

/* ============================ SCREENS ======================================= */
void drawHeader(const char* title) {
  display.fillRect(0, 0, SCREEN_W, 11, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(2, 2);
  display.print(title);
  if (WiFi.status() == WL_CONNECTED) display.fillCircle(122, 5, 2, SSD1306_BLACK);
  else                               display.drawCircle(122, 5, 2, SSD1306_BLACK);
  display.setTextColor(SSD1306_WHITE);
}

void drawClock() {
  struct tm t;
  if (!getLocalTime(&t, 50)) {
    drawHeader("Clock");
    centerText("Waiting for time", 30, 1);
    return;
  }
  char hhmm[8], ss[4], date[24], wd[12];
  strftime(hhmm, sizeof(hhmm), "%H:%M", &t);
  strftime(ss,   sizeof(ss),   "%S",    &t);
  strftime(date, sizeof(date), "%d %b %Y", &t);
  strftime(wd,   sizeof(wd),   "%A",    &t);

  display.setTextSize(3);
  display.setCursor(8, 12);
  display.print(hhmm);
  display.setTextSize(1);
  display.setCursor(98, 28);
  display.print(ss);
  display.drawFastHLine(0, 42, SCREEN_W, SSD1306_WHITE);
  centerText(String(wd), 46, 1);
  centerText(String(date), 56, 1);
}

void drawWeather() {
  drawHeader("Weather");
  if (!wxValid) {
    centerText(WiFi.status() == WL_CONNECTED ? "Loading..." : "No Wi-Fi", 32, 1);
    return;
  }
  drawWeatherIcon(2, 14, wxCode);
  display.setTextSize(2);
  display.setCursor(34, 16);
  display.print(String(wxTemp, 1));
  display.setTextSize(1);
  display.print(UNITS_METRIC ? "C" : "F");
  display.setTextSize(1);
  display.setCursor(34, 34);
  display.print(weatherWord(wxCode));
  display.drawFastHLine(0, 44, SCREEN_W, SSD1306_WHITE);
  display.setCursor(2, 47);
  display.print(wxCity.substring(0, 12));
  display.setCursor(2, 56);
  display.print(String((int)round(wxMin)) + "/" + String((int)round(wxMax)) + "  " +
                String(wxHum) + "%  " + String((int)round(wxWind)) + (UNITS_METRIC ? "km/h" : "mph"));
}

void drawCrypto() {
  drawHeader("Crypto  USD");
  if (!coins[0].ok && !coins[1].ok && !coins[2].ok) {
    centerText(WiFi.status() == WL_CONNECTED ? "Loading..." : "No Wi-Fi", 32, 1);
    return;
  }
  int y = 15;
  for (int i = 0; i < 3; i++) {
    display.setTextSize(1);
    display.setCursor(2, y);
    display.print(coins[i].tag);
    if (!coins[i].ok) { display.setCursor(30, y); display.print("--"); y += 17; continue; }
    display.setCursor(28, y);
    display.print(money(coins[i].usd));

    String ch = (coins[i].chg >= 0 ? "+" : "") + String(coins[i].chg, 1) + "%";
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(ch, 0, y, &x1, &y1, &w, &h);
    display.setCursor(SCREEN_W - w - 2, y);
    display.print(ch);
    if (coins[i].chg >= 0) display.fillTriangle(SCREEN_W-w-10, y+6, SCREEN_W-w-6, y+6, SCREEN_W-w-8, y+1, SSD1306_WHITE);
    else                   display.fillTriangle(SCREEN_W-w-10, y+1, SCREEN_W-w-6, y+1, SCREEN_W-w-8, y+6, SSD1306_WHITE);

    y += 17;
    if (i < 2) display.drawFastHLine(0, y - 5, SCREEN_W, SSD1306_WHITE);
  }
}

void drawNotes() {
  drawHeader(noteTitle.substring(0, 18).c_str());
  const uint8_t rows = 6;
  uint16_t pages = (noteLines.size() + rows - 1) / rows;
  if (pages == 0) pages = 1;
  if (notePage >= pages) notePage = 0;
  for (uint8_t r = 0; r < rows; r++) {
    uint16_t idx = notePage * rows + r;
    if (idx >= noteLines.size()) break;
    display.setTextSize(1);
    display.setCursor(0, 14 + r * 8);
    display.print(noteLines[idx]);
  }
  if (pages > 1)
    for (uint16_t p = 0; p < pages && p < 8; p++) {
      if (p == notePage) display.fillCircle(125, 16 + p * 6, 1, SSD1306_WHITE);
      else               display.drawPixel(125, 16 + p * 6, SSD1306_WHITE);
    }
}

void render() {
  if (!haveDisplay) return;
  display.clearDisplay();
  switch (screen) {
    case SCR_CLOCK:   drawClock();   break;
    case SCR_WEATHER: drawWeather(); break;
    case SCR_CRYPTO:  drawCrypto();  break;
    case SCR_NOTES:   drawNotes();   break;
  }
  if (apMode) {
    display.fillRect(0, 56, SCREEN_W, 8, SSD1306_BLACK);
    display.setTextSize(1);
    display.setCursor(0, 56);
    display.print("AP " AP_SSID " 192.168.4.1");
  }
  drawHoldBar();
  display.display();
}

/* ============================ WEB PAGE ====================================== */
String esc(const String& s) {
  String o = s;
  o.replace("&", "&amp;");
  o.replace("<", "&lt;");
  return o;
}

const char CSS[] PROGMEM = R"css(
:root{--ink:#e8e4d9;--dim:#8d8778;--bg:#14161a;--panel:#1d2026;--glow:#ffb000;--line:#2b2f37}
*{box-sizing:border-box}
body{margin:0;padding:28px 18px 60px;background:var(--bg);color:var(--ink);
     font:16px/1.55 "Helvetica Neue",Helvetica,Arial,sans-serif}
main{max-width:38rem;margin:0 auto}
h1{font-size:1.45rem;font-weight:600;letter-spacing:-.01em;margin:0 0 4px}
p.sub{margin:0 0 26px;color:var(--dim);font-size:.9rem}
.screen{background:#0a0c0e;border:1px solid var(--line);border-radius:10px;padding:14px 16px;margin-bottom:22px}
.screen pre{margin:0;font:13px/1.45 ui-monospace,Menlo,Consolas,monospace;color:var(--glow);
            white-space:pre-wrap;word-break:break-word;min-height:6.5em}
label{display:block;font-size:.85rem;color:var(--dim);margin:16px 0 6px}
input,textarea{width:100%;background:var(--panel);border:1px solid var(--line);border-radius:8px;
               color:var(--ink);padding:11px 12px;font:15px/1.5 inherit}
textarea{min-height:170px;resize:vertical;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:14px}
input:focus,textarea:focus{outline:2px solid var(--glow);outline-offset:1px;border-color:transparent}
button{margin-top:18px;background:var(--glow);color:#14161a;border:0;border-radius:8px;
       padding:12px 20px;font-size:15px;font-weight:600;cursor:pointer}
button:focus-visible{outline:2px solid var(--ink);outline-offset:2px}
section{border-top:1px solid var(--line);margin-top:34px;padding-top:8px}
h2{font-size:1rem;font-weight:600;margin:14px 0 0}
.note{color:var(--dim);font-size:.85rem;margin:6px 0 0}
.stat{color:var(--dim);font-size:.85rem;margin:0}
.stat b{color:var(--ink);font-weight:500}
.warn{color:var(--glow)}
)css";

String liveNotePreview() {
  String out;
  uint8_t shown = 0;
  for (size_t i = 0; i < noteLines.size() && shown < 6; i++, shown++) out += esc(noteLines[i]) + "\n";
  if (noteLines.size() > 6) out += "...";
  return out;
}

void handleRoot() {
  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  struct tm t; char now[24] = "not synced";
  if (getLocalTime(&t, 20)) strftime(now, sizeof(now), "%H:%M  %d %b", &t);

  String h; h.reserve(4500);
  h += F("<!doctype html><html lang=en><head><meta charset=utf-8>"
         "<meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>Info Terminal</title><style>");
  h += FPSTR(CSS);
  h += F("</style></head><body><main>");
  h += F("<h1>Hello, Kitty</h1><p class=sub>Write what the little screen should say.</p>");
  h += F("<div class=screen><pre>");
  h += liveNotePreview();
  h += F("</pre></div>");

  h += F("<form method=POST action='/save'>");
  h += F("<label for=title>Screen title</label><input id=title name=title maxlength=18 value='");
  h += esc(noteTitle);
  h += F("'><label for=text>Text (about 21 characters per line, 6 lines per page)</label>"
         "<textarea id=text name=text maxlength=1400>");
  h += esc(noteText);
  h += F("</textarea><button type=submit>Send to screen</button></form>");

  h += F("<section><h2>Wi-Fi</h2><p class=note>Saving new details restarts the device.</p>"
         "<form method=POST action='/wifi'>"
         "<label for=ssid>Network name</label><input id=ssid name=ssid value='");
  h += esc(apMode ? String("") : wifiSsid);
  h += F("'><label for=pass>Password</label><input id=pass name=pass type=password>"
         "<button type=submit>Save and restart</button></form></section>");

  h += F("<section><h2>Status</h2><p class=stat>Address <b>");
  h += ip;
  h += F("</b> &nbsp; Clock <b>");
  h += now;
  h += F("</b> &nbsp; Mode <b>");
  h += mode == MODE_EYES ? F("Eyes") : F("Terminal");
  h += F("</b><br>Screen <b>");
  if (haveDisplay) { h += "OK on GPIO" + String(usedSda) + "/" + String(usedScl) + " at 0x" + String(oledAddr, HEX); }
  else             { h += F("<span class=warn>not detected</span>"); }
  h += F("</b><br>Weather <b>");
  h += wxValid ? (wxCity + " " + String(wxTemp, 1) + (UNITS_METRIC ? "C" : "F")) : String("not loaded");
  h += F("</b><br>BTC <b>");
  h += coins[0].ok ? money(coins[0].usd) : String("--");
  h += F("</b> ETH <b>");
  h += coins[1].ok ? money(coins[1].usd) : String("--");
  h += F("</b> SOL <b>");
  h += coins[2].ok ? money(coins[2].usd) : String("--");
  h += F("</b></p></section></main></body></html>");

  server.send(200, "text/html", h);
}

void handleSave() {
  if (server.hasArg("text"))  noteText  = server.arg("text");
  if (server.hasArg("title")) noteTitle = server.arg("title");
  if (noteTitle.length() == 0) noteTitle = "Notes";
  rebuildNoteLines();
  saveNote();
  if (mode != MODE_TERMINAL) setMode(MODE_TERMINAL);   // wake up to show the new note
  screen = SCR_NOTES;
  lastDraw = 0;
  beep(1800, 60);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleWifi() {
  String s = server.arg("ssid");
  String p = server.arg("pass");
  if (s.length() == 0) { server.sendHeader("Location", "/"); server.send(303); return; }
  saveWifi(s, p);
  server.send(200, "text/html",
              "<meta charset=utf-8><body style='background:#14161a;color:#e8e4d9;font-family:sans-serif;padding:40px'>"
              "<p>Saved. Restarting and joining " + esc(s) + ".</p></body>");
  delay(600);
  ESP.restart();
}

void handleNotFound() { server.sendHeader("Location", "/"); server.send(303); }

/* ============================ EYES ========================================== */
// Mochi's eyes, as a non-blocking state machine: eyeUpdate() is called from
// loop() and only ever does a little work, so nothing else has to wait.
//
// The eyes stay open for 10-15 s (random each time), then blink, sometimes
// twice. While open they rest on one spot for a second or few, then jump to
// another - left, right, up, down or at an angle - and keep coming back to
// looking straight ahead. While resting they make tiny 1 px movements.
// Blinks can also shift the gaze: close looking up and open looking down, etc.

enum EyeState : uint8_t { EYE_REST, EYE_MOVE, EYE_BLINK, EYE_PAUSE };

static uint8_t  frameBuf[SCREEN_W * SCREEN_H / 8];   // 1024 bytes, 1 bit per pixel
static int8_t   lookX = 0, lookY = 0;               // where the eyes look (offset in px)
static int8_t   fromX, fromY, toX, toY;             // current move / blink path
static int8_t   spotX, spotY;                       // resting spot (twitches go around it)
static EyeState eyeState = EYE_REST;
static uint16_t eyeStep, eyeSteps;                  // progress along a move or blink
static uint32_t eyeOpenEnd, eyeRestEnd, eyeNextTwitch, eyeNextStep;
static bool     restThenBlink = false;              // this rest ends with a blink
static bool     blinkMayDouble = false;
static bool     eyeDirty = true;                    // frame needs to be sent to the OLED

// Applies frame idx of animation a to frameBuf.
// Each frame is stored as PackBits-compressed XOR against the previous frame,
// so decode frames in order and clear frameBuf before frame 0.
void decodeFrame(const Anim &a, uint16_t idx) {
  const uint8_t *p   = a.data + pgm_read_word(&a.offsets[idx]);
  const uint8_t *end = a.data + pgm_read_word(&a.offsets[idx + 1]);
  uint16_t o = 0;
  while (p < end && o < sizeof(frameBuf)) {
    int8_t t = (int8_t)pgm_read_byte(p++);
    if (t >= 0) {                        // literal: the next t+1 bytes
      uint16_t n = t + 1;
      while (n-- && o < sizeof(frameBuf)) frameBuf[o++] ^= pgm_read_byte(p++);
    } else {                             // repeat: next byte, 1-t times
      uint16_t n = 1 - t;
      uint8_t v = pgm_read_byte(p++);
      while (n-- && o < sizeof(frameBuf)) frameBuf[o++] ^= v;
    }
  }
}

// Draws frameBuf shifted by (lookX, lookY).
void eyeDraw() {
  if (!haveDisplay) return;
  display.clearDisplay();
  display.drawBitmap(lookX, lookY, frameBuf, SCREEN_W, SCREEN_H, SSD1306_WHITE);
  drawHoldBar();
  display.display();
}

bool timeReached(uint32_t t) { return (int32_t)(millis() - t) >= 0; }

// True when the eyes look straight ahead (allowing for the tiny 1 px movements).
bool lookingAhead() {
  return abs(lookX) <= 1 && abs(lookY) <= 1;
}

// Picks a new spot to look at, different from the current one. People look
// sideways more often than up or down, and don't always look all the way over,
// so the direction is weighted and the distance varies.
void pickSpot(int8_t &x, int8_t &y) {
  if (!lookingAhead() && random(100) < CENTER_PCT) {
    x = 0;                                          // back to straight ahead
    y = 0;
    return;
  }
  //                          L   R  Up Down UpL UpR DnL DnR
  static const int8_t DX[] = {-1, 1,  0,  0,  -1,  1, -1,  1};
  static const int8_t DY[] = { 0, 0, -1,  1,  -1, -1,  1,  1};
  static const uint8_t W[] = { 3, 3,  2,  2,   1,  1,  1,  1};   // weights, sum 14
  do {
    long r = random(14);
    uint8_t d = 0;
    while (r >= W[d]) r -= W[d++];
    x = DX[d] * random(LOOK_X_PX / 2, LOOK_X_PX + 1);
    y = DY[d] * random(LOOK_Y_PX / 2, LOOK_Y_PX + 1);
  } while (abs(x - lookX) + abs(y - lookY) < 3);    // make sure it really moves
}

// Picks where the eyes look when they open from the next blink.
void blinkTarget(int8_t &x, int8_t &y) {
  x = lookX;                                        // plain blink: keep looking there
  y = lookY;
  if (random(100) >= BLINK_SHIFT_PCT) return;
  long r = random(100);
  if (lookingAhead()) {
    pickSpot(x, y);                                 // ahead -> somewhere new
  } else if (r < 45) {
    x = 0;                                          // e.g. left -> straight ahead
    y = 0;
  } else if (r < 75) {
    x = -lookX;                                     // opposite: up -> down, left -> right
    y = -lookY;
  } else {
    pickSpot(x, y);                                 // somewhere new
  }
}

void scheduleTwitch() {
  eyeNextTwitch = millis() + (TWITCH_MIN_MS ? random(TWITCH_MIN_MS, TWITCH_MAX_MS + 1) : 0);
}

// Rests on the current spot. If there is not enough open time left for
// another spot, this rest runs to the end of the open period and then blinks.
void eyeStartRest() {
  uint32_t now  = millis();
  uint32_t left = (int32_t)(eyeOpenEnd - now) > 0 ? eyeOpenEnd - now : 0;
  uint32_t hold = random(REST_MIN_MS, REST_MAX_MS + 1);
  restThenBlink = hold + REST_MIN_MS > left;
  eyeRestEnd    = now + (restThenBlink ? left : hold);
  spotX = lookX;
  spotY = lookY;
  scheduleTwitch();
  eyeState = EYE_REST;
}

// Starts a new open period: eyes open for 10-15 s before the next blink.
void eyeStartOpen() {
  eyeOpenEnd = millis() + random(OPEN_MIN_MS, OPEN_MAX_MS + 1);
  eyeStartRest();
}

// Moves the eyes in a straight line to (x, y), one pixel step at a time.
void eyeStartMove(int8_t x, int8_t y) {
  fromX = lookX; fromY = lookY; toX = x; toY = y;
  eyeSteps = max(abs(x - fromX), abs(y - fromY));
  if (eyeSteps == 0) { eyeStartRest(); return; }
  eyeStep = 0;
  eyeNextStep = millis();
  eyeState = EYE_MOVE;
}

// Blinks while the eyes move from where they look now to (x, y), so they can
// close looking one way and open looking another. Frame 0 is fully open, 1-4
// close, 5 is shut, 6-10 open again. Frame 10 is the open eye too, so the eyes
// rest on it after the blink.
void eyeStartBlink(int8_t x, int8_t y, bool mayDouble) {
  memset(frameBuf, 0, sizeof(frameBuf));
  decodeFrame(ANIMS[ANIM_EYE], 0);
  fromX = lookX; fromY = lookY; toX = x; toY = y;
  blinkMayDouble = mayDouble;
  eyeStep = 0;
  eyeNextStep = millis();
  eyeState = EYE_BLINK;
}

// Resets the eyes: open, looking straight ahead.
void eyeBegin() {
  memset(frameBuf, 0, sizeof(frameBuf));
  decodeFrame(ANIMS[ANIM_EYE], 0);
  lookX = lookY = 0;
  eyeStartOpen();
  eyeDirty = true;
}

// A tap in EYES mode: Mochi looks at you and blinks.
void eyeTap() {
  if (eyeState == EYE_BLINK || eyeState == EYE_PAUSE) return;
  beep(2600, 25);
  eyeStartBlink(0, 0, true);
}

void eyeUpdate() {
  const Anim &a = ANIMS[ANIM_EYE];

  switch (eyeState) {
    case EYE_REST:
      if (timeReached(eyeRestEnd)) {
        int8_t x, y;
        if (restThenBlink) { blinkTarget(x, y); eyeStartBlink(x, y, true); }
        else               { pickSpot(x, y);    eyeStartMove(x, y); }
      } else if (TWITCH_MIN_MS && timeReached(eyeNextTwitch)) {
        lookX = constrain(spotX + random(-1, 2), -LOOK_X_PX, LOOK_X_PX);
        lookY = constrain(spotY + random(-1, 2), -LOOK_Y_PX, LOOK_Y_PX);
        eyeDirty = true;
        scheduleTwitch();
      }
      break;

    case EYE_MOVE:
      if (timeReached(eyeNextStep)) {
        eyeStep++;
        lookX = fromX + (toX - fromX) * (int)eyeStep / (int)eyeSteps;
        lookY = fromY + (toY - fromY) * (int)eyeStep / (int)eyeSteps;
        eyeDirty = true;
        eyeNextStep = millis() + LOOK_STEP_MS;
        if (eyeStep >= eyeSteps) eyeStartRest();
      }
      break;

    case EYE_BLINK:
      if (timeReached(eyeNextStep)) {
        if (eyeStep >= a.frames - 1) {               // last frame has been shown long enough
          if (blinkMayDouble && random(100) < DOUBLE_BLINK_PCT) {
            eyeNextStep = millis() + DOUBLE_BLINK_GAP;
            eyeState = EYE_PAUSE;
          } else {
            eyeStartOpen();
          }
          break;
        }
        eyeStep++;
        decodeFrame(a, eyeStep);
        lookX = fromX + (toX - fromX) * (int)eyeStep / (a.frames - 1);
        lookY = fromY + (toY - fromY) * (int)eyeStep / (a.frames - 1);
        eyeDirty = true;
        eyeNextStep = millis() + BLINK_FRAME_MS;
      }
      break;

    case EYE_PAUSE:                                  // gap before the second blink
      if (timeReached(eyeNextStep)) eyeStartBlink(lookX, lookY, false);
      break;
  }

  if (eyeDirty) { eyeDirty = false; eyeDraw(); }
}

/* ============================ MODES ========================================= */
void setMode(Mode m) {
  mode = m;
  if (m == MODE_EYES) {
    eyeBegin();
    beep(1200, 120);                     // lower tone: going to sleep... er, eyes
  } else {
    screen = SCR_CLOCK;
    notePage = 0;
    lastNoteFlip = millis();
    lastDraw = 0;                        // draw the clock right away
    lastPoll = 0;                        // then refresh anything that went stale
    beep(2000, 120);
  }
  LOG("Mode -> %s\n", m == MODE_EYES ? "EYES" : "TERMINAL");
}

void requestRedraw() {
  if (mode == MODE_EYES) eyeDirty = true;
  else                   lastDraw = 0;
}

/* ============================ TOUCH ========================================= */
bool touchPrev = false, switchFired = false, refreshArmed = false;
unsigned long touchStart = 0;

void refreshAll(bool force) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (force || millis() - lastWeather > WEATHER_REFRESH_MS || !wxValid)
    if (fetchWeather()) lastWeather = millis();
  if (force || millis() - lastCrypto > CRYPTO_REFRESH_MS || !coins[0].ok)
    if (fetchCrypto()) lastCrypto = millis();
}

// True while the pad is held long enough to show the progress bar.
bool holdBarVisible() {
  return touchPrev && !switchFired && millis() - touchStart >= HOLD_HINT_MS;
}

// A bar along the bottom edge that fills up while the pad is held; when it is
// full the mode switches. The tick marks where releasing would refresh data.
void drawHoldBar() {
  if (!holdBarVisible()) return;
  unsigned long held = min((unsigned long)(millis() - touchStart), (unsigned long)HOLD_SWITCH_MS);
  int w = (int)(held * SCREEN_W / HOLD_SWITCH_MS);
  display.fillRect(0, SCREEN_H - 5, SCREEN_W, 5, SSD1306_BLACK);
  display.drawRect(0, SCREEN_H - 4, SCREEN_W, 4, SSD1306_WHITE);
  display.fillRect(0, SCREEN_H - 4, w, 4, SSD1306_WHITE);
  if (mode == MODE_TERMINAL) {
    int tick = HOLD_REFRESH_MS * SCREEN_W / HOLD_SWITCH_MS;
    display.drawFastVLine(tick, SCREEN_H - 6, 2, SSD1306_WHITE);
  }
}

// Keeps the progress bar animating while held, and wipes it after release.
void serviceHoldBar() {
  static bool shown = false;
  static unsigned long lastBar = 0;
  bool vis = holdBarVisible();
  if ((vis && millis() - lastBar > 50) || (!vis && shown)) {
    lastBar = millis();
    requestRedraw();
  }
  shown = vis;
}

void serviceTouch() {
  bool now = digitalRead(PIN_TOUCH) == HIGH;
  if (now && !touchPrev) { touchStart = millis(); switchFired = false; refreshArmed = false; }
  unsigned long held = millis() - touchStart;

  if (now && !switchFired) {
    // TERMINAL: past 1.5 s, a short beep says "let go now to refresh"
    if (mode == MODE_TERMINAL && !refreshArmed && held > HOLD_REFRESH_MS) {
      refreshArmed = true;
      beep(900, 60);
    }
    // both modes: 5 s switches between EYES and TERMINAL, without waiting for release
    if (held > HOLD_SWITCH_MS) {
      switchFired = true;
      setMode(mode == MODE_EYES ? MODE_TERMINAL : MODE_EYES);
    }
  }

  if (!now && touchPrev && !switchFired && held > 40) {
    if (mode == MODE_EYES) {
      eyeTap();
    } else if (refreshArmed) {
      beep(900, 120);
      if (haveDisplay) { display.clearDisplay(); centerText("Refreshing", 28, 1); display.display(); }
      refreshAll(true);
      lastDraw = 0;
    } else {
      screen = (screen + 1) % SCR_COUNT;
      notePage = 0;
      lastNoteFlip = millis();
      beep(2200, 35);
      lastDraw = 0;
      LOG("Screen -> %d\n", screen);
    }
  }
  touchPrev = now;
}

/* ============================ SETUP / LOOP ================================== */
void setup() {
#if SERIAL_SAFE
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== Mochi Terminal booting ===");
#endif

  pinMode(PIN_TOUCH, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  beepBlocking(1500, 80);                 // 1 beep = alive

  haveDisplay = startDisplay();
  if (haveDisplay) {
    beepBlocking(1900, 80);               // 2 beeps = screen OK
    centerText("Hello, Kitty", 26, 1);
    display.display();
    delay(700);
  } else {
    LOGLN("No OLED. Continuing without it - Wi-Fi and web page still work.");
    beepBlocking(400, 400);               // long low beep = no screen
  }

  loadConfig();

  if (!connectWifi()) startAP();
  else beepBlocking(2400, 80);            // high beep = online

  configTzTime(TZ_STRING, "pool.ntp.org", "time.google.com", "time.nist.gov");
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);

  server.on("/",     HTTP_GET,  handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.onNotFound(handleNotFound);
  server.begin();
  LOGLN("Web server running.");

  if (haveDisplay) {
    display.clearDisplay();
    centerText(apMode ? "Hotspot mode" : "Online", 20, 1);
    centerText(apMode ? "192.168.4.1" : WiFi.localIP().toString(), 34, 1);
    display.display();
    delay(1500);
  }
  refreshAll(true);                       // have data ready for the first switch

  mode = MODE_EYES;
  eyeBegin();
}

void loop() {
  server.handleClient();
  serviceTouch();
  serviceBeep();
  serviceHoldBar();

  if (mode == MODE_EYES) {
    eyeUpdate();
  } else {
    if (millis() - lastDraw > 400) { render(); lastDraw = millis(); }

    if (screen == SCR_NOTES && millis() - lastNoteFlip > NOTE_PAGE_MS) {
      uint16_t pages = (noteLines.size() + 5) / 6;
      if (pages > 1) { notePage = (notePage + 1) % pages; lastDraw = 0; }
      lastNoteFlip = millis();
    }

    // network fetches can block for a few seconds, so only in TERMINAL mode
    if (lastPoll == 0 || millis() - lastPoll > 5000) { lastPoll = millis(); refreshAll(false); }
  }

  static unsigned long lastRetry = 0;
  if (!apMode && WiFi.status() != WL_CONNECTED && millis() - lastRetry > 30000) {
    lastRetry = millis();
    LOGLN("Wi-Fi dropped, reconnecting.");
    WiFi.reconnect();
  }

  // heartbeat over serial so you can tell it is alive with a dead screen
  static unsigned long lastBeat = 0;
  if (millis() - lastBeat > 10000) {
    lastBeat = millis();
    LOG("alive  mode=%s  screen=%d  oled=%d  wifi=%d  heap=%u\n",
        mode == MODE_EYES ? "eyes" : "terminal",
        screen, haveDisplay, WiFi.status() == WL_CONNECTED, ESP.getFreeHeap());
  }
}
