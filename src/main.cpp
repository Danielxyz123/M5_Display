// =============================================================================
// M5Stack-Display für PV / Netz / Venus-Speicher
// =============================================================================
// Seiten (langer Druck auf B blättert):
//   1. Übersicht: PV, Netz, Akku, Ladestand, Autarkie
//      Rahmenfarbe Akku: grün = lädt, orange = entlädt, grau = Stand-by/voll,
//                        rot = Störung am Venus (kein Sollwert, Modbus, keine Daten)
//      Grauer Wert = Daten nicht aktuell
//   2. Verlauf der letzten 60 min (Netz, PV, Akku)
//   3. Diagnose (Bridge, Regelung, Venus-Controller, Display)
//
// Tasten:  kurz A = Lampe an/aus   kurz B = heller   kurz C = dunkler
//          lang A = Testmodus      lang B = nächste Seite   lang C = Display dimmen
// =============================================================================

#include <M5Stack.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <cmath>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "include/secrets.h fehlt: include/secrets.h.example kopieren und anpassen"
#endif

// Debugging toggle
#define DEBUG 1
#if DEBUG
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTF(x, ...) Serial.printf(x, ##__VA_ARGS__)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTF(x, ...)
#endif

// === KONFIGURATION ===
#define WATCHDOG_TIMEOUT_S   30        // muss länger sein als ein blockierender connect()
#define UPDATE_INTERVAL      500
#define WIFI_RETRY_MS        15000
#define WIFI_MAX_OFFLINE_MS  300000UL  // so lange ohne WLAN → Neustart
#define MQTT_BACKOFF_MIN     1000
#define MQTT_BACKOFF_MAX     30000
#define MQTT_PACKET_SIZE     2048      // Venus-JSON ist ca. 1 KB
#define LONG_PRESS_MS        800
#define FEEDBACK_MS          800
#define BRIGHT_NORMAL        100
#define BRIGHT_DIM           10

// Wann gelten Werte als veraltet?
#define BRIDGE_MAX_AGE_MS    10000     // Lebenszeichen der Bridge
#define VENUS_MAX_AGE_MS     15000     // Venus-Controller (JSON, Lifebit)
#define SOLL_MAX_AGE_MS      10000     // Regelskript sendet jede Sekunde
#define AKKU_MAX_AGE_MS      60000     // Shelly-Spiegel sendet nur bei Änderung
#define SLOW_MAX_AGE_MS      1800000UL // Ladestand/Autarkie ändern sich selten
#define STANDBY_W            15        // darunter gilt der Akku als ruhend

// Verlauf: 120 Punkte à 30 s = 60 min
#define HIST_POINTS          120
#define HIST_INTERVAL_MS     30000
#define HIST_INVALID         INT16_MIN

// === TOPICS ===
#define T_GRID      "PV/grid_powerFast"
#define T_GEN       "PV/generationPower"
#define T_HB        "PV/heartbeat"
#define T_GVALID    "PV/gridValid"
#define T_INV       "PV/inverterOnline"
#define T_RSSI      "PV/rssi"
#define T_MQC       "PV/mqttConnects"
#define T_MBERR     "PV/modbusErr"
#define T_BUPTIME   "PV/uptime"
#define T_AKKU      "VenusData/PowerShelly"
#define T_SOC       "VenusData/Ladezustand"
#define T_AUTARKIE  "VenusData/Autarkie_heute"
#define T_VENUS     "Venus-control"
#define T_SOLL      "Ven_Neu/powerNeu"
#define T_ONLINE    "M5Display/online"

// === LAMPE STEUERUNG ÜBER TASTEN ===
#define LAMP_TOPIC_ONOFF        "lampe/wohnzimmer/set"            // ioBroker: ON / OFF (retained)
#define LAMP_TOPIC_BRIGHTNESS   "lampe/wohnzimmer/brightness_set" // ioBroker: +10 oder -10

// === DATEN ===
struct Werte {
  float    grid = 0, gen = 0, akku = 0, soll = 0;
  int      soc = 0, autarkie = 0;
  uint32_t tGrid = 0, tGen = 0, tAkku = 0, tSoc = 0, tAutarkie = 0, tHb = 0, tVenus = 0, tSoll = 0;
  uint32_t hb = 0;
  bool     gridValid = false, invOnline = false;
  int      rssi = 0;
  uint32_t mqttConnects = 0, modbusErr = 0, bridgeUptime = 0;
  // Venus-Controller
  char     venusText[96] = "";
  bool     venusModbusOk = true;
  uint32_t venusLifebit = 0, tVenusLifebit = 0, venusUptime = 0;
  int      venusSet = 0, venusAc = 0;
  char     venusReset[20] = "";
} w;

struct FieldState {                    // zuletzt gezeichneter Zustand eines Feldes
  char     text[16];
  uint16_t border;
  uint16_t textColor;
};
FieldState fsPV = {}, fsNetz = {}, fsAkku = {}, fsSoc = {}, fsAut = {};

int16_t  histGrid[HIST_POINTS], histGen[HIST_POINTS], histAkku[HIST_POINTS];
uint16_t histIdx = 0;
uint32_t histLast = 0;

WiFiClient   espClient;
PubSubClient mqttClient(espClient);

uint8_t  page = 0;                     // 0 = Übersicht, 1 = Verlauf, 2 = Diagnose
bool     forceRedraw = true;
bool     dimmed = false;
bool     testMode = false;
bool     lampOn = false;
uint32_t feedbackUntil = 0;
uint32_t updateTimeOld = 0;
char     lastFault[64] = "";
uint32_t lastFaultMillis = 0;
bool     faultActive = false;

// Function prototypes
void initWifi();
void handleWifi();
void handleMqtt();
void callbackMqttReceive(char* topic, byte* payload, unsigned int length);
void handleButtons();
void handleTestMode();
void sampleHistory();
void drawPage();
void drawOverview(bool force);
void drawHistory();
void drawDiagnose();
void drawFieldShadow(int x, int y, int w, int h, int radius, uint16_t shadowColor);
void drawFieldBorder(int x, int y, int w, int h, int radius, uint16_t borderColor);
void fillGradientRoundRect(int x, int y, int w, int h, int radius, uint16_t colorStart, uint16_t colorEnd);

// =============================================================================
// Hilfsfunktionen
// =============================================================================
static bool frisch(uint32_t t, uint32_t maxAge) {
  return t != 0 && (millis() - t) < maxAge;
}

static uint32_t alterS(uint32_t t) {
  return t == 0 ? 0 : (millis() - t) / 1000;
}

static bool bridgeOk() {
  return frisch(w.tHb, BRIDGE_MAX_AGE_MS) && w.gridValid && frisch(w.tGrid, BRIDGE_MAX_AGE_MS);
}

enum VenusZustand { VZ_UNBEKANNT, VZ_STOERUNG, VZ_RUHE, VZ_LAEDT, VZ_ENTLAEDT };

static VenusZustand venusZustand(const char** grund) {
  static const char* dummy;
  if (!grund) grund = &dummy;
  *grund = "";
  if (w.tVenus == 0) {
    if (millis() < 20000) return VZ_UNBEKANNT;          // nach dem Start noch keine Daten
    *grund = "Venus-Controller sendet nicht";
    return VZ_STOERUNG;
  }
  if (!frisch(w.tVenus, VENUS_MAX_AGE_MS))         { *grund = "Venus-Controller sendet nicht"; return VZ_STOERUNG; }
  if (!frisch(w.tVenusLifebit, VENUS_MAX_AGE_MS))  { *grund = "Venus-Lifebit steht";           return VZ_STOERUNG; }
  if (!w.venusModbusOk)                            { *grund = "Venus: Modbus gestoert";        return VZ_STOERUNG; }
  if (strstr(w.venusText, "kein Sollwert"))        { *grund = "Venus: kein Sollwert";          return VZ_STOERUNG; }
  if (strstr(w.venusText, "storing"))              { *grund = "Venus: Stoerung P1";            return VZ_STOERUNG; }
  if (strstr(w.venusText, "Stand-by") || strstr(w.venusText, "Te veel") ||
      strstr(w.venusText, "Te weinig") || fabsf(w.akku) < STANDBY_W) {
    return VZ_RUHE;
  }
  return (w.akku > 0) ? VZ_LAEDT : VZ_ENTLAEDT;          // Shelly: + = Laden, - = Entladen
}

// Venus-Controller-Text in kurzes Deutsch übersetzen (für die Diagnose-Seite)
static const char* venusTextDe() {
  if (strstr(w.venusText, "opladen met"))   return "laedt";
  if (strstr(w.venusText, "ontladen met"))  return "entlaedt";
  if (strstr(w.venusText, "Te veel"))       return "Speicher voll";
  if (strstr(w.venusText, "Te weinig"))     return "Speicher leer";
  if (strstr(w.venusText, "kein Sollwert")) return "kein Sollwert!";
  if (strstr(w.venusText, "Stand-by"))      return "Stand-by";
  if (strstr(w.venusText, "minder dan"))    return "unter Mindestleistung";
  return w.venusText[0] ? w.venusText : "-";
}

// Störungen merken (für die Diagnose-Seite)
static void pruefeStoerung() {
  const char* grund = "";
  bool stoerung = false;
  char text[64] = "";
  if (w.tHb != 0 && !bridgeOk()) {
    stoerung = true;
    snprintf(text, sizeof(text), "Zaehlerwert veraltet");
  }
  if (venusZustand(&grund) == VZ_STOERUNG) {
    stoerung = true;
    snprintf(text, sizeof(text), "%s", grund);
  }
  if (stoerung && !faultActive) {
    strncpy(lastFault, text, sizeof(lastFault) - 1);
    lastFaultMillis = millis();
    DEBUG_PRINTF("Stoerung: %s\n", lastFault);
  }
  faultActive = stoerung;
}

// =============================================================================
// Setup / Loop
// =============================================================================
void setup() {
  M5.begin();
  M5.Power.begin();
  Serial.begin(115200);
  M5.Lcd.setBrightness(BRIGHT_NORMAL);
  M5.Lcd.fillScreen(TFT_NAVY);

  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  for (int i = 0; i < HIST_POINTS; i++) {
    histGrid[i] = histGen[i] = histAkku[i] = HIST_INVALID;
  }

  initWifi();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setBufferSize(MQTT_PACKET_SIZE);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(5);
  mqttClient.setCallback(callbackMqttReceive);

  forceRedraw = true;
  DEBUG_PRINT(F("Setup complete.\n"));
  DEBUG_PRINT(F("kurz: A=Lampe B=heller C=dunkler | lang: A=Test B=Seite C=Dimmen\n"));
}

void loop() {
  esp_task_wdt_reset();
  M5.update();

  handleButtons();
  handleWifi();
  handleMqtt();
  handleTestMode();
  sampleHistory();
  pruefeStoerung();

  // Rückmeldung zum Tastendruck nach kurzer Zeit wieder entfernen (einmalig neu zeichnen)
  if (feedbackUntil != 0 && millis() > feedbackUntil) {
    feedbackUntil = 0;
    if (page == 0) {
      fsPV.text[0] = '\0';                                  // PV-Feld neu zeichnen erzwingen
    }
  }

  if (millis() - updateTimeOld > UPDATE_INTERVAL) {
    updateTimeOld = millis();
    drawPage();
  }
  yield();
}

// =============================================================================
// WLAN / MQTT (nicht blockierend)
// =============================================================================
void initWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  DEBUG_PRINT(F("WiFi verbinden...\n"));
}

