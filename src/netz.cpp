#include "app.h"

// =============================================================================
// Netz: WLAN, MQTT (Empfang + eigene Diagnose), OTA-Updates
// =============================================================================
WiFiClient   espClient;
PubSubClient mqttClient(espClient);

// Verbindungsstatistik des Displays selbst. RTC_NOINIT: übersteht Neustarts
// (Watchdog, Absturz, Software), wird nur beim Einschalten gelöscht.
#define RTC_MAGIC 0x4D354431UL
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t wifiAbbrueche, mqttAbbrueche, mqttStill, neustarts;
RTC_NOINIT_ATTR uint8_t  eigenerGrund;  // 'W' = Neustart wegen WLAN, 'O' = OTA, sonst 0
RTC_NOINIT_ATTR uint8_t  wifiGrund;     // letzter WLAN-Trenngrund (wifi_err_reason_t)
RTC_NOINIT_ATTR int32_t  mqttGrund;     // PubSubClient-Status beim letzten Abbruch
RTC_NOINIT_ATTR uint8_t  rtcOrt;        // wo war das Programm zuletzt (ORT)
RTC_NOINIT_ATTR char     rtcTopic[28];  // zuletzt empfangenes Topic
char     rstText[8] = "";              // Grund des letzten Neustarts
uint8_t  ortVorher = 0;                // Stand vor diesem Neustart
char     topicVorher[28] = "";
uint32_t tLetzteNachricht = 0;         // letzte empfangene MQTT-Nachricht

// Beim Start: Neustartgrund ermitteln, Zähler übernehmen oder (Einschalten) löschen
void diagnoseStart() {
  const esp_reset_reason_t rr = esp_reset_reason();
  if (rtcMagic != RTC_MAGIC || rr == ESP_RST_POWERON) {
    rtcMagic = RTC_MAGIC;
    wifiAbbrueche = mqttAbbrueche = mqttStill = neustarts = 0;
    eigenerGrund = wifiGrund = 0;
    mqttGrund = 0;
    rtcOrt = 0; rtcTopic[0] = '\0';
  } else {
    neustarts++;
  }
  ortVorher = rtcOrt;
  rtcTopic[sizeof(rtcTopic) - 1] = '\0';
  strncpy(topicVorher, rtcTopic, sizeof(topicVorher) - 1);
  rtcOrt = 0;
  const char* rs = "?";
  switch (rr) {
    case ESP_RST_POWERON:  rs = "Strom"; break;
    case ESP_RST_EXT:      rs = "Ext";   break;
    case ESP_RST_SW:       rs = eigenerGrund == 'W' ? "WLAN" : eigenerGrund == 'O' ? "OTA" : "SW"; break;
    case ESP_RST_PANIC:    rs = "Panic"; break;
    case ESP_RST_INT_WDT:  rs = "IntWDT"; break;
    case ESP_RST_TASK_WDT: rs = "TaskWDT"; break;
    case ESP_RST_WDT:      rs = "WDT";   break;
    case ESP_RST_BROWNOUT: rs = "Spann."; break;
    default: break;
  }
  strncpy(rstText, rs, sizeof(rstText) - 1);
  eigenerGrund = 0;
  DEBUG_PRINTF("Neustartgrund: %s (%d), Neustarts: %lu\n", rstText, (int)rr, (unsigned long)neustarts);
}

void mqttStart() {
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setBufferSize(MQTT_PACKET_SIZE);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(10);                          // Lesetimeout je Byte; zu kurz → Paket reißt ab
  mqttClient.setCallback(callbackMqttReceive);
}

// =============================================================================
// OTA: Updates über WLAN (PlatformIO-Umgebung "m5stack-core-esp32-ota")
// =============================================================================

static void otaStatus(const char* zeile1, const char* zeile2, uint16_t farbe) {
  M5.Lcd.fillScreen(TFT_NAVY);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(farbe);
  M5.Lcd.drawCentreString(zeile1, 160, 80, 4);
  M5.Lcd.setTextColor(TFT_WHITE);
  M5.Lcd.drawCentreString(zeile2, 160, 120, 2);
}

