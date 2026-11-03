// ============================================================
// Smart Bedroom Climate Control - Fáza 2 (jadro bez Wi-Fi)
// ESP32 + DHT22 + LDR + PIR + LCD 1602 I2C
// ============================================================
#include <Arduino.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ===================== KONFIGURÁCIA =========================
// --- Piny ---
const uint8_t PIN_DHT        = 4;
const uint8_t PIN_LDR        = 34;   // ADC1, funguje aj pri zapnutej Wi-Fi
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
const float TEMP_FAN_ON  = 28.0;  // zapnutie ventilátora
const float TEMP_FAN_OFF = 26.0;  // vypnutie (hysteréza = 2 °C)
const float TEMP_WARN    = 27.0;  // žltá LED
const float TEMP_CRIT    = 32.0;  // červená LED

// --- Vlhkosť (%) ---
const float HUM_MIN = 30.0;
const float HUM_MAX = 70.0;

// --- Svetlo ---
const int  DARK_THRESHOLD_PCT = 30;       // pod touto hodnotou = tma
const bool LDR_HIGH_MEANS_DARK = true;   // ak je vo Wokwi opačne, daj true
const unsigned long LIGHT_TIMEOUT_MS = 10000;  // 10 s bez pohybu -> vypnúť

// --- Časovanie ---
const unsigned long SENSOR_INTERVAL_MS  = 2000;   // DHT22 max ~0,5 Hz
const unsigned long DISPLAY_INTERVAL_MS = 300;
const unsigned long SENSOR_FAIL_MS      = 60000;  // po 60 s bez dát = chyba
const unsigned long DEBOUNCE_MS         = 40;

// ===================== OBJEKTY A STAV =======================
DHT dht(PIN_DHT, DHT22);
LiquidCrystal_I2C lcd(0x27, 16, 2);

float temperature = NAN;
float humidity    = NAN;
int   lightPct    = 0;        // 0-100 %, relatívna úroveň (NIE luxy)
bool  motion      = false;
bool  isDark      = false;
bool  sensorError = false;

bool autoMode = true;
bool fanOn    = false;
bool lightOn  = false;
uint8_t screen = 0;

unsigned long lastSensorRead = 0;
unsigned long lastGoodDht    = 0;
unsigned long lastMotion     = 0;
unsigned long lastDisplay    = 0;

char lcdCache[2][17];         // čo je práve na LCD (kvôli blikaniu)

// ===================== TLAČIDLÁ =============================
struct Button {
  uint8_t pin;
  bool stable;
  bool lastRaw;
  unsigned long changedAt;
};
Button btnMode   = {PIN_BTN_MODE,   HIGH, HIGH, 0};
Button btnScreen = {PIN_BTN_SCREEN, HIGH, HIGH, 0};
Button btnFan    = {PIN_BTN_FAN,    HIGH, HIGH, 0};

// Vráti true jednorazovo pri stlačení (po debounce)
bool buttonPressed(Button &b) {
  bool raw = digitalRead(b.pin);
  if (raw != b.lastRaw) {
    b.lastRaw = raw;
    b.changedAt = millis();
  }
  if (millis() - b.changedAt > DEBOUNCE_MS && raw != b.stable) {
    b.stable = raw;
    if (raw == LOW) return true;   // pull-up: stlačené = LOW
  }
  return false;
}

// ===================== SENZORY ==============================
// Číta DHT22 (raz za interval), LDR a PIR
void readSensors() {
  unsigned long now = millis();

  // PIR a LDR môžeme čítať stále
  motion = digitalRead(PIN_PIR) == HIGH;
  if (motion) lastMotion = now;

  int raw = analogRead(PIN_LDR);              // 0-4095
  int pct = map(raw, 0, 4095, 0, 100);
  lightPct = LDR_HIGH_MEANS_DARK ? 100 - pct : pct;
  isDark = lightPct < DARK_THRESHOLD_PCT;

  // DHT22 len raz za SENSOR_INTERVAL_MS
  if (now - lastSensorRead >= SENSOR_INTERVAL_MS) {
    lastSensorRead = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (isnan(t) || isnan(h)) {
      Serial.println("DHT22: chyba citania");
    } else {
      temperature = t;
      humidity = h;
      lastGoodDht = now;
    }
    Serial.printf("T=%.1f H=%.1f L=%d%% PIR=%d fan=%d light=%d %s\n",
                  temperature, humidity, lightPct, motion, fanOn, lightOn,
                  autoMode ? "AUTO" : "MANUAL");
  }
  // Chyba senzora = dlho žiadne platné dáta
  sensorError = (now - lastGoodDht > SENSOR_FAIL_MS);
}

