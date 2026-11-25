#include "app.h"

// =============================================================================
// Zeichenhilfen für alle Seiten
// =============================================================================

void drawFieldShadow(int x, int y, int w, int h, int radius, uint16_t shadowColor) {
  M5.Lcd.fillRoundRect(x + 4, y + 4, w - 8, h - 8, radius - 4, shadowColor);
}

void drawFieldBorder(int x, int y, int w, int h, int radius, uint16_t borderColor) {
  M5.Lcd.drawRoundRect(x, y, w, h, radius, borderColor);
  M5.Lcd.drawRoundRect(x + 1, y + 1, w - 2, h - 2, radius - 1, borderColor);
  M5.Lcd.drawRoundRect(x + 2, y + 2, w - 4, h - 4, radius - 2, borderColor);
  M5.Lcd.drawRoundRect(x + 3, y + 3, w - 6, h - 6, radius - 3, borderColor);
}

void fillGradientRoundRect(int x, int y, int w, int h, int radius, uint16_t colorStart, uint16_t colorEnd) {
  if (h <= 0 || w <= 0 || radius <= 0) return;

  uint8_t rStart = (colorStart >> 11) & 0x1F;
  uint8_t gStart = (colorStart >> 5) & 0x3F;
  uint8_t bStart = colorStart & 0x1F;
  uint8_t rEnd = (colorEnd >> 11) & 0x1F;
  uint8_t gEnd = (colorEnd >> 5) & 0x3F;
  uint8_t bEnd = colorEnd & 0x1F;

  radius = max(radius - 4, 0);
  int arcWidths[11];
  for (int dy = 0; dy <= radius && dy < 11; dy++) {
    arcWidths[dy] = (int)sqrt((float)(radius * radius - dy * dy));
  }

  x += 4; y += 4; w -= 8; h -= 8;

  for (int i = 0; i < h; i++) {
    float ratio = (float)i / (h - 1.0);
    uint16_t gradColor = (uint16_t)(
      ((rStart + (uint8_t)((rEnd - rStart) * ratio + 0.5f)) << 11) |
      ((gStart + (uint8_t)((gEnd - gStart) * ratio + 0.5f)) << 5) |
      (bStart + (uint8_t)((bEnd - bStart) * ratio + 0.5f))
    );

    int lineStartX, lineLen;
    if (i < radius) {
      lineStartX = x + radius - arcWidths[i];
      lineLen = w - 2 * (radius - arcWidths[i]);
    } else if (i >= h - radius) {
      int dy = h - 1 - i;
      lineStartX = x + radius - arcWidths[dy];
      lineLen = w - 2 * (radius - arcWidths[dy]);
    } else {
      lineStartX = x;
      lineLen = w;
    }

    if (lineStartX < 0) { lineLen += lineStartX; lineStartX = 0; }
    if (lineLen > 0 && lineStartX + lineLen > 320) lineLen = 320 - lineStartX;
    if (lineLen > 0) {
      M5.Lcd.drawFastHLine(lineStartX, y + i, lineLen, gradColor);
    }
  }
}

// Zahl mit einer Nachkommastelle und deutschem Komma
void fmtKomma(char* buf, size_t len, float v) {
  snprintf(buf, len, "%.1f", v);
  for (char* p = buf; *p; p++) if (*p == '.') *p = ',';
}

// Wert zentriert ausgeben, optional mit Grad-Zeichen (die Schriften haben kein °)
void drawWert(const char* zahl, bool grad, int cx, int y, int font, int size, uint16_t color) {
  M5.Lcd.setTextColor(color);
  M5.Lcd.setTextSize(size);
  if (!grad) {
    M5.Lcd.drawCentreString(zahl, cx, y, font);
    return;
  }
  const int r   = 2 + size;                                  // Radius des Grad-Kreises
  const int wz  = M5.Lcd.textWidth(zahl, font);
  const int wc  = M5.Lcd.textWidth("C", font);
  const int gap = 2 * r + 6;
  const int x0  = cx - (wz + gap + wc) / 2;
  M5.Lcd.drawString(zahl, x0, y, font);
  M5.Lcd.drawCircle(x0 + wz + gap / 2, y + r + 2, r, color);
  M5.Lcd.drawCircle(x0 + wz + gap / 2, y + r + 2, r - 1, color);
  M5.Lcd.drawString("C", x0 + wz + gap, y, font);
}

// Doppelter Rahmen um den ganzen Bildschirm (auch für den Störungsalarm)
void drawSeitenRahmen(uint16_t aussen, uint16_t innen) {
  M5.Lcd.drawRoundRect(1, 1, 318, 238, 10, aussen);
  M5.Lcd.drawRoundRect(2, 2, 316, 236, 8, innen);
}
