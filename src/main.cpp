#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// --- ESP32 Pin Settings ---
#define PIN_BATT_SENSE    34
#define PIN_CURR_SENSE    32
#define PIN_RELAY_HIGH    5   // Switches High Priority Relay & Green LED (Previously PIN_ALARM)
#define PIN_RELAY_MEDIUM  18  // Switches Medium Load Relay & Yellow LED
#define PIN_RELAY_LOW     19  // Switches Low Load Relay & Blue LED

// --- Telemetry Constants ---
const float ADC_RES = 4095.0f;
const float V_REF = 3.3f;
const float VOLT_DIVIDER = 6.0f; // Multiplier scales standard 3.3V max up to 19.8V max
const float MAX_CURRENT = 30.0f; // Scale mapping for simulated 0-30A load current

// --- Battery Management Thresholds (12V Nominal Battery) ---
const float BATT_CRITICAL   = 11.0f; // Cutoff for all non-essential loads
const float BATT_MEDIUM_OK   = 12.0f; // Threshold to permit Medium-Priority load
const float BATT_SUFFICIENT  = 13.0f; // Threshold to permit Low-Priority load
const float CURRENT_LIMIT    = 20.0f; // Overcurrent limit protection

LiquidCrystal_I2C lcd(0x27, 16, 2);
unsigned long lastUpdate = 0;

void setup() {
    Serial.begin(115200);
    
    // Define I/O states
    pinMode(PIN_RELAY_HIGH, OUTPUT);
    pinMode(PIN_RELAY_MEDIUM, OUTPUT);
    pinMode(PIN_RELAY_LOW, OUTPUT);

    // Initial output states (High Priority ON by default, non-essential OFF)
    digitalWrite(PIN_RELAY_HIGH, HIGH);
    digitalWrite(PIN_RELAY_MEDIUM, LOW);
    digitalWrite(PIN_RELAY_LOW, LOW);

    // Initialize display with standard ESP32 I2C pins (SDA=21, SCL=22)
    Wire.begin(21, 22);
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print("Priority System");
    lcd.setCursor(0, 1);
    lcd.print("ESP32 Initialized");
    delay(2000);
    lcd.clear();
}

void loop() {
    // Read raw 12-bit ADC values (0-4095)
    int rawVolt = analogRead(PIN_BATT_SENSE);
    int rawCurr = analogRead(PIN_CURR_SENSE);

    // Compute voltage and current metrics
    float battVoltage = (rawVolt / ADC_RES) * V_REF * VOLT_DIVIDER;
    float totalCurrent = (rawCurr / ADC_RES) * MAX_CURRENT;

    bool highLoadState = true;  // High load active by default
    bool mediumLoadState = false;
    bool lowLoadState = false;

    // --- Dynamic Priority Load Controller ---
    if (totalCurrent > CURRENT_LIMIT) {
        // OVERLOAD STATE: Shed non-essential loads immediately
        highLoadState = true; // Keep essential load running (or set false if full shutdown required)
        mediumLoadState = false;
        lowLoadState = false;
    } else {
        // VOLTAGE STATE: Analyze battery reserves
        if (battVoltage >= BATT_SUFFICIENT) {
            highLoadState = true;
            mediumLoadState = true;  // Low, Medium, and High are ON
            lowLoadState = true;
        } else if (battVoltage >= BATT_MEDIUM_OK) {
            highLoadState = true;
            mediumLoadState = true;  // Shed low-priority load
            lowLoadState = false;
        } else {
            // Below 12V: Shed medium and low priority loads
            highLoadState = true;
            mediumLoadState = false; 
            lowLoadState = false;
            
            // Critical low voltage cutoff (if high load must also turn off below BATT_CRITICAL)
            if (battVoltage < BATT_CRITICAL) {
                highLoadState = false; // Disconnect high priority load to protect battery
            }
        }
    }

    // Write physical states to pins
    digitalWrite(PIN_RELAY_HIGH, highLoadState ? HIGH : LOW);
    digitalWrite(PIN_RELAY_MEDIUM, mediumLoadState ? HIGH : LOW);
    digitalWrite(PIN_RELAY_LOW, lowLoadState ? HIGH : LOW);

    // Telemetry display cycle (once per second)
    if (millis() - lastUpdate >= 1000) {
        lastUpdate = millis();

        // Trace to serial console
        Serial.printf("Batt: %.2fV | Curr: %.2fA | High: %s | Med: %s | Low: %s\n",
                      battVoltage, totalCurrent,
                      highLoadState ? "ON" : "OFF",
                      mediumLoadState ? "ON" : "OFF",
                      lowLoadState ? "ON" : "OFF");

        // Write metrics on LCD Screen
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("V:");
        lcd.print(battVoltage, 1);
        lcd.print("V I:");
        lcd.print(totalCurrent, 1);
        lcd.print("A");

        lcd.setCursor(0, 1);
        lcd.print("H:");
        lcd.print(highLoadState ? "ON " : "OFF");
        lcd.print("M:");
        lcd.print(mediumLoadState ? "ON " : "OFF");
        lcd.print("L:");
        lcd.print(lowLoadState ? "ON" : "OFF");
    }
}