void handleOta() {
  static bool gestartet = false;
  if (!gestartet) {
    if (WiFi.status() != WL_CONNECTED) return;
    ArduinoOTA.setHostname(OTA_HOSTNAME);
#ifdef OTA_PASS
    ArduinoOTA.setPassword(OTA_PASS);
#endif
    ArduinoOTA.onStart([]() {
      esp_task_wdt_delete(NULL);                            // Update dauert länger als der Watchdog
      mqttClient.disconnect();
      otaStatus("Update", "wird uebertragen ...", TFT_YELLOW);
      M5.Lcd.drawRect(40, 160, 240, 20, TFT_WHITE);
    });
    ArduinoOTA.onProgress([](unsigned int fertig, unsigned int gesamt) {
      if (gesamt) M5.Lcd.fillRect(42, 162, (int)(236UL * fertig / gesamt), 16, TFT_GREEN);
    });
    ArduinoOTA.onEnd([]() {
      eigenerGrund = 'O';
      otaStatus("Update fertig", "Neustart ...", TFT_GREEN);
    });
    ArduinoOTA.onError([](ota_error_t fehler) {
      char buf[32];
      snprintf(buf, sizeof(buf), "Fehler %u", (unsigned)fehler);
      otaStatus("Update fehlgeschlagen", buf, TFT_RED);
      delay(3000);
      esp_task_wdt_add(NULL);
      forceRedraw = true;
    });
    ArduinoOTA.begin();
    gestartet = true;
    DEBUG_PRINTF("OTA bereit: %s.local / %s\n", OTA_HOSTNAME, WiFi.localIP().toString().c_str());
  }
  ArduinoOTA.handle();
}

// =============================================================================
// WLAN / MQTT (nicht blockierend)
// =============================================================================
void initWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    wifiGrund = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  DEBUG_PRINT(F("WiFi verbinden...\n"));
}

void handleWifi() {
  static uint32_t offlineSince = 0;
  static uint32_t lastTry = 0;
  const uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (offlineSince != 0) {
      DEBUG_PRINTF("WiFi OK: %s\n", WiFi.localIP().toString().c_str());
      forceRedraw = true;
    }
    offlineSince = 0;
    return;
  }
  if (offlineSince == 0) {
    offlineSince = now; lastTry = now;
    if (now > 20000) wifiAbbrueche++;                      // nicht beim Start zählen
  }
  if (now - lastTry >= WIFI_RETRY_MS) {
    lastTry = now;
    DEBUG_PRINT(F("WiFi neuer Versuch\n"));
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
  if (now - offlineSince >= WIFI_MAX_OFFLINE_MS) {
    DEBUG_PRINT(F("WiFi zu lange weg, Neustart\n"));
    eigenerGrund = 'W';
    ESP.restart();
  }
}

