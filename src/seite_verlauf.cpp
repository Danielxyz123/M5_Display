#include "app.h"

// =============================================================================
// Verlauf
// =============================================================================
static int16_t clamp16(float v) {
  if (v > 32000) return 32000;
  if (v < -32000) return -32000;
  return (int16_t)lroundf(v);
}

void verlaufStart() {
  for (int i = 0; i < HIST_POINTS; i++) histGrid[i] = histGen[i] = histAkku[i] = HIST_INVALID;
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
