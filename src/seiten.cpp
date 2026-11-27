#include "app.h"

// =============================================================================
// Seiten: automatischer Wechsel, Blättern, Zeichnen der aktuellen Seite
// =============================================================================

// Automatischer Wechsel Solar → Heizung → Wetter → Uhr → Sonne & Mond, dazwischen Smiley/Mond.
// Nach einem Tastendruck bleibt die Seite MANUELL_HALTEN_MS stehen; danach endet die Freigabe
// von Verlauf/Diagnose und es geht zur Solarseite zurück.
void handleAutoWechsel() {
  const uint32_t now = millis();
  if (page == SEITE_SMILEY) {                             // Smiley endet auch bei Tastendruck nach Ablauf
    if (now - lastPageSwitch >= SMILEY_DAUER_MS && smileyDemoWeiter()) {
      lastPageSwitch = now;
      forceRedraw = true;
    } else if (now - lastPageSwitch >= SMILEY_DAUER_MS) {
      page = smileyNach;
      lastPageSwitch = now;
      forceRedraw = true;
    }
    return;
  }
  if (autoAngehalten()) return;                           // Wetterwarnung oder Störung: nur noch per Taste
  if (tAlarm != 0 && now - tAlarm < ALARM_HALTEN_MS) return;   // neue Störung: Diagnose stehen lassen
  if (lastButton != 0 && now - lastButton < MANUELL_HALTEN_MS) return;
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed() || feedbackUntil != 0) return;
  diagFrei = false;
  if (page < AUTO_SEITEN) {
    if (now - lastPageSwitch >= AUTO_WECHSEL_MS) {
      page = (page + 1) % AUTO_SEITEN;
      if ((tSmiley == 0 || now - tSmiley >= SMILEY_ABSTAND_MS) && smileyZeit()) {
        smileyNach = page;
        page = SEITE_SMILEY;
        tSmiley = now;
        smileyNr++;
      }
      lastPageSwitch = now;
      forceRedraw = true;
    }
  } else {
    page = SEITE_SOLAR;
    lastPageSwitch = now;
    forceRedraw = true;
  }
}

// Automatischer Wechsel ruht, solange eine Störung oder eine DWD-Wetterwarnung vorliegt
bool autoAngehalten() {
  return faultActive || (frisch(wet.t, WETTER_MAX_AGE_MS) && wet.warnStufe > 0);
}

void seiteWechseln(int schritt) {
  const int n = diagFrei ? SEITEN : AUTO_SEITEN;           // ohne Freigabe nur die automatischen Seiten
  if (page >= n) page = SEITE_SOLAR;
  page = (page + n + schritt) % n;
  smileyDemo = 0;                                          // manuelles Blättern beendet die Demo
  lastPageSwitch = millis();
  forceRedraw = true;
}

// =============================================================================
// Zeichnen
// =============================================================================
void drawPage() {
  const bool force = forceRedraw;
  forceRedraw = false;
  if (force) {
    M5.Lcd.fillScreen(TFT_NAVY);
    drawSeitenRahmen(TFT_LIGHTGREY, TFT_DARKGREY);
    fsPV.text[0] = fsNetz.text[0] = fsAkku.text[0] = fsSoc.text[0] = fsAut.text[0] = '\0';
    fsAussen.text[0] = fsVorlauf.text[0] = fsKessel.text[0] = fsWW.text[0] = fsBrenner.text[0] = '\0';
  }
  switch (page) {
    case SEITE_SOLAR:    drawOverview(force); break;
    case SEITE_HEIZUNG:  drawHeizung(); break;
    case SEITE_WETTER:   drawWetter(force); break;
    case SEITE_UHR:      drawUhr(force); break;
    case SEITE_ASTRO:    drawAstro(force); break;
    case SEITE_VERLAUF:  if (force) drawHistory(); break;
    case SEITE_DIAGNOSE: drawDiagnose(); break;
    case SEITE_SMILEY:   if (force) drawSmiley(); break;
  }
}