void handleMqtt() {
  static uint32_t lastTry = 0;
  static uint32_t backoff = MQTT_BACKOFF_MIN;

  static bool warVerbunden = false;

  if (WiFi.status() != WL_CONNECTED) {
    if (warVerbunden) { warVerbunden = false; mqttAbbrueche++; mqttGrund = -3; }   // WLAN weg
    return;
  }

  // Halboffene Verbindung: Client meint "verbunden", es kommt aber nichts mehr an
  // (Bridge sendet mehrmals pro Sekunde) -> Socket schließen und neu verbinden
  if (mqttClient.connected() && millis() - tLetzteNachricht > MQTT_STILL_MS) {
    DEBUG_PRINT(F("MQTT still, neu verbinden\n"));
    mqttStill++;
    mqttClient.disconnect();
    espClient.stop();
  }

  if (!mqttClient.connected()) {
    if (warVerbunden) {
      warVerbunden = false;
      mqttAbbrueche++;
      mqttGrund = mqttClient.state();
      DEBUG_PRINTF("MQTT getrennt rc=%d\n", mqttGrund);
    }
    if (lastTry != 0 && millis() - lastTry < backoff) return;   // kein delay() mehr
    lastTry = millis();
    String clientId = "M5Stack-" + WiFi.macAddress();
    DEBUG_PRINT(F("MQTT verbinden...\n"));
    if (mqttClient.connect(clientId.c_str(), T_ONLINE, 1, true, "0")) {
      mqttClient.publish(T_ONLINE, "1", true);
      const char* topics[] = {
        T_GRID, T_GEN, T_HB, T_GVALID, T_INV, T_RSSI, T_MQC, T_MBERR, T_BUPTIME,
        T_AKKU, T_SOC, T_AUTARKIE, T_VENUS, T_SOLL, LAMP_TOPIC_ONOFF, T_PV_HEUTE,
        T_HEIZUNG, T_BR_ZEIT, T_BR_STARTS, T_BR_24H, T_WOHNZIMMER, T_WETTER, T_SMILEY
      };
      for (auto t : topics) mqttClient.subscribe(t);
      backoff = MQTT_BACKOFF_MIN;
      warVerbunden = true;
      tLetzteNachricht = millis();
      DEBUG_PRINT(F("MQTT verbunden\n"));
    } else {
      DEBUG_PRINTF("MQTT Fehler rc=%d, naechster Versuch in %lu ms\n", mqttClient.state(), (unsigned long)backoff);
      backoff = min((uint32_t)(backoff * 2), (uint32_t)MQTT_BACKOFF_MAX);
    }
    return;
  }
  mqttClient.loop();
  ORT(4);

  // Eigene Verbindungswerte melden (retained), damit ioBroker Abbrüche aufzeichnen kann
  static uint32_t tDiag = 0;
  if (mqttClient.connected() && (tDiag == 0 || millis() - tDiag >= DIAG_PUBLISH_MS)) {
    tDiag = millis();
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"rssi\":%d,\"bssid\":\"%s\",\"kanal\":%d,\"uptime\":%lu,\"wifiAbbr\":%lu,\"wifiGrund\":%u,"
             "\"mqttAbbr\":%lu,\"mqttGrund\":%d,\"mqttStill\":%lu,\"neustarts\":%lu,\"rst\":\"%s\","
             "\"ort\":%u,\"topic\":\"%s\",\"heap\":%u,\"heapMin\":%u}",
             (int)WiFi.RSSI(), WiFi.BSSIDstr().c_str(), (int)WiFi.channel(), (unsigned long)(millis() / 1000),
             (unsigned long)wifiAbbrueche, (unsigned)wifiGrund, (unsigned long)mqttAbbrueche, (int)mqttGrund,
             (unsigned long)mqttStill, (unsigned long)neustarts, rstText, (unsigned)ortVorher, topicVorher,
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    mqttClient.publish(T_DIAG, buf, true);
  }
}

static bool parseZahl(const char* s, float& out) {
  char* end;
  out = strtof(s, &end);
  while (*end == ' ' || *end == '\r' || *end == '\n') end++;
  return end != s && *end == '\0' && std::isfinite(out);
}

static void verarbeiteVenus(byte* payload, unsigned int length) {
  StaticJsonDocument<256> filter;
  filter["Controller_tekst"] = true;
  filter["Modbus_ok"]        = true;
  filter["Lifebit"]          = true;
  filter["Setpoint"]         = true;
  filter["AC power bat"]     = true;
  filter["Reset_reason"]     = true;
  filter["Uptime"]           = true;
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, payload, length, DeserializationOption::Filter(filter))) {
    DEBUG_PRINT(F("Venus-JSON fehlerhaft\n"));
    return;
  }
  const uint32_t lb = strtoul(doc["Lifebit"] | "0", nullptr, 10);
  if (lb != w.venusLifebit) { w.venusLifebit = lb; w.tVenusLifebit = millis(); }
  strncpy(w.venusText, doc["Controller_tekst"] | "", sizeof(w.venusText) - 1);
  strncpy(w.venusReset, doc["Reset_reason"] | "", sizeof(w.venusReset) - 1);
  w.venusModbusOk = strcmp(doc["Modbus_ok"] | "1", "0") != 0;
  w.venusSet      = atoi(doc["Setpoint"] | "0");
  w.venusAc       = atoi(doc["AC power bat"] | "0");
  w.venusUptime   = strtoul(doc["Uptime"] | "0", nullptr, 10);
  w.tVenus        = millis();
  // Akkuleistung direkt aus dem Venus-JSON (kommt jede Sekunde; der MQTT-Spiegel
  // VenusData/PowerShelly sendet nur bei Änderung). Vorzeichen wie Shelly: + Laden, - Entladen.
  if (!testMode && w.venusModbusOk && !doc["AC power bat"].isNull()) {
    w.akku  = -(float)w.venusAc;
    w.tAkku = w.tVenus;
  }
}

