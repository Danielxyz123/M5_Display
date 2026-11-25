// =============================================================================
// M5Stack-Display für PV / Netz / Venus-Speicher
// =============================================================================
// Seiten (A/C blaettern; Verlauf und Diagnose erst nach langem Druck auf B):
//   1. Übersicht: PV (Leistung und Tagesertrag), Netz, Akku, Ladestand, Autarkie
//      Rahmenfarbe Akku: grün = lädt, orange = entlädt, grau = Stand-by/voll,
//                        rot = Störung am Venus (kein Sollwert, Modbus, keine Daten)
//      Grauer Wert = Daten nicht aktuell
//   2. Heizung: Außen- und Wohnzimmertemperatur, Vorlauf FBH, Kessel, Warmwasser,
//      Brenner heute (Laufzeit, Starts, Laufzeitanteil der letzten 24 h)
//      Rahmen: Kessel orange = Brenner läuft, Warmwasser orange = Speicherladung,
//      rot = Vorlauf bzw. Warmwasser deutlich unter Soll
//   3. Wetter Melle: jetzt, die naechsten Stunden, heute und morgen mit Regen und PV-Prognose
//      (Open-Meteo ueber das ioBroker-Skript WetterMqttDisplay)
//      Glaette-Warnung (DWD oder eigene Abschaetzung): Kasten oben rechts,
//      orange = moeglich, rot = wahrscheinlich / DWD-Warnung; auf der Heizungsseite "GLATT!"
//      Seite 1, 2 und 3 wechseln automatisch alle 10 s
//   4. Verlauf der letzten 60 min (Netz, PV, Akku)
//   5. Diagnose (Bridge, Regelung, Venus-Controller, Display)
//
// Tasten:  Klick (beim Loslassen):  A = Seite zurueck   B = Lampe an/aus   C = Seite vor
//          halten:  A = Lampe dunkler, C = Lampe heller (wiederholt, solange gehalten)
//                   B 1,5 s = Verlauf/Diagnose freigeben (springt zur Diagnose; dort: Testmodus)
//                   A + C 3 s = Display dimmen
//          A/C blaettern nur durch Solar, Heizung, Wetter; nach Freigabe durch alle Seiten.
//          Nach manuellem Seitenwechsel bleibt die Seite 1 min stehen, danach endet auch
//          die Freigabe von Verlauf/Diagnose.
// =============================================================================

#include <M5Stack.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <ArduinoOTA.h>                // Updates über WLAN
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
#define MQTT_STILL_MS        30000     // so lange keine Nachricht trotz "verbunden" -> neu verbinden
#define DIAG_PUBLISH_MS      60000     // eigene Verbindungswerte an ioBroker melden
#define LONG_PRESS_MS        3000      // so lange A + C halten: Display dimmen
#define DIAG_PRESS_MS        1500      // so lange B halten: Verlauf/Diagnose freigeben
#define KLICK_MAX_MS         500       // kürzer = Klick, länger gehalten = Lampe dimmen
#define DIMM_INTERVALL_MS    400       // beim Halten alle 400 ms eine Helligkeitsstufe
#define FEEDBACK_MS          800
#define BRIGHT_NORMAL        100
#define BRIGHT_DIM           10

// Wann gelten Werte als veraltet?
#define BRIDGE_MAX_AGE_MS    10000     // Lebenszeichen der Bridge
#define VENUS_MAX_AGE_MS     15000     // Venus-Controller (JSON, Lifebit)
#define SOLL_MAX_AGE_MS      10000     // Regelskript sendet jede Sekunde
#define AKKU_MAX_AGE_MS      60000     // Shelly-Spiegel sendet nur bei Änderung
#define SLOW_MAX_AGE_MS      1800000UL // Ladestand/Autarkie ändern sich selten
#define PV_HEUTE_MAX_AGE_MS  600000    // Tagesertrag kommt alle 30 s
#define STANDBY_W            15        // darunter gilt der Akku als ruhend

// Verlauf: 120 Punkte à 30 s = 60 min
#define HIST_POINTS          120
#define HIST_INTERVAL_MS     30000
#define HIST_INVALID         INT16_MIN

// === TOPICS ===
#define T_GRID      "PV/grid_powerFast"
#define T_GEN       "PV/generationPower"
#define T_PV_HEUTE  "PV/generation_totalPerDay"           // PV-Ertrag heute [kWh] (Bridge, um Mitternacht 0)
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
#define T_DIAG      "M5Display/diag"                      // JSON: RSSI, AP, Abbrueche, Gruende
#define T_HEIZUNG   "oeg_kmsd/state"                      // Heizungsregelung OEG KMS-D (JSON, ca. alle 10 s)
#define T_BR_ZEIT   "Heizung/Brenner_Laufzeit_heute"      // vom ioBroker-Skript gespiegelt [h]
#define T_BR_STARTS "Heizung/Brenner_Starts_heute"
#define T_BR_24H    "Heizung/Brenner_Anteil_24h"          // Brennerlaufzeit-Anteil letzte 24 h [%]
#define T_WOHNZIMMER "sensor1/temperature"                 // Wohnzimmertemperatur [°C]
#define WOHNZIMMER_MAX_AGE_MS 900000UL                     // älter als 15 min → grau
#define T_WETTER    "Wetter/vorhersage"                   // Vorhersage Melle (JSON, ioBroker-Skript)
#define WETTER_MAX_AGE_MS    5400000UL // Abruf alle 30 min; älter als 90 min → grau

// Heizung
#define HEIZ_MAX_AGE_MS      120000    // Heizungsregelung sendet ca. alle 10 s
#define BRENNER_MAX_AGE_MS   600000    // Brennerwerte kommen spätestens jede Minute
#define FBH_ABWEICHUNG_K     5.0f      // Vorlauf so weit unter Soll → roter Rahmen
#define WW_ABWEICHUNG_K      5.0f      // Warmwasser so weit unter Soll → roter Rahmen

// Seiten
#define SEITE_SOLAR          0
#define SEITE_HEIZUNG        1
#define SEITE_WETTER         2
#define SEITE_VERLAUF        3
#define SEITE_DIAGNOSE       4
#define SEITEN               5
#define AUTO_SEITEN          3         // Solar → Heizung → Wetter wechseln automatisch
#define AUTO_WECHSEL_MS      10000
#define MANUELL_HALTEN_MS    60000     // manuell gewählte Seite so lange stehen lassen

// === LAMPE STEUERUNG ÜBER TASTEN ===
#define LAMP_TOPIC_ONOFF        "lampe/wohnzimmer/set"            // ioBroker: ON / OFF (retained)
#define LAMP_TOPIC_BRIGHTNESS   "lampe/wohnzimmer/brightness_set" // ioBroker: +10 oder -10

// === DATEN ===
struct Werte {
  float    grid = 0, gen = 0, akku = 0, soll = 0, pvHeute = 0;
  int      soc = 0, autarkie = 0;
  uint32_t tPvHeute = 0;
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
  // Heizung
  float    tAussen = 0, tKessel = 0, tVorlauf = 0, tWW = 0;
  float    tWohnzimmer = 0;
  uint32_t tWohnzimmerZeit = 0;
  float    sollKessel = 0, sollVorlauf = 0, sollWW = 0;
  uint32_t relais = 0;
  uint32_t tHeiz = 0;
  float    brennerH = 0;
  int      brennerStarts = 0;
  float    brennerAnteil24 = 0;
  bool     brennerAnteil24Da = false;
  uint32_t tBrenner = 0;
} w;

