#include "app.h"

// =============================================================================
// Uhrseite: große Uhrzeit, Wochentag und Datum, Außentemperatur
// =============================================================================
static const char* const WOCHENTAG[] = { "Sonntag", "Montag", "Dienstag", "Mittwoch",
                                         "Donnerstag", "Freitag", "Samstag" };
static const char* const MONAT[] = { "Januar", "Februar", "Maerz", "April", "Mai", "Juni", "Juli",
                                     "August", "September", "Oktober", "November", "Dezember" };

void drawUhr(bool force) {
  static int letzteMinute = -1;
  static bool letzteTemp = false;
  struct tm t;
  const bool zeit = getLocalTime(&t, 0);
  float temp = 0;
  const bool tempDa = aussenTemp(temp);
  if (!force && zeit && t.tm_min == letzteMinute && tempDa == letzteTemp) return;
  letzteMinute = zeit ? t.tm_min : -1;
  letzteTemp = tempDa;

  M5.Lcd.fillRect(4, 4, 312, 232, TFT_NAVY);
  M5.Lcd.setTextSize(1);
  if (!zeit) {
    M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_NAVY);
    M5.Lcd.drawCentreString("Uhrzeit fehlt noch", 160, 105, 4);
    return;
  }
  char buf[32];
  // Uhrzeit: 7-Segment-Schrift doppelt groß (96 px hoch)
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Lcd.setTextSize(2);
  M5.Lcd.drawCentreString(buf, 160, 22, 7);
  M5.Lcd.setTextSize(1);

  M5.Lcd.setTextColor(TFT_CYAN, TFT_NAVY);
  M5.Lcd.drawCentreString(WOCHENTAG[t.tm_wday], 160, 134, 4);
  snprintf(buf, sizeof(buf), "%d. %s %d", t.tm_mday, MONAT[t.tm_mon], t.tm_year + 1900);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Lcd.drawCentreString(buf, 160, 164, 4);

  if (tempDa) {
    char zahl[12];
    fmtKomma(zahl, sizeof(zahl), temp);
    M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_NAVY);
    M5.Lcd.drawCentreString("Aussen", 160, 198, 2);
    drawWert(zahl, true, 160, 212, 2, 1, temp <= 0 ? TFT_CYAN : TFT_WHITE);
  }
}
