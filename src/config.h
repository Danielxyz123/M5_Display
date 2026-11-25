#pragma once
// Konfiguration: Bibliotheken, Zeiten, Grenzwerte, MQTT-Topics, Seiten

#include <M5Stack.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <ArduinoOTA.h>                // Updates über WLAN
#include <math.h>
#include <cmath>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "include/secrets.h fehlt: include/secrets.h.example kopieren und anpassen"
#endif

// Debugging toggle
#define DEBUG 1
#if DEBUG
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTF(x, ...) Serial.printf(x, ##__VA_ARGS__)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTF(x, ...)
#endif

// === KONFIGURATION ===
#define WATCHDOG_TIMEOUT_S   30        // muss länger sein als ein blockierender connect()
#define UPDATE_INTERVAL      500
#define WIFI_RETRY_MS        15000
#define WIFI_MAX_OFFLINE_MS  300000UL  // so lange ohne WLAN → Neustart
#define MQTT_BACKOFF_MIN     1000
#define MQTT_BACKOFF_MAX     30000
#define MQTT_PACKET_SIZE     2048      // Venus-JSON ist ca. 1 KB
#define MQTT_STILL_MS        30000     // so lange keine Nachricht trotz "verbunden" -> neu verbinden
#define DIAG_PUBLISH_MS      60000     // eigene Verbindungswerte an ioBroker melden
#define LONG_PRESS_MS        3000      // so lange A + C halten: Display dimmen
#define DIAG_PRESS_MS        1500      // so lange B halten: Verlauf/Diagnose freigeben
#define KLICK_MAX_MS         500       // kürzer = Klick, länger gehalten = Lampe dimmen
#define DIMM_INTERVALL_MS    400       // beim Halten alle 400 ms eine Helligkeitsstufe
#define FEEDBACK_MS          800
#define BRIGHT_NORMAL        100
#define BRIGHT_DIM           10

// Wann gelten Werte als veraltet?
#define BRIDGE_MAX_AGE_MS    10000     // Lebenszeichen der Bridge
#define VENUS_MAX_AGE_MS     15000     // Venus-Controller (JSON, Lifebit)
#define SOLL_MAX_AGE_MS      10000     // Regelskript sendet jede Sekunde
#define AKKU_MAX_AGE_MS      60000     // Shelly-Spiegel sendet nur bei Änderung
#define SLOW_MAX_AGE_MS      1800000UL // Ladestand/Autarkie ändern sich selten
#define PV_HEUTE_MAX_AGE_MS  600000    // Tagesertrag kommt alle 30 s
#define STANDBY_W            15        // darunter gilt der Akku als ruhend

// Verlauf: 120 Punkte à 30 s = 60 min
#define HIST_POINTS          120
#define HIST_INTERVAL_MS     30000
#define HIST_INVALID         INT16_MIN

// === TOPICS ===
#define T_GRID      "PV/grid_powerFast"
#define T_GEN       "PV/generationPower"
#define T_PV_HEUTE  "PV/generation_totalPerDay"           // PV-Ertrag heute [kWh] (Bridge, um Mitternacht 0)
#define T_HB        "PV/heartbeat"
#define T_GVALID    "PV/gridValid"
#define T_INV       "PV/inverterOnline"
#define T_RSSI      "PV/rssi"
#define T_MQC       "PV/mqttConnects"
#define T_MBERR     "PV/modbusErr"
#define T_BUPTIME   "PV/uptime"
#define T_AKKU      "VenusData/PowerShelly"
#define T_SOC       "VenusData/Ladezustand"
#define T_AUTARKIE  "VenusData/Autarkie_heute"
#define T_VENUS     "Venus-control"
#define T_SOLL      "Ven_Neu/powerNeu"
#define T_ONLINE    "M5Display/online"
#define T_SMILEY    "M5Display/smiley"                    // beliebige Nachricht (nicht retained) → Smiley sofort
#define T_DIAG      "M5Display/diag"                      // JSON: RSSI, AP, Abbrueche, Gruende
#define T_HEIZUNG   "oeg_kmsd/state"                      // Heizungsregelung OEG KMS-D (JSON, ca. alle 10 s)
#define T_BR_ZEIT   "Heizung/Brenner_Laufzeit_heute"      // vom ioBroker-Skript gespiegelt [h]
#define T_BR_STARTS "Heizung/Brenner_Starts_heute"
#define T_BR_24H    "Heizung/Brenner_Anteil_24h"          // Brennerlaufzeit-Anteil letzte 24 h [%]
#define T_WOHNZIMMER "sensor1/temperature"                 // Wohnzimmertemperatur [°C]
#define WOHNZIMMER_MAX_AGE_MS 900000UL                     // älter als 15 min → grau
#define T_WETTER    "Wetter/vorhersage"                   // Vorhersage Melle (JSON, ioBroker-Skript)
#define WETTER_MAX_AGE_MS    5400000UL // Abruf alle 30 min; älter als 90 min → grau

// Heizung
#define HEIZ_MAX_AGE_MS      120000    // Heizungsregelung sendet ca. alle 10 s
#define BRENNER_MAX_AGE_MS   600000    // Brennerwerte kommen spätestens jede Minute
#define FBH_ABWEICHUNG_K     5.0f      // Vorlauf so weit unter Soll → roter Rahmen
#define WW_ABWEICHUNG_K      5.0f      // Warmwasser so weit unter Soll → roter Rahmen

// Seiten
#define SEITE_SOLAR          0
#define SEITE_HEIZUNG        1
#define SEITE_WETTER         2
#define SEITE_UHR            3
#define SEITE_VERLAUF        4
#define SEITE_DIAGNOSE       5
#define SEITEN               6
#define SEITE_SMILEY         6         // Smiley/Mond-Einblendung, nicht per Taste erreichbar
#define AUTO_SEITEN          4         // Solar → Heizung → Wetter → Uhr wechseln automatisch
#define AUTO_WECHSEL_MS      10000
#define MANUELL_HALTEN_MS    60000     // manuell gewählte Seite so lange stehen lassen

// Zeit, Nachtmodus
#define ZEITZONE             "CET-1CEST,M3.5.0,M10.5.0/3"
#define NACHT_VON_MIN        (22 * 60)       // Nachtmodus ab 22:00 ...
#define NACHT_BIS_MIN        (6 * 60)        // ... bis 6:00
#define NACHT_WACH_MS        30000     // nach Tastendruck so lange hell
#define BRIGHT_NACHT         4

// Smiley (morgens) und Gute-Nacht-Mond (abends)
#define SMILEY_VON_MIN       (6 * 60)        // morgens ab 6:00 ...
#define SMILEY_BIS_MIN       (10 * 60)       // ... bis 10:00
#define MOND_VON_MIN         (21 * 60)       // abends ab 21:00 ...
#define MOND_BIS_MIN         (23 * 60)       // ... bis 23:00
#define SMILEY_ABSTAND_MS    180000    // höchstens alle 3 min einblenden
#define SMILEY_DAUER_MS      6000      // so lange stehen lassen

// Störungsalarm
#define ALARM_HALTEN_MS      60000     // nach neuer Störung so lange auf der Diagnose bleiben
#define ALARM_BLINK_MS       500       // Rahmen blinkt rot
#define ALARM_START_SPERRE_MS 60000    // nach dem Start noch keinen Alarm (Daten kommen erst)

// === LAMPE STEUERUNG ÜBER TASTEN ===
#define LAMP_TOPIC_ONOFF        "lampe/wohnzimmer/set"            // ioBroker: ON / OFF (retained)
#define LAMP_TOPIC_BRIGHTNESS   "lampe/wohnzimmer/brightness_set" // ioBroker: +10 oder -10

#define SERIEN_KLICK_MS      600       // Abstand zwischen Klicks einer Serie (5x A/C = Smiley-Demo)
#define OTA_HOSTNAME         "m5display"