struct FieldState {                    // zuletzt gezeichneter Zustand eines Feldes
  char     text[32];
  uint16_t border;
  uint16_t textColor;
};
// Wettervorhersage
struct WetterStunde { int std; float temp; int code; int regenP; bool tag; float mm; };   // mm = l/m²
struct WetterTag    { char name[8]; int code; float tmax, tmin, mm; int regenP; float pv; };
struct Wetter {
  uint32_t     t = 0;                  // Empfangszeit
  char         stand[8] = "";
  float        temp = 0;
  float        regen1h = 0;            // Regen in der kommenden Stunde [l/m²]
  int          code = 0, wind = 0;
  bool         tag = true;
  uint8_t      nStunden = 0, nTage = 0;
  WetterStunde h[4];
  WetterTag    d[4];
  int          glStufe = 0;            // 0 keine, 1 möglich, 2 wahrscheinlich/DWD, 3 DWD-Unwetter
  char         glArt[20] = "", glZusatz[16] = "", glZeit[12] = "";
} wet;
bool wetterNeu = false;                // neue Daten → Seite neu zeichnen

FieldState fsPV = {}, fsNetz = {}, fsAkku = {}, fsSoc = {}, fsAut = {};
FieldState fsAussen = {}, fsVorlauf = {}, fsKessel = {}, fsWW = {}, fsBrenner = {};

int16_t  histGrid[HIST_POINTS], histGen[HIST_POINTS], histAkku[HIST_POINTS];
uint16_t histIdx = 0;
uint32_t histLast = 0;

WiFiClient   espClient;
PubSubClient mqttClient(espClient);

uint8_t  page = SEITE_SOLAR;
uint32_t lastPageSwitch = 0;           // für den automatischen Wechsel
uint32_t lastButton = 0;               // letzter Tastendruck
bool     forceRedraw = true;
bool     dimmed = false;
bool     testMode = false;
bool     lampOn = false;
uint32_t feedbackUntil = 0;
uint32_t updateTimeOld = 0;
char     lastFault[64] = "";
uint32_t lastFaultMillis = 0;

// Verbindungsstatistik des Displays selbst. RTC_NOINIT: übersteht Neustarts
// (Watchdog, Absturz, Software), wird nur beim Einschalten gelöscht.
#define RTC_MAGIC 0x4D354431UL
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t wifiAbbrueche, mqttAbbrueche, mqttStill, neustarts;
RTC_NOINIT_ATTR uint8_t  eigenerGrund;  // 'W' = Neustart wegen WLAN, sonst 0
RTC_NOINIT_ATTR uint8_t  wifiGrund;     // letzter WLAN-Trenngrund (wifi_err_reason_t)
RTC_NOINIT_ATTR int32_t  mqttGrund;     // PubSubClient-Status beim letzten Abbruch
char     rstText[8] = "";              // Grund des letzten Neustarts
uint32_t tLetzteNachricht = 0;         // letzte empfangene MQTT-Nachricht
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
void drawHeizung();
void drawWetter(bool force);
void handleAutoWechsel();
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

  const esp_reset_reason_t rr = esp_reset_reason();
  if (rtcMagic != RTC_MAGIC || rr == ESP_RST_POWERON) {
    rtcMagic = RTC_MAGIC;
    wifiAbbrueche = mqttAbbrueche = mqttStill = neustarts = 0;
    eigenerGrund = wifiGrund = 0;
    mqttGrund = 0;
  } else {
    neustarts++;
  }
  const char* rs = "?";
  switch (rr) {
    case ESP_RST_POWERON:  rs = "Strom"; break;
    case ESP_RST_EXT:      rs = "Ext";   break;
    case ESP_RST_SW:       rs = eigenerGrund == 'W' ? "WLAN" : eigenerGrund == 'O' ? "OTA" : "SW"; break;
    case ESP_RST_PANIC:    rs = "Panic"; break;
    case ESP_RST_INT_WDT:  rs = "IntWDT"; break;
    case ESP_RST_TASK_WDT: rs = "TaskWDT"; break;
    case ESP_RST_WDT:      rs = "WDT";   break;
    case ESP_RST_BROWNOUT: rs = "Spann."; break;
    default: break;
  }
  strncpy(rstText, rs, sizeof(rstText) - 1);
  eigenerGrund = 0;
  DEBUG_PRINTF("Neustartgrund: %s (%d), Neustarts: %lu\n", rstText, (int)rr, (unsigned long)neustarts);

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
  DEBUG_PRINT(F("Klick: A=zurueck B=Lampe C=vor | halten: A=dunkler C=heller, B 1,5 s=Diagnose, A+C 3 s=Display dimmen\n"));
}

// =============================================================================
// OTA: Updates über WLAN (PlatformIO-Umgebung "m5stack-core-esp32-ota")
// =============================================================================
#define OTA_HOSTNAME "m5display"

static void otaStatus(const char* zeile1, const char* zeile2, uint16_t farbe) {
  M5.Lcd.fillScreen(TFT_NAVY);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(farbe);
  M5.Lcd.drawCentreString(zeile1, 160, 80, 4);
  M5.Lcd.setTextColor(TFT_WHITE);
  M5.Lcd.drawCentreString(zeile2, 160, 120, 2);
}

void handleOta() {
  static bool gestartet = false;
  if (!gestartet) {
    if (WiFi.status() != WL_CONNECTED) return;
    ArduinoOTA.setHostname(OTA_HOSTNAME);
#ifdef OTA_PASS
    ArduinoOTA.setPassword(OTA_PASS);
#endif
    ArduinoOTA.onStart([]() {
      esp_task_wdt_delete(NULL);                            // Update dauert länger als der Watchdog
      mqttClient.disconnect();
      otaStatus("Update", "wird uebertragen ...", TFT_YELLOW);
      M5.Lcd.drawRect(40, 160, 240, 20, TFT_WHITE);
    });
    ArduinoOTA.onProgress([](unsigned int fertig, unsigned int gesamt) {
      if (gesamt) M5.Lcd.fillRect(42, 162, (int)(236UL * fertig / gesamt), 16, TFT_GREEN);
    });
    ArduinoOTA.onEnd([]() {
      eigenerGrund = 'O';
      otaStatus("Update fertig", "Neustart ...", TFT_GREEN);
    });
    ArduinoOTA.onError([](ota_error_t fehler) {
      char buf[32];
      snprintf(buf, sizeof(buf), "Fehler %u", (unsigned)fehler);
      otaStatus("Update fehlgeschlagen", buf, TFT_RED);
      delay(3000);
      esp_task_wdt_add(NULL);
      forceRedraw = true;
    });
    ArduinoOTA.begin();
    gestartet = true;
    DEBUG_PRINTF("OTA bereit: %s.local / %s\n", OTA_HOSTNAME, WiFi.localIP().toString().c_str());
  }
  ArduinoOTA.handle();
}

void loop() {
  esp_task_wdt_reset();
  handleOta();
  M5.update();

  handleButtons();
  handleWifi();
  handleMqtt();
  handleTestMode();
  sampleHistory();
  pruefeStoerung();
  handleAutoWechsel();

  // Rückmeldung zum Tastendruck nach kurzer Zeit wieder entfernen (einmalig neu zeichnen)
  if (feedbackUntil != 0 && millis() > feedbackUntil) {
    feedbackUntil = 0;
    fsPV.text[0] = '\0';                                    // oberes Feld neu zeichnen erzwingen
    fsAussen.text[0] = '\0';
    wetterNeu = true;                                       // Wetterkopf neu zeichnen
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
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    wifiGrund = info.disconnected.reason;
  }, SYSTEM_EVENT_STA_DISCONNECTED);
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
  if (offlineSince == 0) {
    offlineSince = now; lastTry = now;
    if (now > 20000) wifiAbbrueche++;                      // nicht beim Start zählen
  }
  if (now - lastTry >= WIFI_RETRY_MS) {
    lastTry = now;
    DEBUG_PRINT(F("WiFi neuer Versuch\n"));
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
  if (now - offlineSince >= WIFI_MAX_OFFLINE_MS) {
    DEBUG_PRINT(F("WiFi zu lange weg, Neustart\n"));
    eigenerGrund = 'W';
    ESP.restart();
  }
}

