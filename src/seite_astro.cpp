#include "app.h"

// =============================================================================
// Seite "Sonne & Mond": Sonnenbogen mit Sonne jetzt, Auf-/Untergang, Tageslänge,
// Mondscheibe mit aktueller Phase, Beleuchtung, nächster Voll-/Neumond.
// Rechnet alles auf dem Gerät (astro.cpp); neu gezeichnet wird jede Minute.
// =============================================================================
#define AS_FELD   0x18E3                  // wie die Wetterseite
#define AS_RAHMEN TFT_DARKCYAN
#define AS_SONNE  TFT_YELLOW
#define AS_HELL   0xFFDA                  // Mond, hell
#define AS_DUNKEL 0x31A6                  // Mond, Schattenseite

static void feld(int x, int y, int w, int h) {
  M5.Lcd.fillRoundRect(x, y, w, h, 8, AS_FELD);
  M5.Lcd.drawRoundRect(x, y, w, h, 8, AS_RAHMEN);
  M5.Lcd.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 7, AS_RAHMEN);
}

static void hhmm(char* buf, size_t n, time_t t) {
  if (!t) { snprintf(buf, n, "--:--"); return; }
  struct tm tm; localtime_r(&t, &tm);
  snprintf(buf, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

// Mondscheibe zeilenweise: hell ist bei zunehmendem Mond die rechte Seite (Nordhalbkugel)
static void mondScheibe(int cx, int cy, int r, float b, bool zunehmend) {
  M5.Lcd.fillCircle(cx, cy, r, AS_DUNKEL);
  for (int y = -r; y <= r; y++) {
    const float w = sqrtf((float)(r * r - y * y));
    const float grenze = w * (1.0f - 2.0f * b);                // Schattengrenze (Ellipse)
    int x0 = (int)lroundf(zunehmend ? grenze : -w), x1 = (int)lroundf(zunehmend ? w : -grenze);
    if (x1 > x0) M5.Lcd.drawFastHLine(cx + x0, cy + y, x1 - x0 + 1, AS_HELL);
  }
  M5.Lcd.drawCircle(cx, cy, r, TFT_DARKGREY);
}

// Halbbogen der Sonne vom Aufgang (links) zum Untergang (rechts)
static void sonnenBogen(int cx, int cy, int r, const SonneTag& s, time_t jetzt) {
  for (int g = 0; g <= 180; g += 6) {                         // gestrichelter Bogen
    const float a = g * DEG_TO_RAD;
    M5.Lcd.fillCircle(cx + (int)(r * cosf(a)), cy - (int)(r * sinf(a)), 1, TFT_LIGHTGREY);
  }
  M5.Lcd.drawFastHLine(cx - r - 10, cy, 2 * r + 20, TFT_DARKGREY);
  if (s.auf && s.unter && jetzt > s.auf && jetzt < s.unter) {
    const float a = PI * (1.0f - (float)(jetzt - s.auf) / (float)(s.unter - s.auf));
    const int x = cx + (int)(r * cosf(a)), y = cy - (int)(r * sinf(a));
    M5.Lcd.fillCircle(x, y, 9, AS_SONNE);
    M5.Lcd.drawCircle(x, y, 11, 0xFE60);
  }
}

void drawAstro(bool force) {
  static int letzteMinute = -1;
  struct tm t;
  if (!getLocalTime(&t, 0)) {
    if (force) { M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_NAVY); M5.Lcd.drawCentreString("Uhrzeit fehlt noch", 160, 105, 4); }
    return;
  }
  if (!force && t.tm_min == letzteMinute) return;
  letzteMinute = t.tm_min;
  const time_t jetzt = time(nullptr);
  const SonneTag s = sonneBerechnen(jetzt), g = sonneBerechnen(jetzt - 86400);
  const MondInfo m = mondBerechnen(jetzt);
  char buf[40], a[8], u[8];
  M5.Lcd.setTextSize(1);

  // ---- Sonne ----
  feld(5, 5, 310, 112);
  M5.Lcd.setTextColor(TFT_WHITE);
  M5.Lcd.drawString("Sonne", 16, 12, 2);
  sonnenBogen(160, 92, 62, s, jetzt);
  hhmm(a, sizeof(a), s.auf); hhmm(u, sizeof(u), s.unter);
  M5.Lcd.setTextColor(AS_SONNE);
  M5.Lcd.drawString("auf", 16, 70, 2);   M5.Lcd.drawString(a, 16, 86, 4);
  M5.Lcd.drawRightString("unter", 304, 70, 2); M5.Lcd.drawRightString(u, 304, 86, 4);
  if (s.laengeMin >= 0) {
    const int d = (g.laengeMin >= 0) ? s.laengeMin - g.laengeMin : 0;
    snprintf(buf, sizeof(buf), "Tag %d:%02d h (%c%d min)", s.laengeMin / 60, s.laengeMin % 60, d >= 0 ? '+' : '-', abs(d));
    M5.Lcd.setTextColor(TFT_LIGHTGREY);
    M5.Lcd.drawRightString(buf, 304, 12, 2);
  }

  // ---- Mond ----
  feld(5, 122, 310, 113);
  mondScheibe(62, 178, 44, m.beleuchtung, m.zunehmend);
  M5.Lcd.setTextColor(TFT_WHITE);
  M5.Lcd.drawString("Mond", 122, 130, 2);
  M5.Lcd.setTextColor(AS_HELL);
  M5.Lcd.drawString(m.name, 122, 150, 2);
  snprintf(buf, sizeof(buf), "%d %% beleuchtet", (int)lroundf(m.beleuchtung * 100));
  M5.Lcd.setTextColor(TFT_LIGHTGREY);
  M5.Lcd.drawString(buf, 122, 170, 2);
  const time_t voll = naechsterMond(jetzt, true), neu = naechsterMond(jetzt, false);
  struct tm tv, tn; localtime_r(&voll, &tv); localtime_r(&neu, &tn);
  snprintf(buf, sizeof(buf), "Vollmond %d.%d. %02d:%02d", tv.tm_mday, tv.tm_mon + 1, tv.tm_hour, tv.tm_min);
  M5.Lcd.setTextColor(voll < neu ? TFT_WHITE : TFT_LIGHTGREY);
  M5.Lcd.drawString(buf, 122, 192, 2);
  snprintf(buf, sizeof(buf), "Neumond  %d.%d. %02d:%02d", tn.tm_mday, tn.tm_mon + 1, tn.tm_hour, tn.tm_min);
  M5.Lcd.setTextColor(neu < voll ? TFT_WHITE : TFT_LIGHTGREY);
  M5.Lcd.drawString(buf, 122, 210, 2);
}
