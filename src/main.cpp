#include <M5Stack.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>
#include <math.h>

// Debugging toggle (set to 0 for production to reduce serial output)
#define DEBUG 1
#if DEBUG
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTF(x, ...) Serial.printf(x, ##__VA_ARGS__)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTF(x, ...)
#endif

// Constants
#define WATCHDOG_TIMEOUT 5
#define WIFI_PASSWORD "96031546242323421756"
#define MQTT_SERVER "192.168.178.44"
#define MQTT_PORT 1883
#define SSID "FRITZ!Box 7590 DG"
#define UPDATE_INTERVAL 500 // ms
#define WIFI_RECONNECT_INTERVAL 30000 // ms
#define MQTT_BACKOFF_MAX 10000 // ms
#define FIELD_BUFFER_SIZE 8 // Max 7 chars (e.g., "-1234 W")
#define MQTT_BUFFER_SIZE 32 // Reduced for MQTT payloads

// Function prototypes
void initWifi();
void callbackMqttReceive(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
void reconnectWifi();
void updateDisplay();
void drawGrid();
void drawPVField(float value, bool forceRedraw = false);
void drawNetzField(const char* value, int intValue, bool forceRedraw = false);
void drawAkkuField(const char* value, int intValue, bool forceRedraw = false);
void drawBatteryField(const char* value, int intValue, bool forceRedraw = false);
void drawAutarkyField(const char* value, bool forceRedraw = false);
void drawFieldShadow(int x, int y, int w, int h, int radius, uint16_t shadowColor);
void drawFieldBorder(int x, int y, int w, int h, int radius, uint16_t borderColor);
void fillGradientRoundRect(int x, int y, int w, int h, int radius, uint16_t colorStart, uint16_t colorEnd);

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

    DEBUG_PRINT(F("Setup complete. Press A to toggle test mode.\n"));
}

void loop() {
    esp_task_wdt_reset();
    M5.update();

    if (M5.BtnA.wasPressed()) {
        testMode = !testMode;
        DEBUG_PRINTF("Test mode: %s\n", testMode ? "ON" : "OFF");
    }

    if (testMode) {
        static uint32_t testTime = 0;
        if (millis() - testTime > 5000) {
            testTime = millis();
            snprintf(generationPower, FIELD_BUFFER_SIZE, "%ld", random(0, 10000));
            snprintf(gridPower, FIELD_BUFFER_SIZE, "%ld", random(-500, 1000));
            snprintf(accuPower, FIELD_BUFFER_SIZE, "%ld", random(-200, 500));
            snprintf(batteryLevelPercent, FIELD_BUFFER_SIZE, "%ld", random(20, 90));
            snprintf(autarkyPercent, FIELD_BUFFER_SIZE, "%ld", random(0, 100));
            DEBUG_PRINTF("TEST: PV=%sW, Netz=%sW, Akku=%sW, Batt=%s%%, Aut=%s%%\n",
                         generationPower, gridPower, accuPower, batteryLevelPercent, autarkyPercent);
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

    drawPVField(0.0, true);
    drawNetzField("0", 0, true);
    drawAkkuField("0", 0, true);
    drawBatteryField("0", 0, true);
    drawAutarkyField("0", true);
}

void drawFieldShadow(int x, int y, int w, int h, int radius, uint16_t shadowColor) {
    // Passe den Schatten an, um innerhalb des dickeren Rahmens zu bleiben
    M5.Lcd.fillRoundRect(x + 4, y + 4, w - 8, h - 8, radius - 4, shadowColor);
}

void drawFieldBorder(int x, int y, int w, int h, int radius, uint16_t borderColor) {
    // Zeichne vier Rahmenlinien für doppelte Linienstärke (ca. 4 Pixel)
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

    // Reduziere den Radius für die gefüllten Rechtecke, um innerhalb des Rahmens zu bleiben
    radius = max(radius - 4, 0); // Stelle sicher, dass der Radius nicht negativ wird
    int arcWidths[11];
    for (int dy = 0; dy <= radius && dy < 11; dy++) {
        arcWidths[dy] = (int)sqrt((float)(radius * radius - dy * dy));
    }

    // Verschiebe und verkleinere das gefüllte Rechteck, um Platz für den Rahmen zu lassen
    x += 4;
    y += 4;
    w -= 8;
    h -= 8;

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

        if (lineStartX < 0) {
            lineLen += lineStartX;
            lineStartX = 0;
        }
        if (lineLen > 0 && lineStartX + lineLen > 320) lineLen = 320 - lineStartX;
        if (lineLen > 0) {
            M5.Lcd.drawFastHLine(lineStartX, y + i, lineLen, gradColor);
        }
    }
}

void drawPVField(float value, bool forceRedraw) {
    static float oldValue = -1.0f;
    if (!forceRedraw && fabs(value - oldValue) < 0.1f) return;
    oldValue = value;

    int x = 5, y = 5, w = 310, h = 75, r = 10;
    drawFieldShadow(x, y, w, h, r, TFT_BLUE);
    fillGradientRoundRect(x, y, w, h, r, TFT_CYAN, TFT_BLUE);
    drawFieldBorder(x, y, w, h, r, TFT_DARKCYAN);

    M5.Lcd.setTextColor(TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.drawCentreString(F("PV Leistung [W]"), 160, 12, 1);
    char buf[FIELD_BUFFER_SIZE];
    snprintf(buf, FIELD_BUFFER_SIZE, "%.1f W", value);
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
    DEBUG_PRINTF("AkkuField: value=%s, intValue=%d, Border=%s\n", value, intValue, (intValue > 0) ? "GREEN" : "ORANGE");
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

    bool changed = false;
    if (newGeneration != old_generation) {
        drawPVField(newGeneration);
        old_generation = newGeneration;
        changed = true;
    }
    if (newGrid != old_gridPower) {
        drawNetzField(gridPower, newGrid);
        old_gridPower = newGrid;
        changed = true;
    }
    if (newAccu != old_accuPower) {
        drawAkkuField(accuPower, newAccu);
        old_accuPower = newAccu;
        changed = true;
    }
    if (newBattery != old_batteryLevel) {
        drawBatteryField(batteryLevelPercent, newBattery);
        old_batteryLevel = newBattery;
        changed = true;
    }
    if (newAutarky != old_autarky) {
        drawAutarkyField(autarkyPercent);
        old_autarky = newAutarky;
        changed = true;
    }

    if (changed) {
        DEBUG_PRINTF("UPDATE: PV=%.1fW, Netz=%sW, Akku=%sW, Batt=%s%%, Aut=%s%%\n",
                     newGeneration / 1000.0f, gridPower, accuPower, batteryLevelPercent, autarkyPercent);
    }
}

bool isValidNumber(const char* str) {
    if (!str || *str == '\0') return false;
    bool hasDigit = false;
    bool hasDot = false;
    for (int i = 0; str[i]; i++) {
        char c = str[i];
        if (isdigit(c)) { hasDigit = true; continue; }
        if (c == '.' && !hasDot && i > 0 && str[i + 1]) { hasDot = true; continue; }
        if (c == '-' && i == 0) continue;
        DEBUG_PRINTF("Invalid char '%c' at index %d\n", c, i);
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

    const char* targets[] = {
        "VenusData/Autarkie_heute", autarkyPercent,
        "VenusData/Ladezustand", batteryLevelPercent,
        "PV/grid_powerFast", gridPower,
        "PV/generationPower", generationPower,
        "VenusData/PowerShelly", accuPower
    };
    for (int i = 0; i < 5; i++) {
        if (strcmp(topic, targets[i * 2]) == 0) {
            strncpy((char*)targets[i * 2 + 1], message, FIELD_BUFFER_SIZE - 1);
            ((char*)targets[i * 2 + 1])[FIELD_BUFFER_SIZE - 1] = '\0';
            DEBUG_PRINTF("Updated %s: %s\n", targets[i * 2], message);
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