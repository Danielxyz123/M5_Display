#include <M5Stack.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>
#include <math.h>

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
#define WATCHDOG_TIMEOUT 5
#define WIFI_PASSWORD "96031546242323421756"
#define MQTT_SERVER "192.168.178.44"
#define MQTT_PORT 1883
#define SSID "FRITZ!Box 7590 DG"
#define UPDATE_INTERVAL 500
#define WIFI_RECONNECT_INTERVAL 30000
#define MQTT_BACKOFF_MAX 10000
#define FIELD_BUFFER_SIZE 8
#define MQTT_BUFFER_SIZE 32

// === LAMPE STEUERUNG ÜBER TASTEN ===
#define LAMP_TOPIC_ONOFF        "lampe/wohnzimmer/set"          // ioBroker: ON / OFF (retained)
#define LAMP_TOPIC_BRIGHTNESS   "lampe/wohnzimmer/brightness_set" // ioBroker: +10 oder -10

// Entprellung & Feedback
uint32_t lastBtnPress = 0;
#define BUTTON_DEBOUNCE 250  // ms

// Function prototypes
void initWifi();
void callbackMqttReceive(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
void reconnectWifi();
void updateDisplay();
void drawGrid();
void drawPVField(int value, bool forceRedraw = false);
void drawNetzField(const char* value, int intValue, bool forceRedraw = false);
void drawAkkuField(const char* value, int intValue, bool forceRedraw = false);
void drawBatteryField(const char* value, int intValue, bool forceRedraw = false);
void drawAutarkyField(const char* value, bool forceRedraw = false);
void drawFieldShadow(int x, int y, int w, int h, int radius, uint16_t shadowColor);
void drawFieldBorder(int x, int y, int w, int h, int radius, uint16_t borderColor);
void fillGradientRoundRect(int x, int y, int w, int h, int radius, uint16_t colorStart, uint16_t colorEnd);
void clearButtonFeedback();

// Global variables
WiFiClient espClient;
PubSubClient mqttClient(espClient);
char gridPower[FIELD_BUFFER_SIZE] = "0";
char generationPower[FIELD_BUFFER_SIZE] = "0";
char accuPower[FIELD_BUFFER_SIZE] = "0";
char autarkyPercent[FIELD_BUFFER_SIZE] = "0";
char batteryLevelPercent[FIELD_BUFFER_SIZE] = "0";
uint32_t updateTime = 0;
uint32_t updateTimeOld = 0;
int old_gridPower = 0;
int old_accuPower = 0;
int old_batteryLevel = 0;
int old_autarky = 0;
int old_generation = 0;
uint8_t testMode = 0;
bool lampToggleState = false;       // für Toggle-Logik von BtnA

void setup() {
    M5.begin();
    M5.Power.begin();
    Serial.begin(115200);
    M5.Lcd.fillScreen(TFT_BLACK);

    esp_task_wdt_init(WATCHDOG_TIMEOUT, true);
    esp_task_wdt_add(NULL);

    drawGrid();
    initWifi();

    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setKeepAlive(60);
    mqttClient.setCallback(callbackMqttReceive);

    DEBUG_PRINT(F("Setup complete.\n"));
    DEBUG_PRINT(F("BtnA = Lampe Ein/Aus  |  BtnB = Heller  |  BtnC = Dunkler\n"));
}

void loop() {
    esp_task_wdt_reset();
    M5.update();

    // === TESTMODE (unverändert) ===
    if (M5.BtnA.wasPressed() && millis() - lastBtnPress < 100) {
        // sehr kurzer Druck innerhalb 100ms → Testmode togglen (wie vorher)
        testMode = !testMode;
        DEBUG_PRINTF("Test mode: %s\n", testMode ? "ON" : "OFF");
    }

    // === LAMPE STEUERUNG ===
    if (millis() - lastBtnPress > BUTTON_DEBOUNCE) {
        if (M5.BtnA.wasPressed()) {
            lastBtnPress = millis();
            lampToggleState = !lampToggleState;
            const char* payload = lampToggleState ? "ON" : "OFF";
            mqttClient.publish(LAMP_TOPIC_ONOFF, payload, true); // retained
            M5.Lcd.fillRect(260, 8, 52, 28, TFT_BLACK);
            M5.Lcd.setTextColor(lampToggleState ? TFT_GREEN : TFT_RED);
            M5.Lcd.drawCentreString(lampToggleState ? "AN" : "AUS", 286, 12, 2);
        }

        if (M5.BtnB.wasPressed()) {
            lastBtnPress = millis();
            mqttClient.publish(LAMP_TOPIC_BRIGHTNESS, "+10", false);
            M5.Lcd.fillRect(260, 8, 52, 28, TFT_BLACK);
            M5.Lcd.setTextColor(TFT_WHITE);
            M5.Lcd.drawCentreString("+", 286, 10, 4);
        }

        if (M5.BtnC.wasPressed()) {
            lastBtnPress = millis();
            mqttClient.publish(LAMP_TOPIC_BRIGHTNESS, "-10", false);
            M5.Lcd.fillRect(260, 8, 52, 28, TFT_BLACK);
            M5.Lcd.setTextColor(TFT_WHITE);
            M5.Lcd.drawCentreString("-", 286, 10, 4);
        }
    }

    // Feedback nach 800 ms wieder löschen
    if (lastBtnPress != 0 && millis() - lastBtnPress > 800) {
        M5.Lcd.fillRect(260, 8, 52, 28, TFT_NAVY);
        // kein Reset von lastBtnPress nötig – wird beim nächsten Druck überschrieben
    }

    // === TESTDATEN (unverändert) ===
    if (testMode) {
        static uint32_t testTime = 0;
        if (millis() - testTime > 5000) {
            testTime = millis();
            snprintf(generationPower, FIELD_BUFFER_SIZE, "%ld", random(0, 10000));
            snprintf(gridPower, FIELD_BUFFER_SIZE, "%ld", random(-500, 1000));
            snprintf(accuPower, FIELD_BUFFER_SIZE, "%ld", random(-200, 500));
            snprintf(batteryLevelPercent, FIELD_BUFFER_SIZE, "%ld", random(20, 90));
            snprintf(autarkyPercent, FIELD_BUFFER_SIZE, "%ld", random(0, 100));
        }
    }

    reconnectWifi();
    if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
        reconnectMQTT();
    }
    mqttClient.loop();

    updateTime = millis();
    if (updateTime - updateTimeOld > UPDATE_INTERVAL) {
        updateTimeOld = updateTime;
        updateDisplay();
    }

    yield();
}