void handleMqtt() {
  static uint32_t lastTry = 0;
  static uint32_t backoff = MQTT_BACKOFF_MIN;

  static bool warVerbunden = false;

  if (WiFi.status() != WL_CONNECTED) {
    if (warVerbunden) { warVerbunden = false; mqttAbbrueche++; mqttGrund = -3; }   // WLAN weg
    return;
  }

  // Halboffene Verbindung: Client meint "verbunden", es kommt aber nichts mehr an
  // (Bridge sendet mehrmals pro Sekunde) -> Socket schließen und neu verbinden
  if (mqttClient.connected() && millis() - tLetzteNachricht > MQTT_STILL_MS) {
    DEBUG_PRINT(F("MQTT still, neu verbinden\n"));
    mqttStill++;
    mqttClient.disconnect();
    espClient.stop();
  }

  if (!mqttClient.connected()) {
    if (warVerbunden) {
      warVerbunden = false;
      mqttAbbrueche++;
      mqttGrund = mqttClient.state();
      DEBUG_PRINTF("MQTT getrennt rc=%d\n", mqttGrund);
    }
    if (lastTry != 0 && millis() - lastTry < backoff) return;   // kein delay() mehr
    lastTry = millis();
    String clientId = "M5Stack-" + WiFi.macAddress();
    DEBUG_PRINT(F("MQTT verbinden...\n"));
    if (mqttClient.connect(clientId.c_str(), T_ONLINE, 1, true, "0")) {
      mqttClient.publish(T_ONLINE, "1", true);
      const char* topics[] = {
        T_GRID, T_GEN, T_HB, T_GVALID, T_INV, T_RSSI, T_MQC, T_MBERR, T_BUPTIME,
        T_AKKU, T_SOC, T_AUTARKIE, T_VENUS, T_SOLL, LAMP_TOPIC_ONOFF, T_PV_HEUTE,
        T_HEIZUNG, T_BR_ZEIT, T_BR_STARTS, T_BR_24H, T_WOHNZIMMER, T_WETTER
      };
      for (auto t : topics) mqttClient.subscribe(t);
      backoff = MQTT_BACKOFF_MIN;
      warVerbunden = true;
      tLetzteNachricht = millis();
      DEBUG_PRINT(F("MQTT verbunden\n"));
    } else {
      DEBUG_PRINTF("MQTT Fehler rc=%d, naechster Versuch in %lu ms\n", mqttClient.state(), (unsigned long)backoff);
      backoff = min((uint32_t)(backoff * 2), (uint32_t)MQTT_BACKOFF_MAX);
    }
    return;
  }
  mqttClient.loop();

  // Eigene Verbindungswerte melden (retained), damit ioBroker Abbrüche aufzeichnen kann
  static uint32_t tDiag = 0;
  if (mqttClient.connected() && (tDiag == 0 || millis() - tDiag >= DIAG_PUBLISH_MS)) {
    tDiag = millis();
    char buf[220];
    snprintf(buf, sizeof(buf),
             "{\"rssi\":%d,\"bssid\":\"%s\",\"kanal\":%d,\"uptime\":%lu,\"wifiAbbr\":%lu,\"wifiGrund\":%u,"
             "\"mqttAbbr\":%lu,\"mqttGrund\":%d,\"mqttStill\":%lu,\"neustarts\":%lu,\"rst\":\"%s\",\"heap\":%u}",
             (int)WiFi.RSSI(), WiFi.BSSIDstr().c_str(), (int)WiFi.channel(), (unsigned long)(millis() / 1000),
             (unsigned long)wifiAbbrueche, (unsigned)wifiGrund, (unsigned long)mqttAbbrueche, (int)mqttGrund,
             (unsigned long)mqttStill, (unsigned long)neustarts, rstText, (unsigned)ESP.getFreeHeap());
    mqttClient.publish(T_DIAG, buf, true);
  }
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

static void verarbeiteHeizung(byte* payload, unsigned int length) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, payload, length)) {
    DEBUG_PRINT(F("Heizungs-JSON fehlerhaft\n"));
    return;
  }
  w.tAussen     = doc["t2"] | 0.0f;
  w.tKessel     = doc["t3"] | 0.0f;
  w.tVorlauf    = doc["t4"] | 0.0f;
  w.tWW         = doc["t5"] | 0.0f;
  w.sollKessel  = doc["boiler_set"] | 0.0f;
  w.sollVorlauf = doc["flow_set"] | 0.0f;
  w.sollWW      = doc["dhw_set"] | 0.0f;
  w.relais      = doc["relays"] | 0;
  w.tHeiz       = millis();
}

static void verarbeiteWetter(byte* payload, unsigned int length) {
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, payload, length)) {
    DEBUG_PRINT(F("Wetter-JSON fehlerhaft\n"));
    return;
  }
  JsonArrayConst n = doc["n"];
  if (n.isNull()) return;
  strncpy(wet.stand, doc["z"] | "", sizeof(wet.stand) - 1);
  wet.temp = n[0] | 0.0f;
  wet.code = n[1] | 0;
  wet.wind = n[2] | 0;
  wet.tag  = (n[3] | 1) != 0;
  wet.regen1h = n[4] | 0.0f;
  wet.nStunden = 0;
  for (JsonArrayConst e : doc["h"].as<JsonArrayConst>()) {
    if (wet.nStunden >= 4) break;
    WetterStunde& h = wet.h[wet.nStunden++];
    h.std = e[0] | 0;  h.temp = e[1] | 0.0f;  h.code = e[2] | 0;
    h.regenP = e[3] | 0;  h.tag = (e[4] | 1) != 0;  h.mm = e[5] | 0.0f;
  }
  wet.nTage = 0;
  for (JsonArrayConst e : doc["d"].as<JsonArrayConst>()) {
    if (wet.nTage >= 4) break;
    WetterTag& d = wet.d[wet.nTage++];
    strncpy(d.name, e[0] | "", sizeof(d.name) - 1);
    d.name[sizeof(d.name) - 1] = '\0';
    d.code = e[1] | 0;  d.tmax = e[2] | 0.0f;  d.tmin = e[3] | 0.0f;
    d.mm = e[4] | 0.0f;  d.regenP = e[5] | 0;  d.pv = e[6] | -1.0f;
  }
  JsonArrayConst g = doc["g"];
  wet.glStufe = g.isNull() ? 0 : (g[0] | 0);
  strncpy(wet.glArt,    g.isNull() ? "" : (g[1] | ""), sizeof(wet.glArt) - 1);
  strncpy(wet.glZusatz, g.isNull() ? "" : (g[2] | ""), sizeof(wet.glZusatz) - 1);
  strncpy(wet.glZeit,   g.isNull() ? "" : (g[3] | ""), sizeof(wet.glZeit) - 1);
  wet.t = millis();
  wetterNeu = true;
}