void handleWifi() {
  static uint32_t offlineSince = 0;
  static uint32_t lastTry = 0;
  const uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (offlineSince != 0) {
      DEBUG_PRINTF("WiFi OK: %s\n", WiFi.localIP().toString().c_str());
      forceRedraw = true;
    }
    offlineSince = 0;
    return;
  }
  if (offlineSince == 0) { offlineSince = now; lastTry = now; }
  if (now - lastTry >= WIFI_RETRY_MS) {
    lastTry = now;
    DEBUG_PRINT(F("WiFi neuer Versuch\n"));
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
  if (now - offlineSince >= WIFI_MAX_OFFLINE_MS) {
    DEBUG_PRINT(F("WiFi zu lange weg, Neustart\n"));
    ESP.restart();
  }
}

void handleMqtt() {
  static uint32_t lastTry = 0;
  static uint32_t backoff = MQTT_BACKOFF_MIN;

  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqttClient.connected()) {
    if (lastTry != 0 && millis() - lastTry < backoff) return;   // kein delay() mehr
    lastTry = millis();
    String clientId = "M5Stack-" + WiFi.macAddress();
    DEBUG_PRINT(F("MQTT verbinden...\n"));
    if (mqttClient.connect(clientId.c_str(), T_ONLINE, 1, true, "0")) {
      mqttClient.publish(T_ONLINE, "1", true);
      const char* topics[] = {
        T_GRID, T_GEN, T_HB, T_GVALID, T_INV, T_RSSI, T_MQC, T_MBERR, T_BUPTIME,
        T_AKKU, T_SOC, T_AUTARKIE, T_VENUS, T_SOLL, LAMP_TOPIC_ONOFF
      };
      for (auto t : topics) mqttClient.subscribe(t);
      backoff = MQTT_BACKOFF_MIN;
      DEBUG_PRINT(F("MQTT verbunden\n"));
    } else {
      DEBUG_PRINTF("MQTT Fehler rc=%d, naechster Versuch in %lu ms\n", mqttClient.state(), (unsigned long)backoff);
      backoff = min((uint32_t)(backoff * 2), (uint32_t)MQTT_BACKOFF_MAX);
    }
    return;
  }
  mqttClient.loop();
}

