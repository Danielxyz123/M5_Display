#pragma once
// Gemeinsamer Header: bindet Konfiguration, Daten und die Schnittstellen aller Module ein.
//
//   config.h      Einstellungen, Grenzwerte, MQTT-Topics, Seitennummern
//   daten.*       Messwerte, Wetter, Verlauf, Anzeigezustand, Hilfsfunktionen
//   netz.cpp      WLAN, MQTT, OTA, Verbindungsdiagnose
//   zeit.cpp      Uhrzeit (NTP), Nachtmodus / Helligkeit
//   tasten.cpp    Tasten A/B/C
//   seiten.cpp    automatischer Seitenwechsel, Blättern, Seite zeichnen
//   zeichnen.cpp  Zeichenhilfen (Felder, Verläufe, Zahlen mit Komma/Grad)
//   seite_*.cpp   je eine Seite: Solar, Heizung, Wetter, Uhr, Verlauf, Diagnose
//   smiley.cpp    Einblendungen steuern + Morgen-Smiley (animiert)
//   mond.cpp      Gute-Nacht-Mond (animiert)
//   stoerung.cpp  Störungen erkennen, Alarm (Rahmen blinkt, Sprung zur Diagnose)

#include "config.h"
#include "daten.h"

// ---- netz -------------------------------------------------------------------
extern PubSubClient mqttClient;
extern uint32_t wifiAbbrueche, mqttAbbrueche, mqttStill, neustarts;
extern uint8_t  eigenerGrund, wifiGrund, rtcOrt;
extern int32_t  mqttGrund;
extern char     rtcTopic[28], rstText[8], topicVorher[28];
extern uint8_t  ortVorher;
extern uint32_t tLetzteNachricht;
#define ORT(n) (rtcOrt = (n))          // Programmstelle merken (nach Absturz in diag sichtbar)
void diagnoseStart();
void initWifi();
void mqttStart();
void handleOta();
void handleWifi();
void handleMqtt();
void callbackMqttReceive(char* topic, byte* payload, unsigned int length);

// ---- zeit -------------------------------------------------------------------
void zeitStart();
bool minutenJetzt(int& minuten);       // Minuten seit Mitternacht; false = noch keine Uhrzeit
bool istNacht();
void handleHelligkeit();

// ---- tasten -----------------------------------------------------------------
extern bool diagFrei;                  // Verlauf/Diagnose per langem B freigegeben
void handleButtons();
void handleTestMode();

// ---- seiten -----------------------------------------------------------------
void handleAutoWechsel();
void seiteWechseln(int schritt);
void drawPage();
void drawOverview(bool force);
void drawHeizung();
void drawWetter(bool force);
void drawUhr(bool force);
void verlaufStart();
void sampleHistory();
void drawHistory();
void drawDiagnose();

// ---- zeichnen ---------------------------------------------------------------
void drawFieldShadow(int x, int y, int w, int h, int radius, uint16_t shadowColor);
void drawFieldBorder(int x, int y, int w, int h, int radius, uint16_t borderColor);
void fillGradientRoundRect(int x, int y, int w, int h, int radius, uint16_t colorStart, uint16_t colorEnd);
void fmtKomma(char* buf, size_t len, float v);
void drawWert(const char* zahl, bool grad, int cx, int y, int font, int size, uint16_t color);
void drawSeitenRahmen(uint16_t aussen, uint16_t innen);

// ---- smiley -----------------------------------------------------------------
extern uint8_t  smileyNach;            // Seite nach dem Smiley
extern uint32_t tSmiley;
extern uint16_t smileyNr;
extern uint8_t  smileyDemo;            // 0 aus, sonst Demo-Schritt
bool smileyZeit();                     // morgens oder abends?
void zeigeSmileyJetzt();
void drawSmiley();
void animSmiley();
void starteSmileyDemo();
bool smileyDemoWeiter();
void smileyBefehl(const byte* p, unsigned int len);
void drawMond(uint16_t nr);              // mond.cpp
void animMond();

// ---- stoerung ---------------------------------------------------------------
extern char     lastFault[64];
extern uint32_t lastFaultMillis;
extern bool     faultActive;
extern uint32_t tAlarm;                // Zeitpunkt der letzten neuen Störung (0 = keine)
void pruefeStoerung();
void handleAlarm();
