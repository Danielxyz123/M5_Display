#include "app.h"

static void diagZeile(int y, const char* label, const char* wert, uint16_t farbe) {
  M5.Lcd.fillRect(8, y, 304, 16, TFT_NAVY);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(TFT_LIGHTGREY);
  M5.Lcd.drawString(label, 10, y, 2);
  M5.Lcd.setTextColor(farbe);
  M5.Lcd.drawString(wert, 120, y, 2);
}

static void dauerText(char* buf, size_t len, uint32_t s) {
  if (s < 120)        snprintf(buf, len, "%lu s", (unsigned long)s);
  else if (s < 7200)  snprintf(buf, len, "%lu min", (unsigned long)(s / 60));
  else if (s < 172800) snprintf(buf, len, "%lu h", (unsigned long)(s / 3600));
  else                snprintf(buf, len, "%lu Tage", (unsigned long)(s / 86400));
}

void drawDiagnose() {
  char buf[64], d1[16], d2[16];
  int y = 8;
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Lcd.drawCentreString("Diagnose", 160, y, 2);
  y += 20;

  // Bridge
  const bool bOk = bridgeOk();
  dauerText(d1, sizeof(d1), alterS(w.tHb));
  snprintf(buf, sizeof(buf), "%s (vor %s)", bOk ? "OK" : "STOERUNG", w.tHb ? d1 : "-");
  diagZeile(y, "Zaehler/Bridge", buf, bOk ? TFT_GREEN : TFT_RED); y += 17;
  dauerText(d1, sizeof(d1), w.bridgeUptime);
  snprintf(buf, sizeof(buf), "%d dBm, %lu Verb., %s", w.rssi, (unsigned long)w.mqttConnects, d1);
  diagZeile(y, "  WLAN", buf, w.rssi < -75 ? TFT_ORANGE : TFT_WHITE); y += 17;
  snprintf(buf, sizeof(buf), "%lu Fehler", (unsigned long)w.modbusErr);
  diagZeile(y, "  Modbus", buf, w.modbusErr ? TFT_ORANGE : TFT_WHITE); y += 17;

  // Regelung
  const bool sOk = frisch(w.tSoll, SOLL_MAX_AGE_MS);
  dauerText(d1, sizeof(d1), alterS(w.tSoll));
  snprintf(buf, sizeof(buf), "%d W (vor %s)", (int)lroundf(w.soll), w.tSoll ? d1 : "-");
  diagZeile(y, "Regelung", buf, sOk ? TFT_GREEN : TFT_RED); y += 17;

  // Venus-Controller
  const char* grund = "";
  const VenusZustand vz = venusZustand(&grund);
  dauerText(d1, sizeof(d1), alterS(w.tVenus));
  snprintf(buf, sizeof(buf), "%s (vor %s)", vz == VZ_STOERUNG ? "STOERUNG" : "OK", w.tVenus ? d1 : "-");
  diagZeile(y, "Venus-Contr.", buf, vz == VZ_STOERUNG ? TFT_RED : TFT_GREEN); y += 17;
  snprintf(buf, sizeof(buf), "%s %d/%d W", venusTextDe(), w.venusSet, w.venusAc);
  diagZeile(y, "  Soll/Ist", buf, TFT_WHITE); y += 17;
  dauerText(d1, sizeof(d1), w.venusUptime);
  snprintf(buf, sizeof(buf), "Modbus %s, seit %s", w.venusModbusOk ? "ok" : "FEHLER", d1);
  diagZeile(y, "  Controller", buf, w.venusModbusOk ? TFT_WHITE : TFT_RED); y += 17;
  snprintf(buf, sizeof(buf), "%s", w.venusReset[0] ? w.venusReset : "-");
  diagZeile(y, "  Neustart", buf, TFT_WHITE); y += 17;
  if (vz == VZ_STOERUNG) { diagZeile(y, "  Grund", grund, TFT_RED); }
  else if (frisch(mld.t, MELDUNGEN_MAX_AGE_MS)) {          // Meldungen aus dem ioBroker
    snprintf(buf, sizeof(buf), "%d Stoer., %d Warn., %d Info", mld.stoerung, mld.warnung, mld.info);
    diagZeile(y, "Meldungen", buf, mld.stoerung ? TFT_RED : mld.warnung ? TFT_ORANGE : mld.info ? TFT_CYAN : TFT_GREEN);
  } else { diagZeile(y, "Meldungen", "-", TFT_DARKGREY); }
  y += 20;

  // Display selbst
  dauerText(d1, sizeof(d1), millis() / 1000);
  snprintf(buf, sizeof(buf), "%d dBm, MQTT %s, %s", (int)WiFi.RSSI(),
           mqttClient.connected() ? "ok" : "weg", d1);
  diagZeile(y, "Display", buf, mqttClient.connected() ? TFT_WHITE : TFT_RED); y += 17;
  snprintf(buf, sizeof(buf), "W%lu(%u) M%lu/%lu R%lu:%s",
           (unsigned long)wifiAbbrueche, (unsigned)wifiGrund, (unsigned long)mqttAbbrueche,
           (unsigned long)mqttStill, (unsigned long)neustarts, rstText);
  diagZeile(y, "  Abbrueche", buf, (wifiAbbrueche || mqttAbbrueche || neustarts) ? TFT_ORANGE : TFT_WHITE); y += 17;

  // Letzte Störung
  if (lastFaultMillis) {
    dauerText(d2, sizeof(d2), alterS(lastFaultMillis));
    snprintf(buf, sizeof(buf), "vor %s: %s", d2, lastFault);
  } else {
    snprintf(buf, sizeof(buf), "keine");
  }
  diagZeile(y, "Letzte Stoer.", buf, lastFaultMillis ? TFT_ORANGE : TFT_GREEN);
}
