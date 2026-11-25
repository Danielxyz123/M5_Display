// =============================================================================
// M5Stack-Display für PV / Netz / Venus-Speicher / Heizung / Wetter
// =============================================================================
// Seiten (A/C blaettern; Verlauf und Diagnose erst nach langem Druck auf B):
//   1. Übersicht: PV (Leistung und Tagesertrag), Netz, Akku, Ladestand, Autarkie
//      Rahmenfarbe Akku: grün = lädt, orange = entlädt, grau = Stand-by/voll,
//                        rot = Störung am Venus (kein Sollwert, Modbus, keine Daten)
//      Grauer Wert = Daten nicht aktuell
//   2. Heizung: Außen- und Wohnzimmertemperatur, Vorlauf FBH, Kessel, Warmwasser,
//      Brenner heute (Laufzeit, Starts, Laufzeitanteil der letzten 24 h)
//   3. Wetter Melle: jetzt, die naechsten Stunden, heute und morgen mit Regen und PV-Prognose
//   4. Uhr: Uhrzeit, Datum, Außentemperatur
//      Seite 1 bis 4 wechseln automatisch alle 10 s
//   5. Verlauf der letzten 60 min (Netz, PV, Akku)
//   6. Diagnose (Bridge, Regelung, Venus-Controller, Display)
//
// Einblendungen: morgens 6-10 Uhr Smiley (Stil nach Außentemperatur), abends 21-23 Uhr
//   Gute-Nacht-Mond; Topic M5Display/smiley (kalt/heiss/normal/mond) zeigt sofort.
// Nachtmodus 22-6 Uhr: Display dunkel, nach Tastendruck 30 s hell.
// Störung (Bridge, Venus, Heizung): Rahmen blinkt rot, einmal Sprung zur Diagnose.
//
// Tasten:  Klick (beim Loslassen):  A = Seite zurueck   B = Lampe an/aus   C = Seite vor
//          halten:  A = Lampe dunkler, C = Lampe heller (wiederholt, solange gehalten)
//                   B 1,5 s = Verlauf/Diagnose freigeben (springt zur Diagnose; dort: Testmodus)
//                   A + C 3 s = Display dimmen
//          5x schnell A oder C = alle Smileys nacheinander
//          Nach manuellem Seitenwechsel bleibt die Seite 1 min stehen.
//
// Aufbau der Module: siehe app.h
// =============================================================================

#include "app.h"

static uint32_t updateTimeOld = 0;

void setup() {
  M5.begin();
  M5.Power.begin();
  Serial.begin(115200);
  M5.Lcd.setBrightness(BRIGHT_NORMAL);
  M5.Lcd.fillScreen(TFT_NAVY);

  diagnoseStart();
  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  verlaufStart();
  initWifi();
  zeitStart();
  mqttStart();

  forceRedraw = true;
  DEBUG_PRINT(F("Setup complete.\n"));
}

void loop() {
  esp_task_wdt_reset();
  ORT(1); handleOta();
  ORT(2); M5.update();
  handleButtons();
  ORT(3); handleWifi();
  ORT(4); handleMqtt();
  ORT(6); handleTestMode();
  sampleHistory();
  pruefeStoerung();
  handleAutoWechsel();
  handleHelligkeit();

  // Rückmeldung zum Tastendruck nach kurzer Zeit wieder entfernen (einmalig neu zeichnen)
  if (feedbackUntil != 0 && millis() > feedbackUntil) {
    feedbackUntil = 0;
    fsPV.text[0] = '\0';                                    // oberes Feld neu zeichnen erzwingen
    fsAussen.text[0] = '\0';
    wetterNeu = true;                                       // Wetterkopf neu zeichnen
    if (page == SEITE_UHR) forceRedraw = true;
  }

  if (millis() - updateTimeOld > UPDATE_INTERVAL) {
    updateTimeOld = millis();
    ORT(7); drawPage();
  }
  if (page == SEITE_SMILEY && !forceRedraw) animSmiley();
  handleAlarm();
  ORT(0);
  yield();
}
