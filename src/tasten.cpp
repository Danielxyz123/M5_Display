#include "app.h"

// =============================================================================
// Tasten
// =============================================================================
static void zeigeFeedback(const char* text, uint16_t color, uint8_t font) {
  if (page >= AUTO_SEITEN) return;                       // nur Solar, Heizung, Wetter
  M5.Lcd.fillRect(260, 8, 52, 28, TFT_BLACK);
  M5.Lcd.setTextColor(color);
  M5.Lcd.setTextSize(1);
  M5.Lcd.drawCentreString(text, 286, font == 4 ? 10 : 12, font);
  feedbackUntil = millis() + FEEDBACK_MS;
}

bool diagFrei = false;                 // Verlauf/Diagnose per langem B freigegeben

static void lampeDimmen(bool heller) {
  mqttClient.publish(LAMP_TOPIC_BRIGHTNESS, heller ? "+10" : "-10", false);
  zeigeFeedback(heller ? "+" : "-", TFT_WHITE, 4);
}

// Seitentaste (A oder C): Klick = blättern, Halten = Lampe dimmen (wiederholt)
struct SeitenTaste {
  bool     gedimmt = false;            // in diesem Druck schon gedimmt → kein Klick mehr
  uint32_t naechstesDimmen = 0;
  uint8_t  klicks = 0;                 // schnelle Klickserie
  uint32_t letzterKlick = 0;
};

static void seitenTaste(Button& taste, SeitenTaste& st, int schritt, bool heller, bool gesperrt) {
  const uint32_t now = millis();
  if (taste.wasPressed()) {
    st.gedimmt = false;
    st.naechstesDimmen = now + KLICK_MAX_MS;
  }
  if (taste.isPressed() && !gesperrt && (int32_t)(now - st.naechstesDimmen) >= 0) {
    lampeDimmen(heller);
    st.gedimmt = true;
    st.naechstesDimmen = now + DIMM_INTERVALL_MS;
  }
  if (taste.wasReleased()) {
    if (!st.gedimmt && !gesperrt) {
      seiteWechseln(schritt);
      // 5 schnelle Klicks hintereinander → alle Smileys nacheinander zeigen
      st.klicks = (now - st.letzterKlick < SERIEN_KLICK_MS) ? st.klicks + 1 : 1;
      st.letzterKlick = now;
      if (st.klicks >= 5) { st.klicks = 0; starteSmileyDemo(); }
    }
    st.gedimmt = false;
  }
}

void handleButtons() {
  static SeitenTaste tA, tC;
  static bool langB = false;
  static bool kombi = false;           // A und C zusammen gedrückt → kein Klick, kein Dimmen
  static bool kombiErledigt = false;

  if (M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed()) {
    lastButton = millis();
  }

  // A + C zusammen: 3 s halten = Display dimmen
  if (M5.BtnA.isPressed() && M5.BtnC.isPressed()) {
    kombi = true;
    if (!kombiErledigt && M5.BtnA.pressedFor(LONG_PRESS_MS) && M5.BtnC.pressedFor(LONG_PRESS_MS)) {
      kombiErledigt = true;
      dimmed = !dimmed;                                   // Helligkeit setzt handleHelligkeit()
    }
  }

  // A (links): Klick = Seite zurück, halten = Lampe dunkler
  seitenTaste(M5.BtnA, tA, -1, false, kombi);
  // C (rechts): Klick = Seite vor, halten = Lampe heller
  seitenTaste(M5.BtnC, tC, +1, true, kombi);
  if (!M5.BtnA.isPressed() && !M5.BtnC.isPressed()) { kombi = false; kombiErledigt = false; }

  // B (Mitte): Klick = Lampe an/aus, 1,5 s halten = Verlauf/Diagnose freigeben und zur Diagnose;
  // auf der Diagnoseseite schaltet langes B den Testmodus
  if (!langB && M5.BtnB.pressedFor(DIAG_PRESS_MS)) {
    langB = true;
    if (page == SEITE_DIAGNOSE) {
      testMode = !testMode;
      DEBUG_PRINTF("Testmodus: %s\n", testMode ? "AN" : "AUS");
    } else {
      diagFrei = true;
      page = SEITE_DIAGNOSE;
      lastPageSwitch = millis();
    }
    forceRedraw = true;
  }
  if (M5.BtnB.wasReleased()) {
    if (!langB) {
      lampOn = !lampOn;
      mqttClient.publish(LAMP_TOPIC_ONOFF, lampOn ? "ON" : "OFF", true);   // retained
      zeigeFeedback(lampOn ? "AN" : "AUS", lampOn ? TFT_GREEN : TFT_RED, 2);
    }
    langB = false;
  }
}

void handleTestMode() {
  static uint32_t testTime = 0;
  if (!testMode || millis() - testTime < 5000) return;
  testTime = millis();
  const uint32_t now = millis();
  w.gen  = random(0, 10000);   w.tGen  = now;
  w.grid = random(-500, 1000); w.tGrid = now;
  w.akku = random(-500, 500);  w.tAkku = now;
  w.soc  = random(20, 90);     w.tSoc  = now;
  w.autarkie = random(0, 100); w.tAutarkie = now;
}
