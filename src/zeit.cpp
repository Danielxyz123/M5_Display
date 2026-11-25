#include "app.h"

// =============================================================================
// Zeit: Uhrzeit per NTP, Nachtmodus und Helligkeit
// =============================================================================
void zeitStart() {
  configTzTime(ZEITZONE, "de.pool.ntp.org", "pool.ntp.org");
}

bool minutenJetzt(int& minuten) {
  struct tm t;
  if (!getLocalTime(&t, 0)) return false;                 // noch keine Uhrzeit vom NTP
  minuten = t.tm_hour * 60 + t.tm_min;
  return true;
}

bool istNacht() {
  int m;
  return minutenJetzt(m) && (m >= NACHT_VON_MIN || m < NACHT_BIS_MIN);
}

// Helligkeit: A+C-Dimmen > Nacht (außer kurz nach Tastendruck) > normal
void handleHelligkeit() {
  static uint8_t ist = BRIGHT_NORMAL;
  uint8_t soll = BRIGHT_NORMAL;
  const bool wach = lastButton != 0 && millis() - lastButton < NACHT_WACH_MS;
  if (dimmed)                soll = BRIGHT_DIM;
  else if (istNacht() && !wach) soll = BRIGHT_NACHT;
  if (soll != ist) {
    ist = soll;
    M5.Lcd.setBrightness(soll);
  }
}
