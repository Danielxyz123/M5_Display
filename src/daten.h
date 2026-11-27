#pragma once
// Empfangene Messwerte, Wetter, Verlauf und gemeinsamer Anzeigezustand

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
};

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
  int          warnStufe = 0;          // DWD-Wetterwarnung: 0 keine, 1 gelb, 2 orange, 3 rot, 4 violett
  char         warnArt[20] = "", warnZeit[20] = "";
};

extern Werte  w;
extern Wetter wet;
extern bool   wetterNeu;               // neue Wetterdaten → Seite neu zeichnen

extern FieldState fsPV, fsNetz, fsAkku, fsSoc, fsAut;
extern FieldState fsAussen, fsVorlauf, fsKessel, fsWW, fsBrenner;

extern int16_t  histGrid[HIST_POINTS], histGen[HIST_POINTS], histAkku[HIST_POINTS];
extern uint16_t histIdx;
extern uint32_t histLast;

// Anzeigezustand
extern uint8_t  page;
extern uint32_t lastPageSwitch;        // für den automatischen Wechsel
extern uint32_t lastButton;            // letzter Tastendruck
extern bool     forceRedraw;
extern bool     dimmed;                // per A+C gedimmt
extern bool     testMode;
extern bool     lampOn;
extern uint32_t feedbackUntil;

// Hilfsfunktionen
bool        frisch(uint32_t t, uint32_t maxAge);
uint32_t    alterS(uint32_t t);
bool        bridgeOk();
enum VenusZustand { VZ_UNBEKANNT, VZ_STOERUNG, VZ_RUHE, VZ_LAEDT, VZ_ENTLAEDT };
VenusZustand venusZustand(const char** grund);
const char* venusTextDe();
bool        aussenTemp(float& t);      // Heizung, sonst Wetter; false = keine aktuelle Temperatur