// === Alle deine ursprünglichen Funktionen (unverändert, nur drawPVField korrigiert + kW-Anzeige) ===

void initWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(SSID, WIFI_PASSWORD);

    DEBUG_PRINT(F("WiFi connecting..."));
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        DEBUG_PRINT(".");
        attempts++;
        esp_task_wdt_reset();
    }

    if (WiFi.status() == WL_CONNECTED) {
        DEBUG_PRINTF("\nWiFi OK: %s\n", WiFi.localIP().toString().c_str());
    } else {
        DEBUG_PRINT(F("\nWiFi fail! Restart...\n"));
        delay(5000);
        ESP.restart();
    }
}

void drawGrid() {
    M5.Lcd.fillScreen(TFT_NAVY);
    M5.Lcd.drawRoundRect(1, 1, 318, 238, 10, TFT_LIGHTGREY);
    M5.Lcd.drawRoundRect(2, 2, 316, 236, 8, TFT_DARKGREY);

    drawPVField(0, true);
    drawNetzField("0", 0, true);
    drawAkkuField("0", 0, true);
    drawBatteryField("0", 0, true);
    drawAutarkyField("0", true);
}

void drawFieldShadow(int x, int y, int w, int h, int radius, uint16_t shadowColor) {
    M5.Lcd.fillRoundRect(x + 4, y + 4, w - 8, h - 8, radius - 4, shadowColor);
}

