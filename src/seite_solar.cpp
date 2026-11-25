#include "app.h"

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