void callbackMqttReceive(char* topic, byte* payload, unsigned int length) {
  tLetzteNachricht = millis();
  if (strcmp(topic, T_WETTER) == 0) {
    verarbeiteWetter(payload, length);
    return;
  }
  if (strcmp(topic, T_VENUS) == 0) {                      // großes JSON
    verarbeiteVenus(payload, length);
    return;
  }
  if (strcmp(topic, T_HEIZUNG) == 0) {
    verarbeiteHeizung(payload, length);
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
  const uint32_t now = millis();
  // Im Testmodus werden nur die fünf angezeigten Werte durch Testdaten ersetzt;
  // Lebenszeichen, Sollwert und Diagnose laufen normal weiter.
  const bool anzeigeWert = !strcmp(topic, T_GRID) || !strcmp(topic, T_GEN) || !strcmp(topic, T_AKKU) ||
                           !strcmp(topic, T_SOC)  || !strcmp(topic, T_AUTARKIE);
  if (testMode && anzeigeWert) return;

  if      (!strcmp(topic, T_GRID))     { w.grid = v; w.tGrid = now; }
  else if (!strcmp(topic, T_GEN))      { w.gen = v; w.tGen = now; }
  else if (!strcmp(topic, T_PV_HEUTE)) { w.pvHeute = v; w.tPvHeute = now; }
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
  else if (!strcmp(topic, T_BR_ZEIT))  { w.brennerH = v; w.tBrenner = now; }
  else if (!strcmp(topic, T_BR_STARTS)){ w.brennerStarts = lroundf(v); w.tBrenner = now; }
  else if (!strcmp(topic, T_BR_24H))   { w.brennerAnteil24 = v; w.brennerAnteil24Da = true; w.tBrenner = now; }
  else if (!strcmp(topic, T_WOHNZIMMER)) { w.tWohnzimmer = v; w.tWohnzimmerZeit = now; }
}

// =============================================================================
// Tasten
// =============================================================================
static void zeigeFeedback(const char* text, uint16_t color, uint8_t font) {
  if (page >= AUTO_SEITEN) return;                       // nur Solar, Heizung, Wetter
  M5.Lcd.fillRect(260, 8, 52, 28, TFT_BLACK);
  M5.Lcd.setTextColor(color);
  M5.Lcd.setTextSize(1);
  M5.Lcd.drawCentreString(text, 286, font == 4 ? 10 : 12, font);
  feedbackUntil = millis() + FEEDBACK_MS;
}

bool diagFrei = false;                 // Verlauf/Diagnose per langem B freigegeben

// Automatischer Wechsel Solar → Heizung → Wetter.
// Nach einem Tastendruck bleibt die Seite MANUELL_HALTEN_MS stehen; danach endet die Freigabe
// von Verlauf/Diagnose und es geht zur Solarseite zurück.
void handleAutoWechsel() {
  const uint32_t now = millis();
  if (lastButton != 0 && now - lastButton < MANUELL_HALTEN_MS) return;
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed() || feedbackUntil != 0) return;
  diagFrei = false;
  if (page < AUTO_SEITEN) {
    if (now - lastPageSwitch >= AUTO_WECHSEL_MS) {
      page = (page + 1) % AUTO_SEITEN;
      lastPageSwitch = now;
      forceRedraw = true;
    }
  } else {
    page = SEITE_SOLAR;
    lastPageSwitch = now;
    forceRedraw = true;
  }
}

static void seiteWechseln(int schritt) {
  const int n = diagFrei ? SEITEN : AUTO_SEITEN;           // ohne Freigabe nur Solar/Heizung/Wetter
  if (page >= n) page = SEITE_SOLAR;
  page = (page + n + schritt) % n;
  lastPageSwitch = millis();
  forceRedraw = true;
}

static void lampeDimmen(bool heller) {
  mqttClient.publish(LAMP_TOPIC_BRIGHTNESS, heller ? "+10" : "-10", false);
  zeigeFeedback(heller ? "+" : "-", TFT_WHITE, 4);
}

// Seitentaste (A oder C): Klick = blättern, Halten = Lampe dimmen (wiederholt)
struct SeitenTaste {
  bool     gedimmt = false;            // in diesem Druck schon gedimmt → kein Klick mehr
  uint32_t naechstesDimmen = 0;
};

static void seitenTaste(Button& taste, SeitenTaste& st, int schritt, bool heller, bool gesperrt) {
  const uint32_t now = millis();
  if (taste.wasPressed()) {
    st.gedimmt = false;
    st.naechstesDimmen = now + KLICK_MAX_MS;
  }
  if (taste.isPressed() && !gesperrt && (int32_t)(now - st.naechstesDimmen) >= 0) {
    lampeDimmen(heller);
    st.gedimmt = true;
    st.naechstesDimmen = now + DIMM_INTERVALL_MS;
  }
  if (taste.wasReleased()) {
    if (!st.gedimmt && !gesperrt) seiteWechseln(schritt);
    st.gedimmt = false;
  }
}

void handleButtons() {
  static SeitenTaste tA, tC;
  static bool langB = false;
  static bool kombi = false;           // A und C zusammen gedrückt → kein Klick, kein Dimmen
  static bool kombiErledigt = false;

  if (M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed()) {
    lastButton = millis();
  }

  // A + C zusammen: 3 s halten = Display dimmen
  if (M5.BtnA.isPressed() && M5.BtnC.isPressed()) {
    kombi = true;
    if (!kombiErledigt && M5.BtnA.pressedFor(LONG_PRESS_MS) && M5.BtnC.pressedFor(LONG_PRESS_MS)) {
      kombiErledigt = true;
      dimmed = !dimmed;
      M5.Lcd.setBrightness(dimmed ? BRIGHT_DIM : BRIGHT_NORMAL);
    }
  }

  // A (links): Klick = Seite zurück, halten = Lampe dunkler
  seitenTaste(M5.BtnA, tA, -1, false, kombi);
  // C (rechts): Klick = Seite vor, halten = Lampe heller
  seitenTaste(M5.BtnC, tC, +1, true, kombi);
  if (!M5.BtnA.isPressed() && !M5.BtnC.isPressed()) { kombi = false; kombiErledigt = false; }

  // B (Mitte): Klick = Lampe an/aus, 1,5 s halten = Verlauf/Diagnose freigeben und zur Diagnose;
  // auf der Diagnoseseite schaltet langes B den Testmodus
  if (!langB && M5.BtnB.pressedFor(DIAG_PRESS_MS)) {
    langB = true;
    if (page == SEITE_DIAGNOSE) {
      testMode = !testMode;
      DEBUG_PRINTF("Testmodus: %s\n", testMode ? "AN" : "AUS");
    } else {
      diagFrei = true;
      page = SEITE_DIAGNOSE;
      lastPageSwitch = millis();
    }
    forceRedraw = true;
  }
  if (M5.BtnB.wasReleased()) {
    if (!langB) {
      lampOn = !lampOn;
      mqttClient.publish(LAMP_TOPIC_ONOFF, lampOn ? "ON" : "OFF", true);   // retained
      zeigeFeedback(lampOn ? "AN" : "AUS", lampOn ? TFT_GREEN : TFT_RED, 2);
    }
    langB = false;
  }
}