void drawFieldBorder(int x, int y, int w, int h, int radius, uint16_t borderColor) {
    M5.Lcd.drawRoundRect(x, y, w, h, radius, borderColor);
    M5.Lcd.drawRoundRect(x + 1, y + 1, w - 2, h - 2, radius - 1, borderColor);
    M5.Lcd.drawRoundRect(x + 2, y + 2, w - 4, h - 4, radius - 2, borderColor);
    M5.Lcd.drawRoundRect(x + 3, y + 3, w - 6, h - 6, radius - 3, borderColor);
}

void fillGradientRoundRect(int x, int y, int w, int h, int radius, uint16_t colorStart, uint16_t colorEnd) {
    if (h <= 0 || w <= 0 || radius <= 0) return;

    uint8_t rStart = (colorStart >> 11) & 0x1F;
    uint8_t gStart = (colorStart >> 5) & 0x3F;
    uint8_t bStart = colorStart & 0x1F;
    uint8_t rEnd = (colorEnd >> 11) & 0x1F;
    uint8_t gEnd = (colorEnd >> 5) & 0x3F;
    uint8_t bEnd = colorEnd & 0x1F;

    radius = max(radius - 4, 0);
    int arcWidths[11];
    for (int dy = 0; dy <= radius && dy < 11; dy++) {
        arcWidths[dy] = (int)sqrt((float)(radius * radius - dy * dy));
    }

    x += 4; y += 4; w -= 8; h -= 8;

    for (int i = 0; i < h; i++) {
        float ratio = (float)i / (h - 1.0);
        uint16_t gradColor = (uint16_t)(
            ((rStart + (uint8_t)((rEnd - rStart) * ratio + 0.5f)) << 11) |
            ((gStart + (uint8_t)((gEnd - gStart) * ratio + 0.5f)) << 5) |
            (bStart + (uint8_t)((bEnd - bStart) * ratio + 0.5f))
        );

        int lineStartX, lineLen;
        if (i < radius) {
            lineStartX = x + radius - arcWidths[i];
            lineLen = w - 2 * (radius - arcWidths[i]);
        } else if (i >= h - radius) {
            int dy = h - 1 - i;
            lineStartX = x + radius - arcWidths[dy];
            lineLen = w - 2 * (radius - arcWidths[dy]);
        } else {
            lineStartX = x;
            lineLen = w;
        }

        if (lineStartX < 0) { lineLen += lineStartX; lineStartX = 0; }
        if (lineLen > 0 && lineStartX + lineLen > 320) lineLen = 320 - lineStartX;
        if (lineLen > 0) {
            M5.Lcd.drawFastHLine(lineStartX, y + i, lineLen, gradColor);
        }
    }
}