static bool parseZahl(const char* s, float& out) {
  char* end;
  out = strtof(s, &end);
  while (*end == ' ' || *end == '\r' || *end == '\n') end++;
  return end != s && *end == '\0' && std::isfinite(out);
}

static void verarbeiteVenus(byte* payload, unsigned int length) {
  StaticJsonDocument<256> filter;
  filter["Controller_tekst"] = true;
  filter["Modbus_ok"]        = true;
  filter["Lifebit"]          = true;
  filter["Setpoint"]         = true;
  filter["AC power bat"]     = true;
  filter["Reset_reason"]     = true;
  filter["Uptime"]           = true;
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, payload, length, DeserializationOption::Filter(filter))) {
    DEBUG_PRINT(F("Venus-JSON fehlerhaft\n"));
    return;
  }
  const uint32_t lb = strtoul(doc["Lifebit"] | "0", nullptr, 10);
  if (lb != w.venusLifebit) { w.venusLifebit = lb; w.tVenusLifebit = millis(); }
  strncpy(w.venusText, doc["Controller_tekst"] | "", sizeof(w.venusText) - 1);
  strncpy(w.venusReset, doc["Reset_reason"] | "", sizeof(w.venusReset) - 1);
  w.venusModbusOk = strcmp(doc["Modbus_ok"] | "1", "0") != 0;
  w.venusSet      = atoi(doc["Setpoint"] | "0");
  w.venusAc       = atoi(doc["AC power bat"] | "0");
  w.venusUptime   = strtoul(doc["Uptime"] | "0", nullptr, 10);
  w.tVenus        = millis();
}

