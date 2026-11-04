// ============================================================
// Smart Bedroom Climate Control - 
// ESP32 + DHT22 + LDR + PIR + LCD 1602 I2C
// ============================================================
#include <Arduino.h>
#include <stdarg.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include "secrets.h"

// ===================== KONFIGURÁCIA =========================
// --- Piny ---
const uint8_t PIN_DHT        = 4;
const uint8_t PIN_LDR        = 34;
const uint8_t PIN_PIR        = 27;
const uint8_t PIN_BTN_MODE   = 13;
const uint8_t PIN_BTN_SCREEN = 14;
const uint8_t PIN_BTN_FAN    = 33;
const uint8_t PIN_FAN        = 26;
const uint8_t PIN_LIGHT      = 25;
const uint8_t PIN_LED_GREEN  = 16;
const uint8_t PIN_LED_YELLOW = 17;
const uint8_t PIN_LED_RED    = 18;

// --- Teplota (°C) ---
const float TEMP_FAN_ON  = 28.0;
const float TEMP_FAN_OFF = 26.0;
const float TEMP_WARN    = 27.0;
const float TEMP_CRIT    = 32.0;
const float CRIT_HYST    = 1.0;   // návrat z kritického stavu

// --- Vlhkosť (%) ---
const float HUM_MIN  = 30.0;
const float HUM_MAX  = 70.0;
const float HUM_HYST = 3.0;       // návrat do normy o 3 % ďalej od hranice

// --- Svetlo ---
const int  DARK_THRESHOLD_PCT = 30;
const bool LDR_HIGH_MEANS_DARK = true;
const unsigned long LIGHT_TIMEOUT_MS = 10000;

// --- Časovanie ---
const unsigned long SENSOR_INTERVAL_MS  = 2000;
const unsigned long DISPLAY_INTERVAL_MS = 300;
const unsigned long SENSOR_FAIL_MS      = 60000;
const unsigned long DEBOUNCE_MS         = 40;
const unsigned long SCREEN_ROTATE_MS    = 4000;

// --- Wi-Fi a Discord ---
#define DISCORD_DRY_RUN 0                 // 1 = len výpis do Serial Monitora, 0 = skutočné odosielanie
const unsigned long WIFI_RETRY_MS     = 15000;
const unsigned long LONG_HIGH_MS      = 120000;  // "dlhodobo vysoká teplota" (2 min, na test)
const int           DISCORD_QUEUE_LEN = 8;
const int           DISCORD_MAX_TRIES = 3;
const unsigned long DISCORD_RETRY_MS  = 5000;
const char* TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3";   // časová zóna Slovensko

// Typy upozornení: zapnuté/vypnuté 
enum EventType {
  EV_STARTUP, EV_TEMP_HIGH, EV_TEMP_OK, EV_CRITICAL, EV_HUM_BAD, EV_HUM_OK,
  EV_FAN, EV_MOTION, EV_SENSOR_ERR, EV_SENSOR_OK, EV_LONG_HIGH, EV_COUNT
};
//                                    START  T_HIGH T_OK  CRIT  H_BAD H_OK  FAN   MOTION S_ERR S_OK  LONG
const bool NOTIFY_ENABLED[EV_COUNT] = {true, true,  true, true, true, true, true, false, true, true, true};
const unsigned long COOLDOWN_MS[EV_COUNT] =
                                      {0,    60000, 0,    60000,60000,0,    30000,60000, 60000,0,    300000};

// ===================== OBJEKTY A STAV =======================
DHT dht(PIN_DHT, DHT22);
LiquidCrystal_I2C lcd(0x27, 16, 2);

float temperature = NAN;
float humidity    = NAN;
int   lightPct    = 0;
bool  motion      = false;
bool  isDark      = false;
bool  sensorError = false;

bool autoMode = true;
bool fanOn    = false;
bool lightOn  = false;
uint8_t screen = 0;

unsigned long lastSensorRead   = 0;
unsigned long lastGoodDht      = 0;
unsigned long lastMotion       = 0;
unsigned long lastDisplay      = 0;
unsigned long lastScreenSwitch = 0;

char lcdCache[2][17];

// --- Stav Wi-Fi a Discordu ---
struct DiscordMsg { char text[200]; };
QueueHandle_t discordQueue;
volatile uint16_t sentOk = 0, sentFail = 0, droppedMsgs = 0;
unsigned long lastSent[EV_COUNT] = {0};
bool wifiOk = false;
unsigned long lastWifiTry = 0;

// Pamäť na detekciu zmeny stavu (aby sa nepísalo pri každom meraní)
bool nTempHigh = false, nCritical = false, nHumBad = false;
bool nFan = false, nMotion = false, nSensorErr = false;
unsigned long nTempHighSince = 0;

