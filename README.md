# M5Stack-Display: PV, Akku, Heizung, Wetter

M5Stack Core (ESP32, 320×240) im Wohnzimmer. Zeigt die wichtigsten Werte aus dem ioBroker (MQTT) und schaltet die Govee-Wohnzimmerlampe.

## Seiten

| Nr. | Seite | Inhalt | Automatischer Wechsel |
|---|---|---|---|
| 0 | Solar | PV (Leistung, Tagesertrag), Netz, Akku (Rahmenfarbe = Zustand Venus), Ladestand, Autarkie | ja |
| 1 | Heizung | Außen/Wohnzimmer, Vorlauf FBH, Kessel, Warmwasser, Brenner heute/24 h, „GLATT!“ bei Glättegefahr | ja |
| 2 | Wetter | jetzt, 4 Stundenfelder, heute/morgen mit Regen (l/m²) und PV-Prognose; Kasten **WARNUNG!** (DWD) bzw. **GLAETTE!** | ja |
| 3 | Uhr | Uhrzeit, Datum, Außentemperatur | ja |
| 4 | Sonne & Mond | Sonnenbogen mit Sonne jetzt, Auf-/Untergang, Tageslänge (± zu gestern), Mondphase als Bild, Beleuchtung, nächster Voll-/Neumond | ja |
| 5 | Verlauf | Netz, PV, Akku der letzten 60 min | nur nach Freigabe |
| 6 | Diagnose | Bridge, Regelung, Venus-Controller, Display, letzte Störung | nur nach Freigabe |
| – | Smiley/Mond | Einblendung morgens/abends | – |

Seiten wechseln alle 10 s (Solar → Heizung → Wetter → Uhr → Sonne & Mond).

**Der automatische Wechsel ruht**, solange
- eine **Störung** vorliegt – eigene Erkennung (Zählerwert veraltet, Heizung sendet nicht, Venus-Störung; Rahmen blinkt rot, beim Auftreten Sprung zur Diagnose) oder eine Störmeldung des ioBroker-Skripts `Meldungen` (alle Geräte), oder
- eine **DWD-Wetterwarnung** vorliegt: Hitze (ab gelb), Sturm/Orkan, Gewitter, Starkregen/Dauerregen, Hagel, strenger Frost (ab orange). Beim Auftreten einmal Sprung zur Wetterseite.

Blättern mit den Tasten geht weiterhin. Sind Warnung und Störung vorbei, wechselt das Display wieder selbst.

## Tasten

| Taste | Klick (< 0,5 s) | Halten |
|---|---|---|
| A (links) | Seite zurück | Lampe dunkler (alle 0,4 s) |
| B (Mitte) | Lampe an/aus | 1,5 s: Verlauf/Diagnose freigeben (Sprung zur Diagnose); auf der Diagnose: Testmodus |
| C (rechts) | Seite vor | Lampe heller (alle 0,4 s) |
| A + C | – | 3 s: Display dimmen |

Nach einem Tastendruck bleibt die Seite 1 min stehen, danach endet auch die Freigabe von Verlauf/Diagnose.

## Daten (MQTT)

| Topic | Quelle | Inhalt |
|---|---|---|
| `PV/*` | FoxEss-Chint-Bridge | Netz, PV, Tagesertrag, Lebenszeichen |
| `Venus-control` | VenusControl-ESP32 | JSON jede Sekunde; daraus auch die **Akkuleistung** (`AC power bat`) |
| `VenusData/*`, `Ven_Neu/powerNeu` | ioBroker | Ladestand, Autarkie, Sollwert |
| `oeg_kmsd/state` | Heizungsregelung | Temperaturen, Relais (JSON) |
| `Heizung/*` | Skript `HeizungMqttDisplay` | Brennerwerte |
| `sensor1/temperature` | Wohnzimmersensor | °C |
| `Wetter/vorhersage` | Skript `WetterMqttDisplay` | JSON: `n` jetzt, `h` Stunden, `d` Tage, `g` Glätte, `w` DWD-Warnung |
| `Meldungen/zusammenfassung` | Skript `Meldungen` | Anzahl Störungen/Warnungen/Infos und wichtigste Meldung; Diagnoseseite Zeile „Meldungen“ |
| `lampe/wohnzimmer/*` | Display → ioBroker | Lampe an/aus, Helligkeit ±10 |
| `M5Display/online`, `M5Display/diag` | Display | Lebenszeichen, Diagnose |

Sonne und Mond rechnet das Display selbst (`astro.cpp`, Standort `ORT_LAT/ORT_LON` in `config.h`).

## Aufbau (`src/`)

| Datei | Aufgabe |
|---|---|
| `config.h` | Einstellungen, Grenzwerte, Topics, Seitennummern, Standort |
| `daten.*` | Messwerte, Wetter, Verlauf, Anzeigezustand |
| `netz.cpp` | WLAN, MQTT (Empfang/Auswertung), OTA, Verbindungsdiagnose |
| `zeit.cpp` | Uhrzeit (NTP), Nachtmodus/Helligkeit |
| `tasten.cpp` | Tasten |
| `seiten.cpp` | automatischer Wechsel (inkl. Anhalten bei Warnung/Störung), Blättern, Seite zeichnen |
| `zeichnen.cpp` | Zeichenhilfen |
| `seite_*.cpp` | je eine Seite |
| `astro.cpp` | Sonne (Sonnengleichung) und Mond (Meeus) berechnen |
| `stoerung.cpp` | Störungen erkennen, Alarm |
| `smiley.cpp`, `mond.cpp` | Einblendungen |

## Bauen und Aufspielen

- PlatformIO, Umgebung `m5stack-core-esp32` (USB, COM14).
- Über WLAN: `pio run -e m5stack-core-esp32-ota -t upload` (Umgebung in `ota.ini`, nicht eingecheckt, Vorlage `ota.ini.example`).
- Zugangsdaten in `include/secrets.h` (nicht eingecheckt, Vorlage `secrets.h.example`).