void callbackMqttReceive(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, T_VENUS) == 0) {                      // großes JSON
    verarbeiteVenus(payload, length);
    return;
  }

  char msg[32] = {0};
  if (length >= sizeof(msg)) length = sizeof(msg) - 1;
  memcpy(msg, payload, length);

  if (strcmp(topic, LAMP_TOPIC_ONOFF) == 0) {             // tatsächlicher Lampenzustand
    lampOn = (strcmp(msg, "ON") == 0);
    return;
  }

  float v;
  if (!parseZahl(msg, v)) {
    DEBUG_PRINTF("Ungueltig %s: %s\n", topic, msg);
    return;
  }
  if (testMode) return;                                    // Testdaten nicht überschreiben
  const uint32_t now = millis();

  if      (!strcmp(topic, T_GRID))     { w.grid = v; w.tGrid = now; }
  else if (!strcmp(topic, T_GEN))      { w.gen = v; w.tGen = now; }
  else if (!strcmp(topic, T_HB))       { if ((uint32_t)v != w.hb) { w.hb = (uint32_t)v; w.tHb = now; } }
  else if (!strcmp(topic, T_GVALID))   { w.gridValid = (v > 0.5f); }
  else if (!strcmp(topic, T_INV))      { w.invOnline = (v > 0.5f); }
  else if (!strcmp(topic, T_RSSI))     { w.rssi = (int)v; }
  else if (!strcmp(topic, T_MQC))      { w.mqttConnects = (uint32_t)v; }
  else if (!strcmp(topic, T_MBERR))    { w.modbusErr = (uint32_t)v; }
  else if (!strcmp(topic, T_BUPTIME))  { w.bridgeUptime = (uint32_t)v; }
  else if (!strcmp(topic, T_AKKU))     { w.akku = v; w.tAkku = now; }
  else if (!strcmp(topic, T_SOC))      { w.soc = lroundf(v); w.tSoc = now; }
  else if (!strcmp(topic, T_AUTARKIE)) { w.autarkie = lroundf(v); w.tAutarkie = now; }
  else if (!strcmp(topic, T_SOLL))     { w.soll = v * 1000.0f; w.tSoll = now; }
}

// =============================================================================
// Tasten
// =============================================================================
static void zeigeFeedback(const char* text, uint16_t color, uint8_t font) {
  if (page != 0) return;
  M5.Lcd.fillRect(260, 8, 52, 28, TFT_BLACK);
  M5.Lcd.setTextColor(color);
  M5.Lcd.setTextSize(1);
  M5.Lcd.drawCentreString(text, 286, font == 4 ? 10 : 12, font);
  feedbackUntil = millis() + FEEDBACK_MS;
}

void handleButtons() {
  // Lang zuerst prüfen: wasReleasefor() ist nur bei langem Druck wahr
  if (M5.BtnA.wasReleasefor(LONG_PRESS_MS)) {
    testMode = !testMode;
    DEBUG_PRINTF("Testmodus: %s\n", testMode ? "AN" : "AUS");
    forceRedraw = true;
  } else if (M5.BtnA.wasReleased()) {
    lampOn = !lampOn;
    mqttClient.publish(LAMP_TOPIC_ONOFF, lampOn ? "ON" : "OFF", true);   // retained
    zeigeFeedback(lampOn ? "AN" : "AUS", lampOn ? TFT_GREEN : TFT_RED, 2);
  }

  if (M5.BtnB.wasReleasefor(LONG_PRESS_MS)) {
    page = (page + 1) % 3;
    forceRedraw = true;
  } else if (M5.BtnB.wasReleased()) {
    mqttClient.publish(LAMP_TOPIC_BRIGHTNESS, "+10", false);
    zeigeFeedback("+", TFT_WHITE, 4);
  }

  if (M5.BtnC.wasReleasefor(LONG_PRESS_MS)) {
    dimmed = !dimmed;
    M5.Lcd.setBrightness(dimmed ? BRIGHT_DIM : BRIGHT_NORMAL);
  } else if (M5.BtnC.wasReleased()) {
    mqttClient.publish(LAMP_TOPIC_BRIGHTNESS, "-10", false);
    zeigeFeedback("-", TFT_WHITE, 4);
  }
}

