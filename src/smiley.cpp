#include "app.h"

// =============================================================================
// Smiley-Einblendung: Steuerung (wann, Demo, MQTT-Befehl) und Morgen-Smiley
//   Stil nach Außentemperatur: <= 0 °C Mütze + Schnee, >= 25 °C Sonnenbrille.
//   Abends übernimmt der Gute-Nacht-Mond (mond.cpp).
// =============================================================================
#define SM_CX 160
#define SM_CY 92
#define SM_R  72
#define SM_GELB   0xFEA0
#define SM_RAND   0xFC60
#define SM_WANGE  0xFB2C
#define SM_ZUNGE  0xF8B2
#define SM_MUETZE 0xD8A7
#define SM_KALT_C   0.0f              // bis hierhin: Mütze und Schnee
#define SM_WARM_C   25.0f             // ab hier: Sonnenbrille
#define SM_FRAME_MS 70
#define SM_FLOCKEN  16

uint8_t  smileyDemo = 0;               // 0 aus, 1..4 = normal, kalt, heiß, Mond
uint8_t  smileyNach = SEITE_SOLAR;     // dorthin geht es nach dem Smiley weiter
uint32_t tSmiley = 0;
uint16_t smileyNr = 0;                 // wechselt Spruch und Zwinkern


enum SmileyStil { SM_NORMAL, SM_KALT, SM_HEISS, SM_MOND };
SmileyStil smStil = SM_NORMAL;
int8_t     smStilTest = -1;            // per Topic/Demo erzwungener Stil (ein Mal)
uint32_t   smStart = 0, smFrame = 0;
float      smPhase = 0;                // Drehung der Strahlen
int16_t    flX[SM_FLOCKEN], flY[SM_FLOCKEN];
uint8_t    smAugen = 0;                // 0 offen, 1 beide zu, 2 rechts zu

static bool istAbend() {
  int m;
  return minutenJetzt(m) && m >= MOND_VON_MIN && m < MOND_BIS_MIN;
}

static SmileyStil smileyStil() {
  if (smStilTest >= 0) { SmileyStil s = (SmileyStil)smStilTest; smStilTest = -1; return s; }
  if (istAbend()) return SM_MOND;
  float t;
  if (!aussenTemp(t)) return SM_NORMAL;
  if (t <= SM_KALT_C) return SM_KALT;
  if (t >= SM_WARM_C) return SM_HEISS;
  return SM_NORMAL;
}

static const char* smileySpruch() {
  struct tm t;
  const bool zeit = getLocalTime(&t, 0);
  if (smileyNr % 2 == 1) {
    if (smStil == SM_KALT)  return smileyNr % 4 == 1 ? "Brrr! Warm anziehen!" : "Muetze auf, es ist kalt!";
    if (smStil == SM_HEISS) return smileyNr % 4 == 1 ? "Heute wird's heiss!" : "Sonnencreme nicht vergessen!";
  }
  // Sonne angesagt? (WMO-Code 0..2 = klar bis leicht bewölkt)
  if (smileyNr % 3 == 0 && frisch(wet.t, WETTER_MAX_AGE_MS) && wet.nTage > 0 && wet.d[0].code <= 2) {
    return "Heute scheint die Sonne!";
  }
  if (zeit && smileyNr % 2 == 0) {
    switch (t.tm_wday) {
      case 1: return "Guter Start in die Woche!";
      case 3: return "Bergfest!";
      case 4: return "Fast geschafft!";
      case 5: return "Bald ist Wochenende!";
      case 6: case 0: return "Schoenes Wochenende!";
    }
  }
  static const char* const sprueche[] = {
    "Heute wird ein guter Tag!", "Du schaffst das!", "Laecheln steckt an!",
    "Kaffee schon gehabt?", "Du bist super!", "Hab einen tollen Tag!"
  };
  return sprueche[smileyNr % (sizeof(sprueche) / sizeof(sprueche[0]))];
}

// Sonnenstrahlen im Winkel "phase" in Farbe c (navy = löschen)
static void smStrahlen(float phase, uint16_t c) {
  const int innen = SM_R + 6, aussen = SM_R + (smStil == SM_HEISS ? 20 : 16);
  for (int i = 0; i < 12; i++) {
    const float a = i * PI / 6 + phase;
    for (int d = -1; d <= 1; d++) {
      const float b = a + d * 0.014f;
      int y1 = SM_CY + sinf(b) * innen, y2 = SM_CY + sinf(b) * aussen;
      M5.Lcd.drawLine(SM_CX + cosf(b) * innen, y1, SM_CX + cosf(b) * aussen, y2, c);
    }
  }
}