static void verarbeiteHeizung(byte* payload, unsigned int length) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, payload, length)) {
    DEBUG_PRINT(F("Heizungs-JSON fehlerhaft\n"));
    return;
  }
  w.tAussen     = doc["t2"] | 0.0f;
  w.tKessel     = doc["t3"] | 0.0f;
  w.tVorlauf    = doc["t4"] | 0.0f;
  w.tWW         = doc["t5"] | 0.0f;
  w.sollKessel  = doc["boiler_set"] | 0.0f;
  w.sollVorlauf = doc["flow_set"] | 0.0f;
  w.sollWW      = doc["dhw_set"] | 0.0f;
  w.relais      = doc["relays"] | 0;
  w.tHeiz       = millis();
}

static void verarbeiteWetter(byte* payload, unsigned int length) {
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, payload, length)) {
    DEBUG_PRINT(F("Wetter-JSON fehlerhaft\n"));
    return;
  }
  JsonArrayConst n = doc["n"];
  if (n.isNull()) return;
  strncpy(wet.stand, doc["z"] | "", sizeof(wet.stand) - 1);
  wet.temp = n[0] | 0.0f;
  wet.code = n[1] | 0;
  wet.wind = n[2] | 0;
  wet.tag  = (n[3] | 1) != 0;
  wet.regen1h = n[4] | 0.0f;
  wet.nStunden = 0;
  for (JsonArrayConst e : doc["h"].as<JsonArrayConst>()) {
    if (wet.nStunden >= 4) break;
    WetterStunde& h = wet.h[wet.nStunden++];
    h.std = e[0] | 0;  h.temp = e[1] | 0.0f;  h.code = e[2] | 0;
    h.regenP = e[3] | 0;  h.tag = (e[4] | 1) != 0;  h.mm = e[5] | 0.0f;
  }
  wet.nTage = 0;
  for (JsonArrayConst e : doc["d"].as<JsonArrayConst>()) {
    if (wet.nTage >= 4) break;
    WetterTag& d = wet.d[wet.nTage++];
    strncpy(d.name, e[0] | "", sizeof(d.name) - 1);
    d.name[sizeof(d.name) - 1] = '\0';
    d.code = e[1] | 0;  d.tmax = e[2] | 0.0f;  d.tmin = e[3] | 0.0f;
    d.mm = e[4] | 0.0f;  d.regenP = e[5] | 0;  d.pv = e[6] | -1.0f;
  }
  JsonArrayConst g = doc["g"];
  wet.glStufe = g.isNull() ? 0 : (g[0] | 0);
  strncpy(wet.glArt,    g.isNull() ? "" : (g[1] | ""), sizeof(wet.glArt) - 1);
  strncpy(wet.glZusatz, g.isNull() ? "" : (g[2] | ""), sizeof(wet.glZusatz) - 1);
  strncpy(wet.glZeit,   g.isNull() ? "" : (g[3] | ""), sizeof(wet.glZeit) - 1);
  // DWD-Wetterwarnung (Hitze, Sturm, Gewitter, Regen, Hagel, strenger Frost); neue Warnung → einmal Wetterseite zeigen
  JsonArrayConst wa = doc["w"];
  const int warnVorher = wet.warnStufe;
  wet.warnStufe = wa.isNull() ? 0 : (wa[0] | 0);
  strncpy(wet.warnArt,  wa.isNull() ? "" : (wa[1] | ""), sizeof(wet.warnArt) - 1);
  strncpy(wet.warnZeit, wa.isNull() ? "" : (wa[2] | ""), sizeof(wet.warnZeit) - 1);
  if (wet.warnStufe > 0 && warnVorher == 0 && !faultActive && page != SEITE_SMILEY) {
    page = SEITE_WETTER;
    lastPageSwitch = millis();
    forceRedraw = true;
  }
  wet.t = millis();
  wetterNeu = true;
}