void handleTestMode() {
  static uint32_t testTime = 0;
  if (!testMode || millis() - testTime < 5000) return;
  testTime = millis();
  const uint32_t now = millis();
  w.gen  = random(0, 10000);   w.tGen  = now;
  w.grid = random(-500, 1000); w.tGrid = now; w.tHb = now; w.gridValid = true;
  w.akku = random(-500, 500);  w.tAkku = now;
  w.soc  = random(20, 90);     w.tSoc  = now;
  w.autarkie = random(0, 100); w.tAutarkie = now;
}

// =============================================================================
// Verlauf
// =============================================================================
static int16_t clamp16(float v) {
  if (v > 32000) return 32000;
  if (v < -32000) return -32000;
  return (int16_t)lroundf(v);
}

void sampleHistory() {
  if (histLast != 0 && millis() - histLast < HIST_INTERVAL_MS) return;
  histLast = millis();
  histGrid[histIdx] = bridgeOk() ? clamp16(w.grid) : HIST_INVALID;
  histGen[histIdx]  = frisch(w.tGen, BRIDGE_MAX_AGE_MS) ? clamp16(w.gen) : HIST_INVALID;
  histAkku[histIdx] = frisch(w.tAkku, AKKU_MAX_AGE_MS) ? clamp16(w.akku) : HIST_INVALID;
  histIdx = (histIdx + 1) % HIST_POINTS;
  if (page == 1) forceRedraw = true;                       // neue Daten → Diagramm neu
}

// =============================================================================
// Zeichnen
// =============================================================================
void drawPage() {
  const bool force = forceRedraw;
  forceRedraw = false;
  if (force) {
    M5.Lcd.fillScreen(TFT_NAVY);
    M5.Lcd.drawRoundRect(1, 1, 318, 238, 10, TFT_LIGHTGREY);
    M5.Lcd.drawRoundRect(2, 2, 316, 236, 8, TFT_DARKGREY);
    fsPV.text[0] = fsNetz.text[0] = fsAkku.text[0] = fsSoc.text[0] = fsAut.text[0] = '\0';
  }
  switch (page) {
    case 0: drawOverview(force); break;
    case 1: if (force) drawHistory(); break;
    case 2: drawDiagnose(); break;
  }
}

void drawFieldShadow(int x, int y, int w, int h, int radius, uint16_t shadowColor) {
  M5.Lcd.fillRoundRect(x + 4, y + 4, w - 8, h - 8, radius - 4, shadowColor);
}

void drawFieldBorder(int x, int y, int w, int h, int radius, uint16_t borderColor) {
  M5.Lcd.drawRoundRect(x, y, w, h, radius, borderColor);
  M5.Lcd.drawRoundRect(x + 1, y + 1, w - 2, h - 2, radius - 1, borderColor);
  M5.Lcd.drawRoundRect(x + 2, y + 2, w - 4, h - 4, radius - 2, borderColor);
  M5.Lcd.drawRoundRect(x + 3, y + 3, w - 6, h - 6, radius - 3, borderColor);
}

void fillGradientRoundRect(int x, int y, int w, int h, int radius, uint16_t colorStart, uint16_t colorEnd) {
  if (h <= 0 || w <= 0 || radius <= 0) return;

  uint8_t rStart = (colorStart >> 11) & 0x1F;
  uint8_t gStart = (colorStart >> 5) & 0x3F;
  uint8_t bStart = colorStart & 0x1F;
  uint8_t rEnd = (colorEnd >> 11) & 0x1F;
  uint8_t gEnd = (colorEnd >> 5) & 0x3F;
  uint8_t bEnd = colorEnd & 0x1F;

  radius = max(radius - 4, 0);
  int arcWidths[11];
  for (int dy = 0; dy <= radius && dy < 11; dy++) {
    arcWidths[dy] = (int)sqrt((float)(radius * radius - dy * dy));
  }

  x += 4; y += 4; w -= 8; h -= 8;

  for (int i = 0; i < h; i++) {
    float ratio = (float)i / (h - 1.0);
    uint16_t gradColor = (uint16_t)(
      ((rStart + (uint8_t)((rEnd - rStart) * ratio + 0.5f)) << 11) |
      ((gStart + (uint8_t)((gEnd - gStart) * ratio + 0.5f)) << 5) |
      (bStart + (uint8_t)((bEnd - bStart) * ratio + 0.5f))
    );

    int lineStartX, lineLen;
    if (i < radius) {
      lineStartX = x + radius - arcWidths[i];
      lineLen = w - 2 * (radius - arcWidths[i]);
    } else if (i >= h - radius) {
      int dy = h - 1 - i;
      lineStartX = x + radius - arcWidths[dy];
      lineLen = w - 2 * (radius - arcWidths[dy]);
    } else {
      lineStartX = x;
      lineLen = w;
    }

    if (lineStartX < 0) { lineLen += lineStartX; lineStartX = 0; }
    if (lineLen > 0 && lineStartX + lineLen > 320) lineLen = 320 - lineStartX;
    if (lineLen > 0) {
      M5.Lcd.drawFastHLine(lineStartX, y + i, lineLen, gradColor);
    }
  }
}