void handleTestMode() {
  static uint32_t testTime = 0;
  if (!testMode || millis() - testTime < 5000) return;
  testTime = millis();
  const uint32_t now = millis();
  w.gen  = random(0, 10000);   w.tGen  = now;
  w.grid = random(-500, 1000); w.tGrid = now;
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
  if (page == SEITE_VERLAUF) forceRedraw = true;                      // neue Daten → Diagramm neu
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
    fsAussen.text[0] = fsVorlauf.text[0] = fsKessel.text[0] = fsWW.text[0] = fsBrenner.text[0] = '\0';
  }
  switch (page) {
    case SEITE_SOLAR:    drawOverview(force); break;
    case SEITE_HEIZUNG:  drawHeizung(); break;
    case SEITE_WETTER:   drawWetter(force); break;
    case SEITE_VERLAUF:  if (force) drawHistory(); break;
    case SEITE_DIAGNOSE: drawDiagnose(); break;
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

static void fmtKomma(char* buf, size_t len, float v);

// PV-Feld: aktuelle Leistung links, Tagesertrag rechts
static void drawPVFeld(const char* leistung, uint16_t farbeLeistung,
                       const char* heute, uint16_t farbeHeute) {
  char key[32];
  snprintf(key, sizeof(key), "%s|%s|%u", leistung, heute, farbeHeute);
  if (strcmp(fsPV.text, key) == 0 && fsPV.textColor == farbeLeistung) return;
  strncpy(fsPV.text, key, sizeof(fsPV.text) - 1);
  fsPV.text[sizeof(fsPV.text) - 1] = '\0';
  fsPV.textColor = farbeLeistung;
  fsPV.border = TFT_DARKCYAN;

  const FeldDef& d = F_PV;
  drawFieldShadow(d.x, d.y, d.w, d.h, d.r, d.shadow);
  fillGradientRoundRect(d.x, d.y, d.w, d.h, d.r, d.gradStart, d.gradEnd);
  drawFieldBorder(d.x, d.y, d.w, d.h, d.r, TFT_DARKCYAN);

  M5.Lcd.setTextColor(TFT_BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.drawCentreString(d.titel, 160, d.titelY, 1);
  M5.Lcd.setTextColor(farbeLeistung);
  M5.Lcd.setTextSize(2);
  M5.Lcd.drawCentreString(leistung, 105, 40, 2);           // aktuelle Leistung
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(farbeHeute);
  M5.Lcd.drawCentreString("heute", 250, 30, 2);            // Tagesertrag
  M5.Lcd.drawCentreString(heute, 250, 47, 4);              // größere Schrift (26 px)
}

void drawOverview(bool force) {
  (void)force;
  char buf[16];
  const bool bridge = bridgeOk() || testMode;

  // PV: grau, wenn keine aktuellen Daten oder Wechselrichter aus (Nacht)
  const int gen = lroundf(w.gen);
  if (gen >= 1000) { fmtKomma(buf, sizeof(buf), gen / 1000.0f); strncat(buf, " kW", sizeof(buf) - strlen(buf) - 1); }
  else             snprintf(buf, sizeof(buf), "%d W", gen);
  const bool pvAktuell = (bridge && frisch(w.tGen, BRIDGE_MAX_AGE_MS)) || testMode;
  char heute[16];
  if (w.tPvHeute) { fmtKomma(heute, sizeof(heute), w.pvHeute); strncat(heute, " kWh", sizeof(heute) - strlen(heute) - 1); }
  else            strcpy(heute, "--");
  const bool heuteAktuell = frisch(w.tPvHeute, PV_HEUTE_MAX_AGE_MS) || testMode;
  if (feedbackUntil == 0) {                                // Feedback-Anzeige nicht übermalen
    drawPVFeld(buf, pvAktuell ? TFT_BLACK : TFT_DARKGREY, heute, heuteAktuell ? TFT_BLACK : TFT_DARKGREY);
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

// =============================================================================
// Heizungsseite
// =============================================================================
struct HFeld {
  int x, y, w, h, r;
  uint16_t shadow, gradStart, gradEnd;
  const char* titel;
  int cx, titelY, titelFont, titelSize;
  int wertY, wertFont, wertSize;
  int subY, subFont;                   // subY = 0: keine zweite Zeile
};

static const HFeld H_AUSSEN  = {  5,   5, 310, 75, 10, TFT_BLUE,      TFT_CYAN,    TFT_BLUE,        "Aussentemperatur", 160,  12, 1, 2,  40, 2, 2,   0, 0 };
static const HFeld H_VORLAUF = {  5,  85, 147, 80,  8, TFT_DARKGREEN, TFT_GREEN,   TFT_GREENYELLOW, "Vorlauf FBH",       78,  92, 1, 2, 108, 2, 2, 146, 2 };
static const HFeld H_KESSEL  = {165,  85, 147, 80,  8, TFT_DARKGREY,  TFT_ORANGE,  TFT_YELLOW,      "Kessel",           238,  92, 1, 2, 108, 2, 2, 146, 2 };
static const HFeld H_WW      = {  5, 170, 147, 65,  8, TFT_DARKGREY,  TFT_YELLOW,  TFT_WHITE,       "Warmwasser",        78, 175, 2, 1, 191, 4, 1, 221, 1 };
static const HFeld H_BRENNER = {165, 170, 147, 65,  8, TFT_PURPLE,    TFT_MAGENTA, TFT_PINK,        "Brenner heute",    238, 175, 2, 1, 191, 4, 1, 221, 1 };

// Zahl mit einer Nachkommastelle und deutschem Komma
static void fmtKomma(char* buf, size_t len, float v) {
  snprintf(buf, len, "%.1f", v);
  for (char* p = buf; *p; p++) if (*p == '.') *p = ',';
}

// Wert zentriert ausgeben, optional mit Grad-Zeichen (die Schriften haben kein °)
static void drawWert(const char* zahl, bool grad, int cx, int y, int font, int size, uint16_t color) {
  M5.Lcd.setTextColor(color);
  M5.Lcd.setTextSize(size);
  if (!grad) {
    M5.Lcd.drawCentreString(zahl, cx, y, font);
    return;
  }
  const int r   = 2 + size;                                  // Radius des Grad-Kreises
  const int wz  = M5.Lcd.textWidth(zahl, font);
  const int wc  = M5.Lcd.textWidth("C", font);
  const int gap = 2 * r + 6;
  const int x0  = cx - (wz + gap + wc) / 2;
  M5.Lcd.drawString(zahl, x0, y, font);
  M5.Lcd.drawCircle(x0 + wz + gap / 2, y + r + 2, r, color);
  M5.Lcd.drawCircle(x0 + wz + gap / 2, y + r + 2, r - 1, color);
  M5.Lcd.drawString("C", x0 + wz + gap, y, font);
}

static void drawHFeld(const HFeld& d, FieldState& st, const char* zahl, bool grad,
                      const char* sub, uint16_t border, uint16_t textColor) {
  char key[32];
  snprintf(key, sizeof(key), "%s|%s", zahl, sub ? sub : "");
  if (strcmp(st.text, key) == 0 && st.border == border && st.textColor == textColor) return;
  strncpy(st.text, key, sizeof(st.text) - 1);
  st.text[sizeof(st.text) - 1] = '\0';
  st.border = border;
  st.textColor = textColor;

  drawFieldShadow(d.x, d.y, d.w, d.h, d.r, d.shadow);
  fillGradientRoundRect(d.x, d.y, d.w, d.h, d.r, d.gradStart, d.gradEnd);
  drawFieldBorder(d.x, d.y, d.w, d.h, d.r, border);

  M5.Lcd.setTextColor(TFT_BLACK);
  M5.Lcd.setTextSize(d.titelSize);
  M5.Lcd.drawCentreString(d.titel, d.cx, d.titelY, d.titelFont);
  drawWert(zahl, grad, d.cx, d.wertY, d.wertFont, d.wertSize, textColor);
  if (d.subY && sub && *sub) {
    M5.Lcd.setTextColor(textColor);
    M5.Lcd.setTextSize(1);
    M5.Lcd.drawCentreString(sub, d.cx, d.subY, d.subFont);
  }
  M5.Lcd.setTextSize(1);
}

// Oberes Feld der Heizungsseite: Außen links, Wohnzimmer rechts
static void drawTemperaturFeld(const char* aussen, bool aussenGrad, uint16_t farbeAussen,
                               const char* wohn, bool wohnGrad, uint16_t farbeWohn, int glaette) {
  char key[32];
  snprintf(key, sizeof(key), "%s|%s|%u|%d", aussen, wohn, farbeWohn, glaette);
  if (strcmp(fsAussen.text, key) == 0 && fsAussen.textColor == farbeAussen) return;
  strncpy(fsAussen.text, key, sizeof(fsAussen.text) - 1);
  fsAussen.text[sizeof(fsAussen.text) - 1] = '\0';
  fsAussen.textColor = farbeAussen;
  fsAussen.border = TFT_DARKCYAN;

  const HFeld& d = H_AUSSEN;
  drawFieldShadow(d.x, d.y, d.w, d.h, d.r, d.shadow);
  fillGradientRoundRect(d.x, d.y, d.w, d.h, d.r, d.gradStart, d.gradEnd);
  drawFieldBorder(d.x, d.y, d.w, d.h, d.r, TFT_DARKCYAN);

  M5.Lcd.setTextColor(TFT_BLACK);
  M5.Lcd.setTextSize(1);
  if (glaette > 0) {                                       // Glätte-Warnung aus der Wetterseite
    M5.Lcd.setTextColor(glaette >= 2 ? TFT_RED : TFT_MAROON);
    M5.Lcd.drawCentreString("Aussen  GLATT!", 85, 12, 2);
    M5.Lcd.setTextColor(TFT_BLACK);
  } else {
    M5.Lcd.drawCentreString("Aussen", 85, 12, 2);
  }
  M5.Lcd.drawCentreString("Wohnzimmer", 235, 12, 2);
  M5.Lcd.drawFastVLine(160, 14, 54, TFT_DARKCYAN);         // Trennlinie
  drawWert(aussen, aussenGrad, 85, 36, 2, 2, farbeAussen);
  drawWert(wohn, wohnGrad, 235, 36, 2, 2, farbeWohn);
  M5.Lcd.setTextSize(1);
}

void drawHeizung() {
  const bool daten  = w.tHeiz != 0;
  const bool aktuell = frisch(w.tHeiz, HEIZ_MAX_AGE_MS);
  const uint16_t farbe = aktuell ? TFT_BLACK : TFT_DARKGREY;
  const bool brenner     = w.relais & (1u << 0);              // R1 = Brenner
  const bool speicherLad = w.relais & (1u << 4);              // R5 = Speicherladung Warmwasser
  char zahl[12], sub[24], soll[12];

  // Außen- und Wohnzimmertemperatur
  char wohn[12];
  const bool wohnDa      = w.tWohnzimmerZeit != 0;
  const bool wohnAktuell = frisch(w.tWohnzimmerZeit, WOHNZIMMER_MAX_AGE_MS);
  if (daten) fmtKomma(zahl, sizeof(zahl), w.tAussen); else strcpy(zahl, "--");
  if (wohnDa) fmtKomma(wohn, sizeof(wohn), w.tWohnzimmer); else strcpy(wohn, "--");
  if (feedbackUntil == 0) {                                  // Tasten-Rückmeldung nicht übermalen
    const int glaette = frisch(wet.t, WETTER_MAX_AGE_MS) ? wet.glStufe : 0;
    drawTemperaturFeld(zahl, daten, farbe, wohn, wohnDa, wohnAktuell ? TFT_BLACK : TFT_DARKGREY, glaette);
  }

  // Vorlauf Fußbodenheizung: rot, wenn deutlich unter Soll
  if (daten) fmtKomma(zahl, sizeof(zahl), w.tVorlauf); else strcpy(zahl, "--");
  fmtKomma(soll, sizeof(soll), w.sollVorlauf);
  snprintf(sub, sizeof(sub), "Soll %s", soll);
  const bool vorlaufTief = aktuell && w.tVorlauf < w.sollVorlauf - FBH_ABWEICHUNG_K;
  drawHFeld(H_VORLAUF, fsVorlauf, zahl, daten, daten ? sub : nullptr,
            vorlaufTief ? TFT_RED : TFT_DARKGREEN, farbe);

  // Kessel: orange, solange der Brenner läuft
  if (daten) fmtKomma(zahl, sizeof(zahl), w.tKessel); else strcpy(zahl, "--");
  snprintf(sub, sizeof(sub), "Soll %d", (int)lroundf(w.sollKessel));
  drawHFeld(H_KESSEL, fsKessel, zahl, daten, daten ? sub : nullptr,
            (aktuell && brenner) ? TFT_ORANGE : TFT_DARKGREY, farbe);

  // Warmwasser: orange bei Speicherladung, rot wenn deutlich unter Soll
  if (daten) fmtKomma(zahl, sizeof(zahl), w.tWW); else strcpy(zahl, "--");
  snprintf(sub, sizeof(sub), "Soll %d", (int)lroundf(w.sollWW));
  uint16_t wwBorder = TFT_DARKGREY;
  if (aktuell && speicherLad)                          wwBorder = TFT_ORANGE;
  else if (aktuell && w.tWW < w.sollWW - WW_ABWEICHUNG_K) wwBorder = TFT_RED;
  drawHFeld(H_WW, fsWW, zahl, daten, daten ? sub : nullptr, wwBorder, farbe);

  // Brenner heute (vom ioBroker-Skript)
  const bool brOk = frisch(w.tBrenner, BRENNER_MAX_AGE_MS);
  if (w.tBrenner) {
    fmtKomma(zahl, sizeof(zahl), w.brennerH);
    strncat(zahl, " h", sizeof(zahl) - strlen(zahl) - 1);
    if (w.brennerAnteil24Da) {
      snprintf(sub, sizeof(sub), "%d Starts | 24h %d%%", w.brennerStarts, (int)lroundf(w.brennerAnteil24));
    } else {
      snprintf(sub, sizeof(sub), "%d Starts", w.brennerStarts);
    }
  } else {
    strcpy(zahl, "--");
    sub[0] = '\0';
  }
  drawHFeld(H_BRENNER, fsBrenner, zahl, false, sub, TFT_PURPLE, brOk ? TFT_BLACK : TFT_DARKGREY);
}

// =============================================================================
// Wetterseite
// =============================================================================
#define W_FELD   0x18E3                // Hintergrund der Wetterfelder (dunkelgrau-blau)
#define W_RAHMEN TFT_DARKCYAN
#define W_REGEN  0x5D9F                // hellblau
#define W_ORANGE 0xFB00                // kraeftiges Orange (TFT_ORANGE wirkt auf dem Panel gelb)

static void wetterFeld(int x, int y, int wd, int ht) {
  M5.Lcd.fillRoundRect(x, y, wd, ht, 8, W_FELD);
  M5.Lcd.drawRoundRect(x, y, wd, ht, 8, W_RAHMEN);
  M5.Lcd.drawRoundRect(x + 1, y + 1, wd - 2, ht - 2, 7, W_RAHMEN);
}

static void iconSonne(int cx, int cy, int r) {
  M5.Lcd.fillCircle(cx, cy, r * 5 / 10, TFT_YELLOW);
  for (int i = 0; i < 8; i++) {
    const float a = i * PI / 4;
    const float c = cosf(a), s = sinf(a);
    for (int d = -1; d <= 1; d++) {                       // 2–3 px breite Strahlen
      M5.Lcd.drawLine(cx + c * r * 0.68f + d * s * 0.6f, cy + s * r * 0.68f - d * c * 0.6f,
                      cx + c * r * 0.98f + d * s * 0.6f, cy + s * r * 0.98f - d * c * 0.6f, TFT_YELLOW);
    }
  }
}

static void iconMond(int cx, int cy, int r) {
  M5.Lcd.fillCircle(cx, cy, r * 6 / 10, 0xFFD6);                     // hellgelb
  M5.Lcd.fillCircle(cx + r * 3 / 10, cy - r * 2 / 10, r * 5 / 10, W_FELD);
}

static void iconWolke(int cx, int cy, int r, uint16_t farbe) {
  M5.Lcd.fillCircle(cx - r * 45 / 100, cy + r * 10 / 100, r * 35 / 100, farbe);
  M5.Lcd.fillCircle(cx + r * 5 / 100,  cy - r * 15 / 100, r * 48 / 100, farbe);
  M5.Lcd.fillCircle(cx + r * 50 / 100, cy + r * 12 / 100, r * 33 / 100, farbe);
  M5.Lcd.fillRoundRect(cx - r * 45 / 100, cy + r * 5 / 100, r * 95 / 100, r * 40 / 100, r / 6, farbe);
}

// Wetter-Symbol nach WMO-Code, zentriert auf (cx, cy), Größe r ≈ halbe Breite
static void wetterIcon(int code, bool tag, int cx, int cy, int r) {
  const int wy = cy - r * 25 / 100;                        // Wolke über Niederschlag
  if (code == 0) {
    if (tag) iconSonne(cx, cy, r); else iconMond(cx, cy, r);
  } else if (code <= 2) {                                  // heiter / wolkig
    if (tag) iconSonne(cx - r * 30 / 100, cy - r * 30 / 100, r * 70 / 100);
    else     iconMond(cx - r * 30 / 100, cy - r * 30 / 100, r * 70 / 100);
    iconWolke(cx + r * 15 / 100, cy + r * 15 / 100, r * (code == 1 ? 65 : 85) / 100, TFT_LIGHTGREY);
  } else if (code == 3) {                                  // bedeckt
    iconWolke(cx - r * 15 / 100, cy - r * 15 / 100, r * 75 / 100, TFT_DARKGREY);
    iconWolke(cx + r * 10 / 100, cy + r * 10 / 100, r * 90 / 100, TFT_LIGHTGREY);
  } else if (code == 45 || code == 48) {                   // Nebel
    for (int i = 0; i < 4; i++) {
      const int y = cy - r * 45 / 100 + i * r * 30 / 100;
      const int off = (i % 2) ? r / 5 : 0;
      M5.Lcd.fillRoundRect(cx - r * 85 / 100 + off, y, r * 150 / 100, 3, 1, TFT_LIGHTGREY);
    }
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {   // Schnee
    iconWolke(cx, wy, r, TFT_LIGHTGREY);
    for (int i = -1; i <= 1; i++) {
      const int x = cx + i * r * 40 / 100, y = cy + r * 55 / 100 + (i == 0 ? r / 6 : 0);
      M5.Lcd.drawLine(x - 3, y, x + 3, y, TFT_WHITE);
      M5.Lcd.drawLine(x, y - 3, x, y + 3, TFT_WHITE);
      M5.Lcd.drawLine(x - 2, y - 2, x + 2, y + 2, TFT_WHITE);
      M5.Lcd.drawLine(x - 2, y + 2, x + 2, y - 2, TFT_WHITE);
    }
  } else if (code >= 95) {                                 // Gewitter
    iconWolke(cx, wy, r, TFT_DARKGREY);
    const int x = cx, y = cy + r * 15 / 100;
    M5.Lcd.fillTriangle(x + r / 5, y, x - r / 5, y + r * 35 / 100, x + r / 20, y + r * 35 / 100, TFT_YELLOW);
    M5.Lcd.fillTriangle(x + r / 10, y + r * 30 / 100, x - r / 10, y + r * 30 / 100, x - r / 4, y + r * 80 / 100, TFT_YELLOW);
  } else if (code >= 51) {                                 // Niesel, Regen, Schauer
    const bool niesel = code <= 57;
    if (code >= 80 && tag) iconSonne(cx - r * 40 / 100, cy - r * 45 / 100, r * 55 / 100);
    iconWolke(cx, wy, r, code >= 65 && code <= 67 ? TFT_DARKGREY : TFT_LIGHTGREY);
    for (int i = -1; i <= 1; i++) {
      const int x = cx + i * r * 38 / 100, y = cy + r * 40 / 100;
      if (niesel) {
        M5.Lcd.fillCircle(x, y + r / 6, 2, W_REGEN);
      } else {
        M5.Lcd.drawLine(x + 3, y, x - 2, y + r * 45 / 100, W_REGEN);
        M5.Lcd.drawLine(x + 4, y, x - 1, y + r * 45 / 100, W_REGEN);
      }
    }
  } else {                                                 // unbekannt
    iconWolke(cx, cy, r, TFT_LIGHTGREY);
  }
}

static const char* wetterText(int code) {
  switch (code) {
    case 0:  return "Klar";
    case 1:  return "Heiter";
    case 2:  return "Wolkig";
    case 3:  return "Bedeckt";
    case 45: case 48: return "Nebel";
    case 51: case 53: case 55: return "Nieselregen";
    case 56: case 57: return "Gefr. Niesel";
    case 61: return "Leichter Regen";
    case 63: return "Regen";
    case 65: return "Starkregen";
    case 66: case 67: return "Glatteisregen";
    case 71: return "Leichter Schnee";
    case 73: return "Schnee";
    case 75: return "Starker Schnee";
    case 77: return "Schneegriesel";
    case 80: case 81: return "Regenschauer";
    case 82: return "Starke Schauer";
    case 85: case 86: return "Schneeschauer";
    case 95: return "Gewitter";
    case 96: case 99: return "Gewitter, Hagel";
    default: return "";
  }
}

// Ganzzahl mit kleinem Grad-Kreis rechts oben (die Schriften haben kein °)
static void gradText(int v, int x, int y, int font, uint16_t farbe, uint8_t ausr) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", v);
  const int wz = M5.Lcd.textWidth(buf, font) + 6;
  int x0 = x;                                              // ausr: 0 links, 1 Mitte, 2 rechts
  if (ausr == 1) x0 = x - wz / 2; else if (ausr == 2) x0 = x - wz;
  M5.Lcd.setTextColor(farbe);
  M5.Lcd.drawString(buf, x0, y, font);
  M5.Lcd.drawCircle(x0 + wz - 2, y + 3, 2, farbe);
}

// Text mit Einheit l/m² zentriert (die Schriften haben kein ², deshalb kleine hochgestellte 2)
static void lm2Text(const char* text, int cx, int y, int font, uint16_t farbe) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%s l/m", text);
  const int w  = M5.Lcd.textWidth(buf, font);
  const int x0 = cx - (w + 6) / 2;
  M5.Lcd.setTextColor(farbe);
  M5.Lcd.drawString(buf, x0, y, font);
  M5.Lcd.drawString("2", x0 + w + 1, font == 1 ? y - 3 : y - 1, 1);
}

static bool istSchnee(int code) {
  return (code >= 71 && code <= 77) || code == 85 || code == 86;
}

void drawWetter(bool force) {
  static bool warAktuell = false;
  const bool da = wet.t != 0;
  const bool aktuell = frisch(wet.t, WETTER_MAX_AGE_MS);
  if (!force && !wetterNeu && aktuell == warAktuell) return;
  if (feedbackUntil != 0 && !force) return;                // Tasten-Rückmeldung nicht übermalen
  wetterNeu = false;
  warAktuell = aktuell;
  const uint16_t farbe = aktuell ? TFT_WHITE : TFT_DARKGREY;
  char buf[24];

  // Jetzt
  wetterFeld(5, 5, 310, 72);
  M5.Lcd.setTextSize(1);
  if (!da) {
    M5.Lcd.setTextColor(TFT_LIGHTGREY);
    M5.Lcd.drawCentreString("Wetter: noch keine Daten", 160, 32, 2);
    for (int i = 0; i < 4; i++) wetterFeld(5 + i * 78, 82, 76, 58);
    for (int i = 0; i < 2; i++) wetterFeld(5 + i * 156, 145, 154, 90);
    return;
  }
  wetterIcon(wet.code, wet.tag, 45, 41, 24);
  fmtKomma(buf, sizeof(buf), wet.temp);
  drawWert(buf, true, 145, 14, 2, 2, farbe);
  M5.Lcd.setTextSize(1);
  if (wet.regen1h >= 0.1f) {                               // Niederschlag in der kommenden Stunde
    char mm[10];
    fmtKomma(mm, sizeof(mm), wet.regen1h);
    snprintf(buf, sizeof(buf), "%s %s", istSchnee(wet.code) ? "Schnee" : "Regen", mm);
    lm2Text(buf, 145, 52, 2, aktuell ? W_REGEN : TFT_DARKGREY);
  } else {
    M5.Lcd.setTextColor(aktuell ? TFT_LIGHTGREY : TFT_DARKGREY);
    M5.Lcd.drawCentreString(wetterText(wet.code), 145, 52, 2);
  }
  if (aktuell && wet.glStufe > 0) {                        // Glätte-Warnung statt Ort/Wind
    const uint16_t hg = wet.glStufe >= 2 ? TFT_RED : W_ORANGE;
    M5.Lcd.fillRoundRect(214, 9, 96, 64, 6, hg);
    if (wet.glStufe >= 3) M5.Lcd.drawRoundRect(215, 10, 94, 62, 5, TFT_WHITE);
    M5.Lcd.setTextColor(wet.glStufe >= 2 ? TFT_WHITE : TFT_BLACK);
    M5.Lcd.drawCentreString("GLAETTE!", 262, 11, 2);
    const int fArt = M5.Lcd.textWidth(wet.glArt, 2) <= 92 ? 2 : 1;
    M5.Lcd.drawCentreString(wet.glArt, 262, fArt == 2 ? 28 : 31, fArt);
    M5.Lcd.drawCentreString(wet.glZusatz, 262, 47, 1);
    M5.Lcd.drawCentreString(wet.glZeit, 262, 59, 1);
  } else {
    M5.Lcd.drawFastVLine(212, 14, 54, W_RAHMEN);
    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.drawCentreString("Melle", 263, 12, 2);
    snprintf(buf, sizeof(buf), "Wind %d km/h", wet.wind);
    M5.Lcd.setTextColor(farbe);
    M5.Lcd.drawCentreString(buf, 263, 33, 2);
    snprintf(buf, sizeof(buf), "Stand %s", wet.stand);
    M5.Lcd.setTextColor(aktuell ? TFT_LIGHTGREY : TFT_ORANGE);
    M5.Lcd.drawCentreString(buf, 263, 56, 1);
  }

  // Die nächsten Stunden
  for (int i = 0; i < 4; i++) {
    const int x = 5 + i * 78, y = 82, cx = x + 38;
    wetterFeld(x, y, 76, 58);
    if (i >= wet.nStunden) continue;
    const WetterStunde& h = wet.h[i];
    snprintf(buf, sizeof(buf), "%d Uhr", h.std);
    M5.Lcd.setTextColor(TFT_LIGHTGREY);
    M5.Lcd.drawCentreString(buf, cx, y + 4, 2);
    wetterIcon(h.code, h.tag, x + 19, y + 38, 12);
    gradText(lroundf(h.temp), x + 54, y + 24, 2, farbe, 1);
    if (h.mm >= 0.1f) {                                    // Menge seit dem vorigen Feld
      char mm[10];
      fmtKomma(mm, sizeof(mm), h.mm);
      lm2Text(mm, x + 53, y + 44, 1, W_REGEN);
    } else if (h.regenP >= 10) {
      snprintf(buf, sizeof(buf), "%d%%", h.regenP);
      M5.Lcd.setTextColor(W_REGEN);
      M5.Lcd.drawCentreString(buf, x + 54, y + 43, 1);
    }
  }

  // Heute und morgen
  for (int i = 0; i < 2; i++) {
    const int x = 5 + i * 156, y = 145, ix = x + 36, cx = x + 110;
    wetterFeld(x, y, 154, 90);
    if (i >= wet.nTage) continue;
    const WetterTag& d = wet.d[i];
    M5.Lcd.setTextColor(TFT_WHITE);
    M5.Lcd.drawCentreString(i == 0 ? "Heute" : "Morgen", ix, y + 5, 2);
    wetterIcon(d.code, true, ix, y + 50, 24);
    M5.Lcd.setTextColor(aktuell ? TFT_LIGHTGREY : TFT_DARKGREY);
    M5.Lcd.drawCentreString(wetterText(d.code), ix, y + 76, 1);
    // Höchst- / Tiefstwert groß, bei sehr breiten Zahlen kleiner
    char mx[8], mn[8];
    snprintf(mx, sizeof(mx), "%d", (int)lroundf(d.tmax));
    snprintf(mn, sizeof(mn), "%d", (int)lroundf(d.tmin));
    const int fT = (M5.Lcd.textWidth(mx, 4) + M5.Lcd.textWidth(mn, 4) + 26 <= 80) ? 4 : 2;
    const int wMx = M5.Lcd.textWidth(mx, fT) + 6, wSl = M5.Lcd.textWidth("/", fT) + 4;
    const int wMn = M5.Lcd.textWidth(mn, fT) + 6;
    const int x0 = cx - (wMx + wSl + wMn) / 2, yT = y + (fT == 4 ? 7 : 12);
    gradText(lroundf(d.tmax), x0, yT, fT, aktuell ? W_ORANGE : TFT_DARKGREY, 0);
    M5.Lcd.setTextColor(TFT_LIGHTGREY);
    M5.Lcd.drawString("/", x0 + wMx + 2, yT, fT);
    gradText(lroundf(d.tmin), x0 + wMx + wSl, yT, fT, aktuell ? TFT_CYAN : TFT_DARKGREY, 0);
    if (d.mm >= 0.1f) {
      char mm[10];
      fmtKomma(mm, sizeof(mm), d.mm);
      lm2Text(mm, cx, y + 42, 2, W_REGEN);
    } else {
      M5.Lcd.setTextColor(TFT_DARKGREY);
      M5.Lcd.drawCentreString("trocken", cx, y + 42, 2);
    }
    if (d.pv >= 0) {
      snprintf(buf, sizeof(buf), "PV %d kWh", (int)lroundf(d.pv));
      M5.Lcd.setTextColor(TFT_YELLOW);
      M5.Lcd.drawCentreString(buf, cx, y + 64, 2);
    }
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
  snprintf(buf, sizeof(buf), "W%lu(%u) M%lu/%lu R%lu:%s",
           (unsigned long)wifiAbbrueche, (unsigned)wifiGrund, (unsigned long)mqttAbbrueche,
           (unsigned long)mqttStill, (unsigned long)neustarts, rstText);
  diagZeile(y, "  Abbrueche", buf, (wifiAbbrueche || mqttAbbrueche || neustarts) ? TFT_ORANGE : TFT_WHITE); y += 17;

  // Letzte Störung
  if (lastFaultMillis) {
    dauerText(d2, sizeof(d2), alterS(lastFaultMillis));
    snprintf(buf, sizeof(buf), "vor %s: %s", d2, lastFault);
  } else {
    snprintf(buf, sizeof(buf), "keine");
  }
  diagZeile(y, "Letzte Stoer.", buf, lastFaultMillis ? TFT_ORANGE : TFT_GREEN);
}
