#include "app.h"

// =============================================================================
// Daten: Messwerte, Wetter, Verlauf, Anzeigezustand
// =============================================================================
Werte  w;
Wetter wet;
bool   wetterNeu = false;
Meldungen mld;

FieldState fsPV = {}, fsNetz = {}, fsAkku = {}, fsSoc = {}, fsAut = {};
FieldState fsAussen = {}, fsVorlauf = {}, fsKessel = {}, fsWW = {}, fsBrenner = {};

int16_t  histGrid[HIST_POINTS], histGen[HIST_POINTS], histAkku[HIST_POINTS];
uint16_t histIdx = 0;
uint32_t histLast = 0;

uint8_t  page = SEITE_SOLAR;
uint32_t lastPageSwitch = 0;
uint32_t lastButton = 0;
bool     forceRedraw = true;
bool     dimmed = false;
bool     testMode = false;
bool     lampOn = false;
uint32_t feedbackUntil = 0;

// =============================================================================
// Hilfsfunktionen
// =============================================================================
bool frisch(uint32_t t, uint32_t maxAge) {
  return t != 0 && (millis() - t) < maxAge;
}

uint32_t alterS(uint32_t t) {
  return t == 0 ? 0 : (millis() - t) / 1000;
}

bool bridgeOk() {
  return frisch(w.tHb, BRIDGE_MAX_AGE_MS) && w.gridValid && frisch(w.tGrid, BRIDGE_MAX_AGE_MS);
}

VenusZustand venusZustand(const char** grund) {
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
const char* venusTextDe() {
  if (strstr(w.venusText, "opladen met"))   return "laedt";
  if (strstr(w.venusText, "ontladen met"))  return "entlaedt";
  if (strstr(w.venusText, "Te veel"))       return "Speicher voll";
  if (strstr(w.venusText, "Te weinig"))     return "Speicher leer";
  if (strstr(w.venusText, "kein Sollwert")) return "kein Sollwert!";
  if (strstr(w.venusText, "Stand-by"))      return "Stand-by";
  if (strstr(w.venusText, "minder dan"))    return "unter Mindestleistung";
  return w.venusText[0] ? w.venusText : "-";
}

bool aussenTemp(float& t) {
  if (frisch(w.tHeiz, HEIZ_MAX_AGE_MS)) { t = w.tAussen; return true; }
  if (frisch(wet.t, WETTER_MAX_AGE_MS)) { t = wet.temp; return true; }
  return false;
}