// Ein Feld nur neu zeichnen, wenn sich Text oder Farben geändert haben
struct FeldDef {
  int x, y, w, h, r;
  uint16_t shadow, gradStart, gradEnd;
  const char* titel;
  int titelX, titelY, titelFont, titelSize;
  int wertY, wertFont, wertSize;
};

static void drawFeld(const FeldDef& d, FieldState& st, const char* text,
                     uint16_t border, uint16_t textColor) {
  if (strcmp(st.text, text) == 0 && st.border == border && st.textColor == textColor) return;
  strncpy(st.text, text, sizeof(st.text) - 1);
  st.text[sizeof(st.text) - 1] = '\0';
  st.border = border;
  st.textColor = textColor;

  drawFieldShadow(d.x, d.y, d.w, d.h, d.r, d.shadow);
  fillGradientRoundRect(d.x, d.y, d.w, d.h, d.r, d.gradStart, d.gradEnd);
  drawFieldBorder(d.x, d.y, d.w, d.h, d.r, border);

  M5.Lcd.setTextColor(TFT_BLACK);
  M5.Lcd.setTextSize(d.titelSize);
  M5.Lcd.drawCentreString(d.titel, d.titelX, d.titelY, d.titelFont);
  M5.Lcd.setTextColor(textColor);
  M5.Lcd.setTextSize(d.wertSize);
  M5.Lcd.drawCentreString(text, d.titelX, d.wertY, d.wertFont);
  M5.Lcd.setTextSize(1);
}

static const FeldDef F_PV   = {  5,   5, 310, 75, 10, TFT_BLUE,     TFT_CYAN,   TFT_BLUE,        "PV Leistung",     160,  12, 1, 2,  40, 2, 2 };
static const FeldDef F_NETZ = {  5,  85, 147, 80,  8, TFT_DARKGREEN, TFT_GREEN, TFT_GREENYELLOW, "Netz [W]",         78,  92, 1, 2, 118, 2, 2 };
static const FeldDef F_AKKU = {165,  85, 147, 80,  8, TFT_DARKGREY, TFT_ORANGE, TFT_YELLOW,      "Akku [W]",        238,  92, 1, 2, 118, 2, 2 };
static const FeldDef F_SOC  = {  5, 170, 147, 65,  8, TFT_DARKGREY, TFT_YELLOW, TFT_WHITE,       "Akku Level [%]",   78, 177, 2, 1, 200, 2, 2 };
static const FeldDef F_AUT  = {165, 170, 147, 65,  8, TFT_PURPLE,   TFT_MAGENTA, TFT_PINK,       "Autarkie [%]",    238, 177, 2, 1, 200, 2, 2 };