static void smAuge(int x, bool zu) {
  const int y = SM_CY - 22;
  M5.Lcd.fillRect(x - 12, y - 15, 25, 31, SM_GELB);          // Augenbereich löschen
  if (zu) {
    for (int dx = -10; dx <= 10; dx++)                          // geschlossen: fröhlicher Bogen
      M5.Lcd.fillCircle(x + dx, y + 4 - (100 - dx * dx) / 12, 2, TFT_BLACK);
  } else {
    M5.Lcd.fillEllipse(x, y, 8, 13, TFT_BLACK);
    M5.Lcd.fillCircle(x + 3, y - 5, 3, TFT_WHITE);
  }
}

static void smAugenZeichnen(uint8_t zustand) {
  if (smStil == SM_HEISS) return;                               // Sonnenbrille: Augen nicht sichtbar
  smAuge(SM_CX - 26, zustand == 1);
  smAuge(SM_CX + 26, zustand != 0);
}

static void smMuetze() {
  // Mützenkappe als oberes Kreissegment, Umschlag, Bommel
  const int oben = SM_CY - SM_R - 4, unten = SM_CY - 50;
  for (int y = oben; y <= unten; y++) {
    const int dy = y - SM_CY;
    const int hw = (int)sqrtf((float)(SM_R + 4) * (SM_R + 4) - dy * dy);
    M5.Lcd.drawFastHLine(SM_CX - hw, y, 2 * hw + 1, SM_MUETZE);
  }
  for (int x = SM_CX - 50; x <= SM_CX + 50; x += 14)            // Strickmuster
    M5.Lcd.drawFastVLine(x, oben + 10, unten - oben - 10, 0xB000);
  M5.Lcd.fillRoundRect(SM_CX - 64, SM_CY - 52, 128, 14, 6, TFT_WHITE);
  M5.Lcd.fillCircle(SM_CX, oben - 6, 10, TFT_WHITE);
}

static void smBrille() {
  const int y = SM_CY - 34;
  M5.Lcd.fillRoundRect(SM_CX - 50, y, 40, 26, 8, TFT_BLACK);
  M5.Lcd.fillRoundRect(SM_CX + 10, y, 40, 26, 8, TFT_BLACK);
  M5.Lcd.fillRect(SM_CX - 10, y + 6, 20, 4, TFT_BLACK);        // Steg
  M5.Lcd.drawLine(SM_CX - 50, y + 6, SM_CX - SM_R + 12, y - 2, TFT_BLACK);
  M5.Lcd.drawLine(SM_CX + 50, y + 6, SM_CX + SM_R - 12, y - 2, TFT_BLACK);
  M5.Lcd.fillRect(SM_CX - 42, y + 5, 10, 4, TFT_WHITE);        // Glanz
  M5.Lcd.fillRect(SM_CX + 18, y + 5, 10, 4, TFT_WHITE);
}

// Schneeflocke nur außerhalb des Gesichts und oberhalb des Textes
static bool flockeFrei(int x, int y) {
  const int dx = x - SM_CX, dy = y - SM_CY;
  return y > 2 && y < 184 && x > 2 && x < 317 && dx * dx + dy * dy > (SM_R + 18) * (SM_R + 18);
}

static void smFlocke(int x, int y, uint16_t c) {
  if (!flockeFrei(x, y)) return;
  M5.Lcd.drawFastHLine(x - 2, y, 5, c);
  M5.Lcd.drawFastVLine(x, y - 2, 5, c);
}

void drawSmiley() {
  smStil = smileyStil();
  if (smStil == SM_MOND) { drawMond(smileyNr); return; }
  smStart = smFrame = millis();
  smPhase = 0;
  smAugen = 0;
  M5.Lcd.fillScreen(TFT_NAVY);
  if (smStil != SM_KALT) smStrahlen(smPhase, smStil == SM_HEISS ? SM_RAND : SM_GELB);
  else for (int i = 0; i < SM_FLOCKEN; i++) { flX[i] = random(4, 316); flY[i] = random(4, 184); }
  M5.Lcd.fillCircle(SM_CX, SM_CY, SM_R, SM_RAND);
  M5.Lcd.fillCircle(SM_CX, SM_CY, SM_R - 4, SM_GELB);
  // Lachender offener Mund: untere Kreishälfte, Zähne, Zunge
  M5.Lcd.fillCircle(SM_CX, SM_CY + 10, 40, TFT_BLACK);
  M5.Lcd.fillRect(SM_CX - 44, SM_CY - 34, 88, 44, SM_GELB);   // obere Hälfte wieder abdecken
  M5.Lcd.fillRect(SM_CX - 38, SM_CY + 10, 76, 6, TFT_WHITE);  // Zähne
  M5.Lcd.fillEllipse(SM_CX, SM_CY + 40, 20, 8, SM_ZUNGE);
  M5.Lcd.fillCircle(SM_CX - 52, SM_CY + 12, 10, SM_WANGE);
  M5.Lcd.fillCircle(SM_CX + 52, SM_CY + 12, 10, SM_WANGE);
  if (smStil == SM_HEISS) smBrille(); else smAugenZeichnen(0);
  if (smStil == SM_KALT) smMuetze();
  // Text
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(SM_GELB, TFT_NAVY);
  M5.Lcd.drawCentreString("Guten Morgen!", 160, 190, 4);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Lcd.drawCentreString(smileySpruch(), 160, 220, 2);
}

