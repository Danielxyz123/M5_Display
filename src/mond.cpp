#include "app.h"

// =============================================================================
// Gute-Nacht-Mond (abends zwischendurch): schlafende Mondsichel,
// funkelnde Sterne, aufsteigende "Z"
// =============================================================================
#define MO_CX     150                 // Mondscheibe
#define MO_CY     95
#define MO_R      70
#define MO_SCH_X  185                 // Schattenkreis, schneidet die Sichel aus
#define MO_SCH_Y  75
#define MO_SCH_R  62
#define MO_GELB   0xFF9A
#define MO_WANGE  0xFB2C
#define MO_STERNE 22
#define MO_ZZZ    3
#define MO_FRAME_MS 90

static int16_t  stX[MO_STERNE], stY[MO_STERNE];
static uint32_t moStart = 0, moFrame = 0;
static int16_t  zX[MO_ZZZ], zY[MO_ZZZ];  // zuletzt gezeichnete Position je "Z" (-1 = keine)
static uint8_t  zFont[MO_ZZZ];

static bool imMond(int x, int y) {
  const int dx = x - MO_CX, dy = y - MO_CY;
  return dx * dx + dy * dy < (MO_R + 5) * (MO_R + 5);
}

static void stern(int i, uint16_t c) {
  M5.Lcd.drawPixel(stX[i], stY[i], c);
  if (i % 3 == 0) {                                          // jeder dritte Stern größer
    M5.Lcd.drawPixel(stX[i] - 1, stY[i], c); M5.Lcd.drawPixel(stX[i] + 1, stY[i], c);
    M5.Lcd.drawPixel(stX[i], stY[i] - 1, c); M5.Lcd.drawPixel(stX[i], stY[i] + 1, c);
  }
}

static const char* mondSpruch(uint16_t nr) {
  static char buf[40];
  switch (nr % 4) {
    case 1:
      if (frisch(wet.t, WETTER_MAX_AGE_MS) && wet.nTage > 1) {
        snprintf(buf, sizeof(buf), "Morgen %d bis %d Grad",
                 (int)lroundf(wet.d[1].tmin), (int)lroundf(wet.d[1].tmax));
        return buf;
      }
      break;
    case 2:
      if (frisch(w.tSoc, SLOW_MAX_AGE_MS)) {
        snprintf(buf, sizeof(buf), "Akku %d %% fuer die Nacht", w.soc);
        return buf;
      }
      break;
    case 3: return "Traeum was Schoenes!";
  }
  return "Schlaf gut!";
}

void drawMond(uint16_t nr) {
  moStart = moFrame = millis();
  M5.Lcd.fillScreen(TFT_NAVY);
  // Mondsichel
  M5.Lcd.fillCircle(MO_CX, MO_CY, MO_R, MO_GELB);
  M5.Lcd.fillCircle(MO_SCH_X, MO_SCH_Y, MO_SCH_R, TFT_NAVY);
  // schlafendes Auge (Bogen nach unten), Lächeln, Wange
  for (int dx = -9; dx <= 9; dx++)
    M5.Lcd.fillCircle(106 + dx, 76 + (81 - dx * dx) / 16, 2, TFT_BLACK);
  for (int dx = -10; dx <= 10; dx++)
    M5.Lcd.fillCircle(112 + dx, 110 + (100 - dx * dx) / 20, 2, TFT_BLACK);
  M5.Lcd.fillCircle(96, 96, 7, MO_WANGE);
  // Sterne außerhalb des Mondes
  for (int i = 0; i < MO_STERNE; i++) {
    do { stX[i] = random(6, 314); stY[i] = random(6, 180); } while (imMond(stX[i], stY[i]));
    stern(i, (i % 2) ? TFT_WHITE : TFT_LIGHTGREY);
  }
  for (int i = 0; i < MO_ZZZ; i++) zX[i] = -1;
  // Text
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(MO_GELB, TFT_NAVY);
  M5.Lcd.drawCentreString("Gute Nacht!", 160, 190, 4);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Lcd.drawCentreString(mondSpruch(nr), 160, 220, 2);
}

void animMond() {
  const uint32_t now = millis();
  if (now - moFrame < MO_FRAME_MS) return;
  moFrame = now;
  const uint32_t t = now - moStart;

  // Sterne funkeln: zwei zufällige Sterne kurz hell bzw. dunkel
  for (int k = 0; k < 2; k++) {
    const int i = random(MO_STERNE);
    stern(i, random(3) ? TFT_WHITE : 0x4208);
  }

  // drei "Z" steigen nacheinander rechts vom Mond auf und werden größer
  M5.Lcd.setTextSize(1);
  for (int i = 0; i < MO_ZZZ; i++) {
    if (zX[i] >= 0) {                                         // altes Z löschen
      M5.Lcd.setTextColor(TFT_NAVY);
      M5.Lcd.drawString("Z", zX[i], zY[i], zFont[i]);
    }
    const int32_t lauf = (int32_t)t - i * 700;
    if (lauf < 0) { zX[i] = -1; continue; }
    const float p = (lauf % 2100) / 2100.0f;                   // 0..1, wiederholt
    zX[i] = 178 + (int)(p * 90);
    zY[i] = 120 - (int)(p * 95);
    zFont[i] = p < 0.4f ? 2 : 4;
    M5.Lcd.setTextColor(p < 0.8f ? TFT_WHITE : TFT_LIGHTGREY);
    M5.Lcd.drawString("Z", zX[i], zY[i], zFont[i]);
  }
}
