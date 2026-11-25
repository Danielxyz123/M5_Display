#include "app.h"

// =============================================================================
// Störungen: erkennen, für die Diagnose merken, Alarm anzeigen
//   neue Störung → einmal zur Diagnose springen (bleibt ALARM_HALTEN_MS stehen)
//   solange aktiv → Bildschirmrahmen blinkt rot
// =============================================================================
char     lastFault[64] = "";
uint32_t lastFaultMillis = 0;
bool     faultActive = false;
uint32_t tAlarm = 0;

void pruefeStoerung() {
  const char* grund = "";
  bool stoerung = false;
  char text[64] = "";
  if (w.tHb != 0 && !bridgeOk()) {
    stoerung = true;
    snprintf(text, sizeof(text), "Zaehlerwert veraltet");
  }
  if (w.tHeiz != 0 && !frisch(w.tHeiz, HEIZ_MAX_AGE_MS)) {
    stoerung = true;
    snprintf(text, sizeof(text), "Heizung sendet nicht");
  }
  if (venusZustand(&grund) == VZ_STOERUNG) {
    stoerung = true;
    snprintf(text, sizeof(text), "%s", grund);
  }
  if (stoerung && !faultActive) {
    strncpy(lastFault, text, sizeof(lastFault) - 1);
    lastFaultMillis = millis();
    DEBUG_PRINTF("Stoerung: %s\n", lastFault);
    if (millis() > ALARM_START_SPERRE_MS) {               // gleich nach dem Start fehlen Daten noch
      tAlarm = millis();
      diagFrei = true;
      smileyDemo = 0;
      page = SEITE_DIAGNOSE;
      lastPageSwitch = millis();
      forceRedraw = true;
    }
  }
  faultActive = stoerung;
}

// Rahmen blinken lassen; nach Ende der Störung einmal normalen Rahmen zeichnen
void handleAlarm() {
  static uint32_t tBlink = 0;
  static bool rot = false;
  const bool smiley = page == SEITE_SMILEY;
  if (!faultActive) {
    if (rot) {
      rot = false;
      if (!smiley) drawSeitenRahmen(TFT_LIGHTGREY, TFT_DARKGREY);
    }
    return;
  }
  if (forceRedraw || millis() - tBlink < ALARM_BLINK_MS) return;
  tBlink = millis();
  rot = !rot;
  if (rot)         drawSeitenRahmen(TFT_RED, TFT_RED);
  else if (smiley) drawSeitenRahmen(TFT_NAVY, TFT_NAVY);
  else             drawSeitenRahmen(TFT_LIGHTGREY, TFT_DARKGREY);
}