// ===================== VENTILÁTOR ===========================
// AUTO: hysteréza. Pri chybe senzora fail-safe = ventilátor ZAPNUTÝ.
void updateClimateControl() {
  if (!autoMode) return;   // v MANUAL ventilátor ovláda len tlačidlo

  if (sensorError) {
    fanOn = true;
  } else if (temperature >= TEMP_FAN_ON) {
    fanOn = true;
  } else if (temperature <= TEMP_FAN_OFF) {
    fanOn = false;
  }
  // medzi OFF a ON hranicou stav ostáva, ako bol
}

// ===================== SVETLO ===============================
// AUTO: tma + pohyb -> zapnúť, po timeoute bez pohybu -> vypnúť
void updateLighting() {
  if (!autoMode) return;   // v MANUAL nič neprepisuj

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
    Serial.println(autoMode ? "Rezim: AUTO" : "Rezim: MANUAL");
  }
  if (buttonPressed(btnScreen)) {
    if (autoMode) {
      screen = (screen + 1) % 2;      // v AUTO prepína obrazovky
    } else {
      lightOn = !lightOn;             // v MANUAL zapína/vypína svetlo
    }
  }
  if (buttonPressed(btnFan)) {
    if (!autoMode) fanOn = !fanOn;    // len v MANUAL
  }
}

// ===================== VÝSTUPY ==============================
void updateOutputs() {
  digitalWrite(PIN_FAN, fanOn);
  digitalWrite(PIN_LIGHT, lightOn);
}

// Zelená = OK, žltá = zvýšené hodnoty, červená = kritické/chyba
void updateStatusLeds() {
  bool red = sensorError || (!isnan(temperature) && temperature >= TEMP_CRIT);
  bool yellow = !red && !isnan(temperature) &&
                (temperature >= TEMP_WARN ||
                 humidity < HUM_MIN || humidity > HUM_MAX);
  digitalWrite(PIN_LED_RED, red);
  digitalWrite(PIN_LED_YELLOW, yellow);
  digitalWrite(PIN_LED_GREEN, !red && !yellow);
}

// ===================== LCD ==================================
// Prepíše riadok len ak sa text zmenil (nebliká)
void lcdLine(uint8_t row, const char *text) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);   // doplní medzerami na 16
  if (strcmp(buf, lcdCache[row]) != 0) {
    lcd.setCursor(0, row);
    lcd.print(buf);
    strcpy(lcdCache[row], buf);
  }
}

void updateDisplay() {
  if (millis() - lastDisplay < DISPLAY_INTERVAL_MS) return;
  lastDisplay = millis();

  char l1[20], l2[20];
  if (screen == 0) {
    if (sensorError || isnan(temperature)) {
      snprintf(l1, sizeof(l1), "DHT22 CHYBA");
    } else {
      snprintf(l1, sizeof(l1), "T:%.1fC H:%.0f%%", temperature, humidity);
    }
    snprintf(l2, sizeof(l2), "Sv:%3d%% Poh:%s", lightPct, motion ? "ANO" : "NIE");
  } else {
    snprintf(l1, sizeof(l1), "Rezim: %s", autoMode ? "AUTO" : "MANUAL");
    snprintf(l2, sizeof(l2), "Vent:%s Sv:%s", fanOn ? "ON" : "OFF", lightOn ? "ON" : "OFF");
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
  lcdLine(0, "climate control");
  lcdLine(1, "Spustam...");

  lastGoodDht = millis();   // dá DHT22 60 s na prvé platné čítanie
  delay(1500);
}

void loop() {
  readSensors();
  handleButtons();
  updateClimateControl();
  updateLighting();
  updateOutputs();
  updateStatusLeds();
  updateDisplay();
}