// Aus loop() aufgerufen, solange der Smiley zu sehen ist
void animSmiley() {
  if (smStil == SM_MOND) { animMond(); return; }
  const uint32_t now = millis();
  if (now - smFrame < SM_FRAME_MS) return;
  smFrame = now;
  const uint32_t t = now - smStart;

  if (smStil == SM_KALT) {                                      // Schnee rieselt
    for (int i = 0; i < SM_FLOCKEN; i++) {
      smFlocke(flX[i], flY[i], TFT_NAVY);
      flY[i] += 2 + (i % 3);
      flX[i] += (i % 2) ? 1 : -1;
      if (flY[i] > 184) { flY[i] = 4; flX[i] = random(4, 316); }
      smFlocke(flX[i], flY[i], TFT_WHITE);
    }
  } else {                                                      // Strahlen drehen sich
    smStrahlen(smPhase, TFT_NAVY);
    smPhase += 0.04f;
    smStrahlen(smPhase, smStil == SM_HEISS ? SM_RAND : SM_GELB);
  }

  // Blinzeln bei 1,2 s, Zwinkern (rechts) bei 3,4 s
  uint8_t soll = 0;
  if (t >= 1200 && t < 1350) soll = 1;
  else if (t >= 3400 && t < 3900) soll = 2;
  if (soll != smAugen) { smAugen = soll; smAugenZeichnen(soll); }
}

// Demo: alle Stile nacheinander (5 schnelle Klicks auf A oder C)
static void zeigeDemoSchritt() {
  static const SmileyStil folge[] = { SM_NORMAL, SM_KALT, SM_HEISS, SM_MOND };
  smStilTest = folge[smileyDemo - 1];
  page = SEITE_SMILEY;
  smileyNr++;
  forceRedraw = true;
}

void starteSmileyDemo() {
  smileyNach = SEITE_SOLAR;
  smileyDemo = 1;
  lastPageSwitch = millis();
  zeigeDemoSchritt();
}

// nach Ablauf eines Smileys: nächster Demo-Schritt? (false = Demo vorbei / keine Demo)
bool smileyDemoWeiter() {
  if (smileyDemo == 0) return false;
  if (smileyDemo >= 4) { smileyDemo = 0; return false; }
  smileyDemo++;
  zeigeDemoSchritt();
  return true;
}

// Befehl über MQTT: "kalt"/"heiss"/"normal"/"mond" = Stil erzwingen, sonst Stil nach Uhrzeit/Temperatur
void smileyBefehl(const byte* p, unsigned int len) {
  char m[8] = {0};
  memcpy(m, p, len < sizeof(m) - 1 ? len : sizeof(m) - 1);
  if      (!strcmp(m, "kalt"))   smStilTest = SM_KALT;
  else if (!strcmp(m, "heiss"))  smStilTest = SM_HEISS;
  else if (!strcmp(m, "normal")) smStilTest = SM_NORMAL;
  else if (!strcmp(m, "mond"))   smStilTest = SM_MOND;
  zeigeSmileyJetzt();
}

void zeigeSmileyJetzt() {
  if (page == SEITE_SMILEY) return;
  smileyNach = page < AUTO_SEITEN ? page : SEITE_SOLAR;
  page = SEITE_SMILEY;
  lastPageSwitch = millis();
  smileyNr++;
  forceRedraw = true;
}

// Darf jetzt eingeblendet werden? (morgens Smiley, abends Mond)
bool smileyZeit() {
  int m;
  return minutenJetzt(m) && ((m >= SMILEY_VON_MIN && m < SMILEY_BIS_MIN) || istAbend());
}
