#include "app.h"

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