void drawOverview(bool force) {
  (void)force;
  char buf[16];
  const bool bridge = bridgeOk() || testMode;

  // PV: grau, wenn keine aktuellen Daten oder Wechselrichter aus (Nacht)
  const int gen = lroundf(w.gen);
  if (gen >= 1000) snprintf(buf, sizeof(buf), "%.1f kW", gen / 1000.0f);
  else             snprintf(buf, sizeof(buf), "%d W", gen);
  const bool pvAktuell = (bridge && frisch(w.tGen, BRIDGE_MAX_AGE_MS)) || testMode;
  if (feedbackUntil == 0) {                                // Feedback-Anzeige nicht übermalen
    drawFeld(F_PV, fsPV, buf, TFT_DARKCYAN, pvAktuell ? TFT_BLACK : TFT_DARKGREY);
  }

  // Netz: Rahmen rot bei > 500 W Bezug oder veraltetem Zählerwert
  const int grid = lroundf(w.grid);
  snprintf(buf, sizeof(buf), "%d W", grid);
  uint16_t netzBorder = (!bridge || grid > 500) ? TFT_RED : TFT_DARKGREEN;
  drawFeld(F_NETZ, fsNetz, buf, netzBorder, bridge ? TFT_BLACK : TFT_DARKGREY);

  // Akku: Rahmenfarbe = Zustand des Venus
  const int akku = lroundf(w.akku);
  snprintf(buf, sizeof(buf), "%d W", akku);
  uint16_t akkuBorder;
  switch (testMode ? (akku > 0 ? VZ_LAEDT : VZ_ENTLAEDT) : venusZustand(nullptr)) {
    case VZ_STOERUNG: akkuBorder = TFT_RED;      break;
    case VZ_LAEDT:    akkuBorder = TFT_GREEN;    break;
    case VZ_ENTLAEDT: akkuBorder = TFT_ORANGE;   break;
    default:          akkuBorder = TFT_DARKGREY; break;
  }
  const bool akkuAktuell = frisch(w.tAkku, AKKU_MAX_AGE_MS) || testMode;
  drawFeld(F_AKKU, fsAkku, buf, akkuBorder, akkuAktuell ? TFT_BLACK : TFT_DARKGREY);

  // Ladestand
  snprintf(buf, sizeof(buf), "%d%%", w.soc);
  uint16_t socBorder = (w.soc > 75) ? TFT_GREEN : (w.soc > 25) ? TFT_YELLOW : TFT_RED;
  drawFeld(F_SOC, fsSoc, buf, socBorder, frisch(w.tSoc, SLOW_MAX_AGE_MS) ? TFT_BLACK : TFT_DARKGREY);

  // Autarkie
  snprintf(buf, sizeof(buf), "%d%%", w.autarkie);
  drawFeld(F_AUT, fsAut, buf, TFT_PURPLE, frisch(w.tAutarkie, SLOW_MAX_AGE_MS) ? TFT_BLACK : TFT_DARKGREY);

  if (testMode) {
    M5.Lcd.setTextColor(TFT_RED, TFT_BLACK);
    M5.Lcd.setTextSize(1);
    M5.Lcd.drawString("TEST", 12, 10, 2);
  }
}

void drawHistory() {
  const int x0 = 42, x1 = 312, y0 = 34, y1 = 200;          // Diagrammfläche
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(TFT_WHITE);
  M5.Lcd.drawCentreString("Verlauf 60 min", 160, 8, 2);

  // Wertebereich bestimmen (immer mit 0)
  int vmin = 0, vmax = 100;
  for (int i = 0; i < HIST_POINTS; i++) {
    const int16_t vals[3] = { histGrid[i], histGen[i], histAkku[i] };
    for (int k = 0; k < 3; k++) {
      if (vals[k] == HIST_INVALID) continue;
      if (vals[k] < vmin) vmin = vals[k];
      if (vals[k] > vmax) vmax = vals[k];
    }
  }
  const int span = vmax - vmin;
  vmin -= span / 20; vmax += span / 20;
  auto yPos = [&](int v) { return y1 - (long)(v - vmin) * (y1 - y0) / (vmax - vmin); };

  M5.Lcd.fillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, TFT_BLACK);
  M5.Lcd.drawRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, TFT_DARKGREY);
  M5.Lcd.drawFastHLine(x0, yPos(0), x1 - x0, TFT_LIGHTGREY);   // Nulllinie

  char buf[12];
  M5.Lcd.setTextColor(TFT_LIGHTGREY);
  snprintf(buf, sizeof(buf), "%d", vmax);  M5.Lcd.drawRightString(buf, x0 - 3, y0, 1);
  snprintf(buf, sizeof(buf), "%d", vmin);  M5.Lcd.drawRightString(buf, x0 - 3, y1 - 8, 1);
  M5.Lcd.drawRightString("0", x0 - 3, yPos(0) - 4, 1);
  M5.Lcd.drawString("-60 min", x0, y1 + 4, 1);
  M5.Lcd.drawRightString("jetzt", x1, y1 + 4, 1);

  const int16_t* reihen[3] = { histGen, histGrid, histAkku };
  const uint16_t farben[3]  = { TFT_YELLOW, TFT_GREEN, TFT_ORANGE };
  for (int k = 0; k < 3; k++) {
    int px = -1, py = 0;
    for (int n = 0; n < HIST_POINTS; n++) {
      const int16_t v = reihen[k][(histIdx + n) % HIST_POINTS];   // älteste zuerst
      const int x = x0 + (long)n * (x1 - x0) / (HIST_POINTS - 1);
      if (v == HIST_INVALID) { px = -1; continue; }
      const int y = yPos(v);
      if (px >= 0) M5.Lcd.drawLine(px, py, x, y, farben[k]);
      px = x; py = y;
    }
  }

  // Legende
  M5.Lcd.setTextColor(TFT_YELLOW); M5.Lcd.drawString("PV",   60, 218, 2);
  M5.Lcd.setTextColor(TFT_GREEN);  M5.Lcd.drawString("Netz", 130, 218, 2);
  M5.Lcd.setTextColor(TFT_ORANGE); M5.Lcd.drawString("Akku", 210, 218, 2);
}

