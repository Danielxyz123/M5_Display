#include "app.h"

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
  if (aktuell && wet.warnStufe > 0) {                      // DWD-Wetterwarnung hat Vorrang vor Glätte
    const uint16_t hg = wet.warnStufe >= 4 ? TFT_MAGENTA : wet.warnStufe == 3 ? TFT_RED : W_ORANGE;
    M5.Lcd.fillRoundRect(214, 9, 96, 64, 6, hg);
    M5.Lcd.drawRoundRect(215, 10, 94, 62, 5, TFT_WHITE);
    M5.Lcd.setTextColor(wet.warnStufe >= 3 ? TFT_WHITE : TFT_BLACK);
    M5.Lcd.drawCentreString("WARNUNG!", 262, 11, 2);
    const int fArt = M5.Lcd.textWidth(wet.warnArt, 2) <= 92 ? 2 : 1;
    M5.Lcd.drawCentreString(wet.warnArt, 262, fArt == 2 ? 29 : 32, fArt);
    M5.Lcd.drawCentreString("DWD", 262, 47, 1);
    M5.Lcd.drawCentreString(wet.warnZeit, 262, 59, 1);
  } else if (aktuell && wet.glStufe > 0) {                 // Glätte-Warnung statt Ort/Wind
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