// ===================== TLAČIDLÁ =============================
struct Button { uint8_t pin; bool stable; bool lastRaw; unsigned long changedAt; };
Button btnMode   = {PIN_BTN_MODE,   HIGH, HIGH, 0};
Button btnScreen = {PIN_BTN_SCREEN, HIGH, HIGH, 0};
Button btnFan    = {PIN_BTN_FAN,    HIGH, HIGH, 0};

// Vráti true jednorazovo pri stlačení (po debounce)
bool buttonPressed(Button &b) {
  bool raw = digitalRead(b.pin);
  if (raw != b.lastRaw) { b.lastRaw = raw; b.changedAt = millis(); }
  if (millis() - b.changedAt > DEBOUNCE_MS && raw != b.stable) {
    b.stable = raw;
    if (raw == LOW) return true;
  }
  return false;
}

// ===================== POMOCNÉ FUNKCIE (DISCORD) ============

void jsonEscape(const char* src, char* dst, size_t n) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 7 < n; i++) {
    unsigned char c = src[i];
    if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = c; }
    else if (c == '\n')        { dst[j++] = '\\'; dst[j++] = 'n'; }
    else if (c < 0x20)         { /* ostatné riadiace znaky preskočíme */ }
    else                       { dst[j++] = c; }
  }
  dst[j] = 0;
}

// Aktuálny čas (NTP) alebo uptime, ak čas ešte nie je dostupný
void timeStr(char* out, size_t n) {
  struct tm ti;
  if (getLocalTime(&ti, 0)) strftime(out, n, "%d.%m.%Y %H:%M:%S", &ti);
  else snprintf(out, n, "uptime %lus", millis() / 1000);
}

// Vloží upozornenie do fronty, ak je typ zapnutý a neprebieha cooldown.
// Nikdy neblokuje (čakanie 0).
void notify(EventType type, const char* fmt, ...) {
  if (!NOTIFY_ENABLED[type]) return;
  unsigned long now = millis();
  if (COOLDOWN_MS[type] > 0 && lastSent[type] != 0 &&
      now - lastSent[type] < COOLDOWN_MS[type]) return;

  char body[150], ts[32];
  va_list args;
  va_start(args, fmt);
  vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);
  timeStr(ts, sizeof(ts));

  DiscordMsg m;
  snprintf(m.text, sizeof(m.text), "%s\n🕒 %s", body, ts);
  if (xQueueSend(discordQueue, &m, 0) == pdTRUE) {
    lastSent[type] = (now == 0) ? 1 : now;
  } else {
    droppedMsgs++;   
  }
}