static void diagZeile(int y, const char* label, const char* wert, uint16_t farbe) {
  M5.Lcd.fillRect(8, y, 304, 16, TFT_NAVY);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(TFT_LIGHTGREY);
  M5.Lcd.drawString(label, 10, y, 2);
  M5.Lcd.setTextColor(farbe);
  M5.Lcd.drawString(wert, 120, y, 2);
}

static void dauerText(char* buf, size_t len, uint32_t s) {
  if (s < 120)        snprintf(buf, len, "%lu s", (unsigned long)s);
  else if (s < 7200)  snprintf(buf, len, "%lu min", (unsigned long)(s / 60));
  else if (s < 172800) snprintf(buf, len, "%lu h", (unsigned long)(s / 3600));
  else                snprintf(buf, len, "%lu Tage", (unsigned long)(s / 86400));
}

void drawDiagnose() {
  char buf[64], d1[16], d2[16];
  int y = 8;
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Lcd.drawCentreString("Diagnose", 160, y, 2);
  y += 20;

  // Bridge
  const bool bOk = bridgeOk();
  dauerText(d1, sizeof(d1), alterS(w.tHb));
  snprintf(buf, sizeof(buf), "%s (vor %s)", bOk ? "OK" : "STOERUNG", w.tHb ? d1 : "-");
  diagZeile(y, "Zaehler/Bridge", buf, bOk ? TFT_GREEN : TFT_RED); y += 17;
  dauerText(d1, sizeof(d1), w.bridgeUptime);
  snprintf(buf, sizeof(buf), "%d dBm, %lu Verb., %s", w.rssi, (unsigned long)w.mqttConnects, d1);
  diagZeile(y, "  WLAN", buf, w.rssi < -75 ? TFT_ORANGE : TFT_WHITE); y += 17;
  snprintf(buf, sizeof(buf), "%lu Fehler", (unsigned long)w.modbusErr);
  diagZeile(y, "  Modbus", buf, w.modbusErr ? TFT_ORANGE : TFT_WHITE); y += 17;

  // Regelung
  const bool sOk = frisch(w.tSoll, SOLL_MAX_AGE_MS);
  dauerText(d1, sizeof(d1), alterS(w.tSoll));
  snprintf(buf, sizeof(buf), "%d W (vor %s)", (int)lroundf(w.soll), w.tSoll ? d1 : "-");
  diagZeile(y, "Regelung", buf, sOk ? TFT_GREEN : TFT_RED); y += 17;

  // Venus-Controller
  const char* grund = "";
  const VenusZustand vz = venusZustand(&grund);
  dauerText(d1, sizeof(d1), alterS(w.tVenus));
  snprintf(buf, sizeof(buf), "%s (vor %s)", vz == VZ_STOERUNG ? "STOERUNG" : "OK", w.tVenus ? d1 : "-");
  diagZeile(y, "Venus-Contr.", buf, vz == VZ_STOERUNG ? TFT_RED : TFT_GREEN); y += 17;
  snprintf(buf, sizeof(buf), "%s %d/%d W", venusTextDe(), w.venusSet, w.venusAc);
  diagZeile(y, "  Soll/Ist", buf, TFT_WHITE); y += 17;
  dauerText(d1, sizeof(d1), w.venusUptime);
  snprintf(buf, sizeof(buf), "Modbus %s, seit %s", w.venusModbusOk ? "ok" : "FEHLER", d1);
  diagZeile(y, "  Controller", buf, w.venusModbusOk ? TFT_WHITE : TFT_RED); y += 17;
  snprintf(buf, sizeof(buf), "%s", w.venusReset[0] ? w.venusReset : "-");
  diagZeile(y, "  Neustart", buf, TFT_WHITE); y += 17;
  if (vz == VZ_STOERUNG) { diagZeile(y, "  Grund", grund, TFT_RED); }
  else                   { diagZeile(y, "", "", TFT_WHITE); }
  y += 20;

  // Display selbst
  dauerText(d1, sizeof(d1), millis() / 1000);
  snprintf(buf, sizeof(buf), "%d dBm, MQTT %s, %s", (int)WiFi.RSSI(),
           mqttClient.connected() ? "ok" : "weg", d1);
  diagZeile(y, "Display", buf, mqttClient.connected() ? TFT_WHITE : TFT_RED); y += 17;

  // Letzte Störung
  if (lastFaultMillis) {
    dauerText(d2, sizeof(d2), alterS(lastFaultMillis));
    snprintf(buf, sizeof(buf), "vor %s: %s", d2, lastFault);
  } else {
    snprintf(buf, sizeof(buf), "keine");
  }
  diagZeile(y, "Letzte Stoer.", buf, lastFaultMillis ? TFT_ORANGE : TFT_GREEN);
}