void callbackMqttReceive(char* topic, byte* payload, unsigned int length) {
  tLetzteNachricht = millis();
  ORT(5);
  strncpy(rtcTopic, topic, sizeof(rtcTopic) - 1);
  rtcTopic[sizeof(rtcTopic) - 1] = '\0';
  if (strcmp(topic, T_WETTER) == 0) {
    verarbeiteWetter(payload, length);
    return;
  }
  if (strcmp(topic, T_VENUS) == 0) {                      // großes JSON
    verarbeiteVenus(payload, length);
    return;
  }
  if (strcmp(topic, T_HEIZUNG) == 0) {
    verarbeiteHeizung(payload, length);
    return;
  }
  if (strcmp(topic, T_SMILEY) == 0) {
    smileyBefehl(payload, length);
    return;
  }

  char msg[32] = {0};
  if (length >= sizeof(msg)) length = sizeof(msg) - 1;
  memcpy(msg, payload, length);

  if (strcmp(topic, LAMP_TOPIC_ONOFF) == 0) {             // tatsächlicher Lampenzustand
    lampOn = (strcmp(msg, "ON") == 0);
    return;
  }

  float v;
  if (!parseZahl(msg, v)) {
    DEBUG_PRINTF("Ungueltig %s: %s\n", topic, msg);
    return;
  }
  const uint32_t now = millis();
  // Im Testmodus werden nur die fünf angezeigten Werte durch Testdaten ersetzt;
  // Lebenszeichen, Sollwert und Diagnose laufen normal weiter.
  const bool anzeigeWert = !strcmp(topic, T_GRID) || !strcmp(topic, T_GEN) || !strcmp(topic, T_AKKU) ||
                           !strcmp(topic, T_SOC)  || !strcmp(topic, T_AUTARKIE);
  if (testMode && anzeigeWert) return;

  if      (!strcmp(topic, T_GRID))     { w.grid = v; w.tGrid = now; }
  else if (!strcmp(topic, T_GEN))      { w.gen = v; w.tGen = now; }
  else if (!strcmp(topic, T_PV_HEUTE)) { w.pvHeute = v; w.tPvHeute = now; }
  else if (!strcmp(topic, T_HB))       { if ((uint32_t)v != w.hb) { w.hb = (uint32_t)v; w.tHb = now; } }
  else if (!strcmp(topic, T_GVALID))   { w.gridValid = (v > 0.5f); }
  else if (!strcmp(topic, T_INV))      { w.invOnline = (v > 0.5f); }
  else if (!strcmp(topic, T_RSSI))     { w.rssi = (int)v; }
  else if (!strcmp(topic, T_MQC))      { w.mqttConnects = (uint32_t)v; }
  else if (!strcmp(topic, T_MBERR))    { w.modbusErr = (uint32_t)v; }
  else if (!strcmp(topic, T_BUPTIME))  { w.bridgeUptime = (uint32_t)v; }
  else if (!strcmp(topic, T_AKKU))     { w.akku = v; w.tAkku = now; }
  else if (!strcmp(topic, T_SOC))      { w.soc = lroundf(v); w.tSoc = now; }
  else if (!strcmp(topic, T_AUTARKIE)) { w.autarkie = lroundf(v); w.tAutarkie = now; }
  else if (!strcmp(topic, T_SOLL))     { w.soll = v * 1000.0f; w.tSoll = now; }
  else if (!strcmp(topic, T_BR_ZEIT))  { w.brennerH = v; w.tBrenner = now; }
  else if (!strcmp(topic, T_BR_STARTS)){ w.brennerStarts = lroundf(v); w.tBrenner = now; }
  else if (!strcmp(topic, T_BR_24H))   { w.brennerAnteil24 = v; w.brennerAnteil24Da = true; w.tBrenner = now; }
  else if (!strcmp(topic, T_WOHNZIMMER)) { w.tWohnzimmer = v; w.tWohnzimmerZeit = now; }
}