// Jedno HTTPS POST volanie na Discord webhook
bool postToDiscord(const char* text) {
  char esc[420];
  jsonEscape(text, esc, sizeof(esc));
  String payload = String("{\"username\":\"Smart Bedroom\",\"content\":\"") + esc + "\"}";

  WiFiClientSecure client;
  client.setInsecure();            
  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(client, DISCORD_WEBHOOK_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  http.end();
  Serial.printf("Discord HTTP kod: %d\n", code);
  return code == 200 || code == 204;   
}

// Samostatná úloha: berie správy z fronty a odosiela ich po jednej.
// Beží na jadre 0, takže loop() sa nikdy nezastaví.
void discordTask(void*) {
  DiscordMsg m;
  for (;;) {
    if (xQueuePeek(discordQueue, &m, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
#if DISCORD_DRY_RUN
    Serial.printf("[DRY-RUN] %s\n", m.text);
    xQueueReceive(discordQueue, &m, 0);
    sentOk++;
    vTaskDelay(pdMS_TO_TICKS(500));
#else
    if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
    bool ok = false;
    for (int t = 0; t < DISCORD_MAX_TRIES && !ok; t++) {
      ok = postToDiscord(m.text);
      if (!ok) vTaskDelay(pdMS_TO_TICKS(DISCORD_RETRY_MS));
    }
    xQueueReceive(discordQueue, &m, 0);   // správu odstránime (aj po vzdaní sa)
    if (ok) sentOk++; else sentFail++;
    vTaskDelay(pdMS_TO_TICKS(1000));      // šetrenie rate-limitu Discordu
#endif
  }
}

// ===================== Wi-Fi ================================
// Spustí pripájanie (neblokuje)
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
  lastWifiTry = millis();
}

// Sleduje stav Wi-Fi a po výpadku skúša pripojenie obnoviť
void handleWiFi() {
  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected && !wifiOk) {
    Serial.println("Wi-Fi pripojene");
    configTzTime(TZ_INFO, "pool.ntp.org");
  }
  if (!connected && wifiOk) Serial.println("Wi-Fi odpojene");
  wifiOk = connected;
  if (!connected && millis() - lastWifiTry > WIFI_RETRY_MS) {
    lastWifiTry = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
  }
}

// ===================== SENZORY ==============================
void readSensors() {
  unsigned long now = millis();

  motion = digitalRead(PIN_PIR) == HIGH;
  if (motion) lastMotion = now;

  int raw = analogRead(PIN_LDR);
  int pct = map(raw, 0, 4095, 0, 100);
  lightPct = LDR_HIGH_MEANS_DARK ? 100 - pct : pct;
  isDark = lightPct < DARK_THRESHOLD_PCT;

  if (now - lastSensorRead >= SENSOR_INTERVAL_MS) {
    lastSensorRead = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (isnan(t) || isnan(h)) {
      Serial.println("DHT22: chyba citania");
    } else {
      temperature = t; humidity = h; lastGoodDht = now;
    }
    Serial.printf("T=%.1f H=%.1f L=%d%% PIR=%d fan=%d light=%d %s\n",
                  temperature, humidity, lightPct, motion, fanOn, lightOn,
                  autoMode ? "AUTO" : "MANUAL");
  }
  sensorError = (now - lastGoodDht > SENSOR_FAIL_MS);
}

// ===================== VENTILÁTOR ===========================
void updateClimateControl() {
  if (!autoMode) return;
  if (sensorError)                        fanOn = true;    // fail-safe
  else if (temperature >= TEMP_FAN_ON)    fanOn = true;
  else if (temperature <= TEMP_FAN_OFF)   fanOn = false;
}

// ===================== SVETLO ===============================
void updateLighting() {
  if (!autoMode) return;
  unsigned long now = millis();
  if (!lightOn) {
    if (isDark && motion) lightOn = true;
  } else {
    if (!isDark || (now - lastMotion > LIGHT_TIMEOUT_MS)) lightOn = false;
  }
}

// ===================== TLAČIDLÁ - LOGIKA ====================
void handleButtons() {
  if (buttonPressed(btnMode)) {
    autoMode = !autoMode;
    Serial.println(autoMode ? "BTN1: AUTO" : "BTN1: MANUAL");
  }
  if (buttonPressed(btnScreen)) {
    Serial.println("BTN2 stlacene");
    if (autoMode) { screen = (screen + 1) % 3; lastScreenSwitch = millis(); }
    else          { lightOn = !lightOn; }
  }
  if (buttonPressed(btnFan)) {
    Serial.println("BTN3 stlacene");
    if (!autoMode) fanOn = !fanOn;
  }
}

// ===================== DISCORD UPOZORNENIA ==================
// Porovná aktuálny stav s predchádzajúcim a pri ZMENE pošle upozornenie
void handleDiscordNotifications() {
  unsigned long now = millis();

  // Chyba senzora a jeho obnovenie
  if (sensorError && !nSensorErr) {
    nSensorErr = true;
    notify(EV_SENSOR_ERR, " Chyba senzora DHT22 – žiadne platné dáta. Ventilátor je zapnutý (fail-safe).");
  } else if (!sensorError && nSensorErr) {
    nSensorErr = false;
    notify(EV_SENSOR_OK, " Senzor DHT22 opäť funguje.");
  }

  if (!sensorError && !isnan(temperature) && !isnan(humidity)) {
    // Vysoká teplota (hranice rovnaké ako pre ventilátor)
    if (!nTempHigh && temperature >= TEMP_FAN_ON) {
      nTempHigh = true; nTempHighSince = now;
      notify(EV_TEMP_HIGH, " Vysoká teplota: %.1f °C (limit %.1f °C).", temperature, TEMP_FAN_ON);
    } else if (nTempHigh && temperature <= TEMP_FAN_OFF) {
      nTempHigh = false;
      notify(EV_TEMP_OK, " Teplota sa vrátila do normy: %.1f °C.", temperature);
    }
    // Dlhodobo vysoká teplota (opakovanie riadi cooldown)
    if (nTempHigh && now - nTempHighSince >= LONG_HIGH_MS) {
      notify(EV_LONG_HIGH, " Teplota je dlhodobo vysoká: %.1f °C.", temperature);
    }
    // Kritický stav
    if (!nCritical && temperature >= TEMP_CRIT) {
      nCritical = true;
      notify(EV_CRITICAL, " KRITICKÝ STAV: teplota %.1f °C.", temperature);
    } else if (nCritical && temperature <= TEMP_CRIT - CRIT_HYST) {
      nCritical = false;
    }
    // Vlhkosť
    bool humOut = (humidity < HUM_MIN || humidity > HUM_MAX);
    bool humBack = (humidity >= HUM_MIN + HUM_HYST && humidity <= HUM_MAX - HUM_HYST);
    if (!nHumBad && humOut) {
      nHumBad = true;
      notify(EV_HUM_BAD, " Vlhkosť mimo rozsahu: %.0f %% (povolené %.0f–%.0f %%).", humidity, HUM_MIN, HUM_MAX);
    } else if (nHumBad && humBack) {
      nHumBad = false;
      notify(EV_HUM_OK, " Vlhkosť sa vrátila do normy: %.0f %%.", humidity);
    }
  }

  // Zmena stavu ventilátora
  if (fanOn != nFan) {
    nFan = fanOn;
    notify(EV_FAN, fanOn ? " Ventilátor ZAPNUTÝ (%.1f °C)." : " Ventilátor VYPNUTÝ (%.1f °C).", temperature);
  }
  // Pohyb 
  if (motion && !nMotion) notify(EV_MOTION, "🚶 Zaznamenaný pohyb v miestnosti.");
  nMotion = motion;
}

// ===================== VÝSTUPY ==============================
void updateOutputs() {
  digitalWrite(PIN_FAN, fanOn);
  digitalWrite(PIN_LIGHT, lightOn);
}

void updateStatusLeds() {
  bool red = sensorError || (!isnan(temperature) && temperature >= TEMP_CRIT);
  bool yellow = !red && !isnan(temperature) &&
                (temperature >= TEMP_WARN || humidity < HUM_MIN || humidity > HUM_MAX);
  digitalWrite(PIN_LED_RED, red);
  digitalWrite(PIN_LED_YELLOW, yellow);
  digitalWrite(PIN_LED_GREEN, !red && !yellow);
}

// ===================== LCD ==================================
void lcdLine(uint8_t row, const char *text) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);
  if (strcmp(buf, lcdCache[row]) != 0) {
    lcd.setCursor(0, row);
    lcd.print(buf);
    strcpy(lcdCache[row], buf);
  }
}