// PV-Feld jetzt mit int + kW-Anzeige
void drawPVField(int value, bool forceRedraw) {
    static int oldValue = -99999;
    if (!forceRedraw && value == oldValue) return;
    oldValue = value;

    int x = 5, y = 5, w = 310, h = 75, r = 10;
    drawFieldShadow(x, y, w, h, r, TFT_BLUE);
    fillGradientRoundRect(x, y, w, h, r, TFT_CYAN, TFT_BLUE);
    drawFieldBorder(x, y, w, h, r, TFT_DARKCYAN);

    M5.Lcd.setTextColor(TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.drawCentreString(F("PV Leistung"), 160, 12, 1);

    char buf[12];
    if (value >= 1000) {
        snprintf(buf, sizeof(buf), "%.1f kW", value / 1000.0f);
    } else {
        snprintf(buf, sizeof(buf), "%d W", value);
    }
    M5.Lcd.drawCentreString(buf, 160, 40, 2);
}

void drawNetzField(const char* value, int intValue, bool forceRedraw) {
    static char oldValue[FIELD_BUFFER_SIZE] = "";
    if (!forceRedraw && strcmp(value, oldValue) == 0) return;
    strncpy(oldValue, value, FIELD_BUFFER_SIZE);

    int x = 5, y = 85, w = 147, h = 80, r = 8;
    drawFieldShadow(x, y, w, h, r, TFT_DARKGREEN);
    fillGradientRoundRect(x, y, w, h, r, TFT_GREEN, TFT_GREENYELLOW);
    uint16_t border = (intValue > 500) ? TFT_RED : TFT_DARKGREEN;
    drawFieldBorder(x, y, w, h, r, border);

    M5.Lcd.setTextColor(TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.drawCentreString(F("Netz [W]"), 78, 92, 1);
    char buf[FIELD_BUFFER_SIZE];
    snprintf(buf, FIELD_BUFFER_SIZE, "%s W", value);
    M5.Lcd.drawCentreString(buf, 78, 118, 2);
}

void drawAkkuField(const char* value, int intValue, bool forceRedraw) {
    static char oldValue[FIELD_BUFFER_SIZE] = "";
    if (!forceRedraw && strcmp(value, oldValue) == 0) return;
    strncpy(oldValue, value, FIELD_BUFFER_SIZE);

    int x = 165, y = 85, w = 147, h = 80, r = 8;
    drawFieldShadow(x, y, w, h, r, TFT_DARKGREY);
    fillGradientRoundRect(x, y, w, h, r, TFT_ORANGE, TFT_YELLOW);
    M5.Lcd.setTextColor(TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.drawCentreString(F("Akku [W]"), 238, 92, 1);
    char buf[FIELD_BUFFER_SIZE];
    snprintf(buf, FIELD_BUFFER_SIZE, "%s W", value);
    M5.Lcd.drawCentreString(buf, 238, 118, 2);
    uint16_t border = (intValue > 0) ? TFT_GREEN : TFT_ORANGE;
    drawFieldBorder(x, y, w, h, r, border);
}

void drawBatteryField(const char* value, int intValue, bool forceRedraw) {
    static char oldValue[FIELD_BUFFER_SIZE] = "";
    if (!forceRedraw && strcmp(value, oldValue) == 0) return;
    strncpy(oldValue, value, FIELD_BUFFER_SIZE);

    int x = 5, y = 170, w = 147, h = 65, r = 8;
    drawFieldShadow(x, y, w, h, r, TFT_DARKGREY);
    fillGradientRoundRect(x, y, w, h, r, TFT_YELLOW, TFT_WHITE);
    uint16_t border = (intValue > 75) ? TFT_GREEN : (intValue > 25) ? TFT_YELLOW : TFT_RED;
    drawFieldBorder(x, y, w, h, r, border);

    M5.Lcd.setTextColor(TFT_BLACK);
    M5.Lcd.setTextSize(1.5);
    M5.Lcd.drawCentreString(F("Akku Level [%]"), 78, 177, 2);
    char buf[FIELD_BUFFER_SIZE];
    snprintf(buf, FIELD_BUFFER_SIZE, "%s%%", value);
    M5.Lcd.setTextSize(2);
    M5.Lcd.drawCentreString(buf, 78, 200, 2);
}

void drawAutarkyField(const char* value, bool forceRedraw) {
    static char oldValue[FIELD_BUFFER_SIZE] = "";
    if (!forceRedraw && strcmp(value, oldValue) == 0) return;
    strncpy(oldValue, value, FIELD_BUFFER_SIZE);

    int x = 165, y = 170, w = 147, h = 65, r = 8;
    drawFieldShadow(x, y, w, h, r, TFT_PURPLE);
    fillGradientRoundRect(x, y, w, h, r, TFT_MAGENTA, TFT_PINK);
    drawFieldBorder(x, y, w, h, r, TFT_PURPLE);

    M5.Lcd.setTextColor(TFT_BLACK);
    M5.Lcd.setTextSize(1.5);
    M5.Lcd.drawCentreString(F("Autarkie [%]"), 238, 177, 2);
    char buf[FIELD_BUFFER_SIZE];
    snprintf(buf, FIELD_BUFFER_SIZE, "%s%%", value);
    M5.Lcd.setTextSize(2);
    M5.Lcd.drawCentreString(buf, 238, 200, 2);
}

void updateDisplay() {
    int newGeneration = atoi(generationPower);
    int newGrid = atoi(gridPower);
    int newAccu = atoi(accuPower);
    int newBattery = atoi(batteryLevelPercent);
    int newAutarky = atoi(autarkyPercent);

    if (newGeneration != old_generation) {
        drawPVField(newGeneration);
        old_generation = newGeneration;
    }
    if (newGrid != old_gridPower) {
        drawNetzField(gridPower, newGrid);
        old_gridPower = newGrid;
    }
    if (newAccu != old_accuPower) {
        drawAkkuField(accuPower, newAccu);
        old_accuPower = newAccu;
    }
    if (newBattery != old_batteryLevel) {
        drawBatteryField(batteryLevelPercent, newBattery);
        old_batteryLevel = newBattery;
    }
    if (newAutarky != old_autarky) {
        drawAutarkyField(autarkyPercent);
        old_autarky = newAutarky;
    }
}

bool isValidNumber(const char* str) {
    if (!str || *str == '\0') return false;
    bool hasDigit = false;
    for (int i = 0; str[i]; i++) {
        char c = str[i];
        if (isdigit(c)) { hasDigit = true; continue; }
        if (c == '.' && i > 0 && str[i + 1]) continue;
        if (c == '-' && i == 0) continue;
        return false;
    }
    return hasDigit;
}

void callbackMqttReceive(char* topic, byte* payload, unsigned int length) {
    char message[MQTT_BUFFER_SIZE] = {0};
    if (length >= MQTT_BUFFER_SIZE) length = MQTT_BUFFER_SIZE - 1;
    memcpy(message, payload, length);

    DEBUG_PRINTF("MQTT %s: %s\n", topic, message);
    if (!isValidNumber(message)) {
        DEBUG_PRINT(F("Invalid payload, skipping.\n"));
        return;
    }

    struct TopicMap {
        const char* topic;
        char* target;
    } topicMap[] = {
        {"VenusData/Autarkie_heute", autarkyPercent},
        {"VenusData/Ladezustand",    batteryLevelPercent},
        {"PV/grid_powerFast",       gridPower},
        {"PV/generationPower",      generationPower},
        {"VenusData/PowerShelly",   accuPower}
    };

    for (auto& map : topicMap) {
        if (strcmp(topic, map.topic) == 0) {
            strncpy(map.target, message, FIELD_BUFFER_SIZE - 1);
            map.target[FIELD_BUFFER_SIZE - 1] = '\0';
            break;
        }
    }
}

void reconnectWifi() {
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck < WIFI_RECONNECT_INTERVAL) return;
    lastCheck = millis();

    if (WiFi.status() != WL_CONNECTED) {
        DEBUG_PRINT(F("WiFi lost, reconnecting...\n"));
        WiFi.disconnect();
        WiFi.begin(SSID, WIFI_PASSWORD);

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 10) {
            delay(500);
            DEBUG_PRINT(".");
            attempts++;
            esp_task_wdt_reset();
        }

        if (WiFi.status() == WL_CONNECTED) {
            DEBUG_PRINTF("\nWiFi reOK: %s\n", WiFi.localIP().toString().c_str());
            drawGrid();
        } else {
            DEBUG_PRINT(F("\nWiFi reconnect fail! Restart...\n"));
            delay(5000);
            ESP.restart();
        }
    }
}

void reconnectMQTT() {
    static int backoff = 1000;
    String clientId = "M5Stack-" + WiFi.macAddress();

    int attempts = 0;
    while (!mqttClient.connected() && attempts < 3) {
        DEBUG_PRINTF("MQTT attempt %d...\n", attempts + 1);
        mqttClient.disconnect();
        delay(backoff);
        backoff = min(backoff * 2, MQTT_BACKOFF_MAX);

        if (mqttClient.connect(clientId.c_str())) {
            DEBUG_PRINT(F("MQTT connected\n"));
            const char* topics[] = {
                "VenusData/Autarkie_heute", "VenusData/Ladezustand",
                "PV/grid_powerFast", "PV/generationPower", "VenusData/PowerShelly"
            };
            for (int i = 0; i < 5; i++) mqttClient.subscribe(topics[i]);
            backoff = 1000;
            return;
        }
        DEBUG_PRINTF("MQTT fail, rc=%d\n", mqttClient.state());
        attempts++;
        esp_task_wdt_reset();
    }
    DEBUG_PRINT(F("MQTT reconnect fail! Restart...\n"));
    delay(10000);
    ESP.restart();
}