void updateDisplay() {
  if (millis() - lastDisplay < DISPLAY_INTERVAL_MS) return;
  lastDisplay = millis();

  if (millis() - lastScreenSwitch > SCREEN_ROTATE_MS) {
    screen = (screen + 1) % 3;
    lastScreenSwitch = millis();
  }

  char l1[24], l2[24];
  if (screen == 0) {
    if (sensorError || isnan(temperature)) snprintf(l1, sizeof(l1), "DHT22 CHYBA");
    else snprintf(l1, sizeof(l1), "T:%.1fC H:%.0f%%", temperature, humidity);
    snprintf(l2, sizeof(l2), "Sv:%3d%% Poh:%s", lightPct, motion ? "ANO" : "NIE");
  } else if (screen == 1) {
    snprintf(l1, sizeof(l1), "Rezim: %s", autoMode ? "AUTO" : "MANUAL");
    snprintf(l2, sizeof(l2), "Vent:%s Sv:%s", fanOn ? "ON" : "OFF", lightOn ? "ON" : "OFF");
  } else {
    snprintf(l1, sizeof(l1), "WiFi:%s Posl:%u", wifiOk ? "OK" : "--", (unsigned)sentOk);
#if DISCORD_DRY_RUN
    snprintf(l2, sizeof(l2), "DRY-RUN Fr:%u", (unsigned)uxQueueMessagesWaiting(discordQueue));
#else
    snprintf(l2, sizeof(l2), "Chyb:%u Fr:%u", (unsigned)sentFail, (unsigned)uxQueueMessagesWaiting(discordQueue));
#endif
  }
  lcdLine(0, l1);
  lcdLine(1, l2);
}

// ===================== SETUP / LOOP =========================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_FAN, OUTPUT);
  pinMode(PIN_LIGHT, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  digitalWrite(PIN_FAN, LOW);
  digitalWrite(PIN_LIGHT, LOW);

  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_BTN_MODE, INPUT_PULLUP);
  pinMode(PIN_BTN_SCREEN, INPUT_PULLUP);
  pinMode(PIN_BTN_FAN, INPUT_PULLUP);

  dht.begin();
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcdCache[0][0] = lcdCache[1][0] = '\0';
  lcdLine(0, "Climate Control");
  lcdLine(1, "Spustam...");

  
  discordQueue = xQueueCreate(DISCORD_QUEUE_LEN, sizeof(DiscordMsg));
  xTaskCreatePinnedToCore(discordTask, "discord", 10240, NULL, 1, NULL, 0);

  connectWiFi();
  lastGoodDht = millis();
  notify(EV_STARTUP, "Climate Control sa spustil.");
  delay(1500);
}

void loop() {
  handleWiFi();
  readSensors();
  handleButtons();
  updateClimateControl();
  updateLighting();
  handleDiscordNotifications();
  updateOutputs();
  updateStatusLeds();
  updateDisplay();
}
