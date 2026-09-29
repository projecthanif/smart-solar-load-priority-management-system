// =============================================================================
//  Smart Solar Load Priority Management System
//
//  Sheds and restores three load tiers — CRITICAL, ESSENTIAL, NON-ESSENTIAL —
//  by battery state of charge, with hysteresis, confirmation delays, minimum
//  dwell, staggered switching and latched overcurrent protection.
//
//  All numbers are in include/config.h. This file is logic only.
// =============================================================================
#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "config.h"

// -----------------------------------------------------------------------------
//  Load tiers. Edit the appliance lists here; thresholds are in config.h.
//  Order matters: index 0 is the highest priority.
// -----------------------------------------------------------------------------
struct Tier {
    const char* name;
    const char* shortName;
    const char* appliances;
    uint8_t     pin;
    float       shedPct;
    float       restorePct;
    float       ratedA;
    bool        isCritical;
    // runtime state
    bool        on;
    uint32_t    pendingSince;   // 0 = no pending change
    uint32_t    lastSwitchMs;
    bool        everSwitched;
    uint32_t    shedCount;
};

Tier tiers[] = {
    { "CRITICAL",      "CRIT", "Security light, corridor light, phone/radio charging",
      PIN_RELAY_CRITICAL,  CRIT_SHED_PCT, CRIT_RESTORE_PCT, CRIT_RATED_A, true,  false, 0, 0, false, 0 },
    { "ESSENTIAL",     "ESS ", "Ceiling fan, room lighting, refrigerator",
      PIN_RELAY_ESSENTIAL, ESS_SHED_PCT,  ESS_RESTORE_PCT,  ESS_RATED_A,  false, false, 0, 0, false, 0 },
    { "NON-ESSENTIAL", "NON ", "Decorative lights, TV, toilet light, extra bulbs",
      PIN_RELAY_NONESS,    NON_SHED_PCT,  NON_RESTORE_PCT,  NON_RATED_A,  false, false, 0, 0, false, 0 },
};
const uint8_t TIER_COUNT = sizeof(tiers) / sizeof(tiers[0]);

// -----------------------------------------------------------------------------
//  State
// -----------------------------------------------------------------------------
enum InputSource { SRC_SENSORS, SRC_INJECT_SOC, SRC_INJECT_VA };

LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 16, 2);
const TimingSet* timing = DEMO_MODE ? &TIMING_DEMO : &TIMING_FIELD;

InputSource source = SRC_SENSORS;
float injSoc = 100.0f, injVolts = 0.0f, injAmps = 0.0f;

float filtVolts = 0.0f, filtAmps = 0.0f;
bool  filterPrimed = false;

// Values the controller acts on this cycle
float vTerminal = 0.0f, vRested = 0.0f, amps = 0.0f, soc = 0.0f;

// Overcurrent
bool     ocActive = false;       // loads held off, cooling down
bool     ocLatched = false;      // hard fault, needs 'reset'
uint8_t  ocTrips = 0;
uint32_t ocSince = 0;            // sustained overcurrent first seen
uint32_t ocTripMs = 0;

bool     booting = true;         // first pass: bring up loads without restore delay
uint32_t bootMs = 0;
uint32_t lastAnySwitchMs = 0;
bool     anySwitched = false;
bool     csvEnabled = true;

uint32_t lastSampleMs = 0, lastControlMs = 0, lastStatusMs = 0, lastLcdMs = 0;
char     lcdShown[2][17] = { "", "" };

// Fan stand-in (Wokwi servo). LEDC channel 2 = timer 1; buzzer on channel 4 = timer 2.
const uint8_t FAN_LEDC_CH = 2, BUZ_LEDC_CH = 4;
int  fanAngle = 0, fanDir = 1;
uint32_t lastFanMs = 0;

// -----------------------------------------------------------------------------
//  Battery maths
// -----------------------------------------------------------------------------
float voltsToSoc(float v) {
    const SocPoint* c = BATT.curve;
    uint8_t n = BATT.curveLen;
    if (v <= c[0].volts)     return c[0].pct;
    if (v >= c[n - 1].volts) return c[n - 1].pct;
    for (uint8_t i = 1; i < n; i++) {
        if (v <= c[i].volts) {
            float t = (v - c[i - 1].volts) / (c[i].volts - c[i - 1].volts);
            return c[i - 1].pct + t * (c[i].pct - c[i - 1].pct);
        }
    }
    return c[n - 1].pct;
}

float socToVolts(float pct) {
    const SocPoint* c = BATT.curve;
    uint8_t n = BATT.curveLen;
    if (pct <= c[0].pct)     return c[0].volts;
    if (pct >= c[n - 1].pct) return c[n - 1].volts;
    for (uint8_t i = 1; i < n; i++) {
        if (pct <= c[i].pct) {
            float t = (pct - c[i - 1].pct) / (c[i].pct - c[i - 1].pct);
            return c[i - 1].volts + t * (c[i].volts - c[i - 1].volts);
        }
    }
    return c[n - 1].volts;
}

// -----------------------------------------------------------------------------
//  Sensing
// -----------------------------------------------------------------------------
float readBusVolts() {
    return analogReadMilliVolts(PIN_BATT_SENSE) / 1000.0f * DIVIDER_RATIO;
}

float readLoadAmps() {
    float vAdc = analogReadMilliVolts(PIN_CURR_SENSE) / 1000.0f;
#if CURRENT_SENSE_MODE == CURRENT_SENSE_ACS712
    float vSensor = vAdc / ACS712_OUT_DIVIDER;
    float a = (vSensor - ACS712_ZERO_V) / ACS712_V_PER_A;
    return a < 0.0f ? 0.0f : a;   // load current only; ignore reverse offset
#else
    return vAdc / ADC_FULL_SCALE_V * POT_MAX_AMPS;
#endif
}

void sampleSensors() {
    float v = readBusVolts();
    float a = readLoadAmps();
    if (!filterPrimed) {
        filtVolts = v;
        filtAmps = a;
        filterPrimed = true;
    } else {
        filtVolts += EMA_ALPHA * (v - filtVolts);
        filtAmps  += EMA_ALPHA * (a - filtAmps);
    }
}

void updateInputs() {
    switch (source) {
        case SRC_INJECT_SOC:
            soc = injSoc;
            amps = filtAmps;
            vRested = socToVolts(soc);
            vTerminal = vRested - amps * BATT.internalR;
            return;
        case SRC_INJECT_VA:
            vTerminal = injVolts;
            amps = injAmps;
            break;
        default:
            vTerminal = filtVolts;
            amps = filtAmps;
            break;
    }
    // Terminal voltage sags by I x R under load; add it back before the lookup
    vRested = vTerminal + amps * BATT.internalR;
    soc = voltsToSoc(vRested);
}

// -----------------------------------------------------------------------------
//  Outputs
// -----------------------------------------------------------------------------
float shedAmps() {
    float a = 0.0f;
    for (uint8_t i = 0; i < TIER_COUNT; i++)
        if (!tiers[i].on) a += tiers[i].ratedA;
    return a;
}

void writeRelay(const Tier& t) {
    bool level = RELAY_ACTIVE_LOW ? !t.on : t.on;
    digitalWrite(t.pin, level ? HIGH : LOW);
}

void setTier(Tier& t, bool on, const char* reason, uint32_t now) {
    if (t.on == on) return;
    t.on = on;
    t.pendingSince = 0;
    t.lastSwitchMs = now;
    t.everSwitched = true;
    lastAnySwitchMs = now;
    anySwitched = true;
    if (!on) t.shedCount++;
    writeRelay(t);
    Serial.printf("[%8lu ms] >>> %-13s %s  (%s)  SoC=%.1f%%  V=%.2f  I=%.2f A  [%s]\n",
                  (unsigned long)now, t.name, on ? "RESTORED" : "SHED    ", reason,
                  soc, vTerminal, amps, t.appliances);
}

bool dwellOk(const Tier& t, uint32_t now) {
    return !t.everSwitched || now - t.lastSwitchMs >= timing->minDwellMs;
}

bool staggerOk(uint32_t now) {
    return !anySwitched || now - lastAnySwitchMs >= timing->staggerMs;
}

// -----------------------------------------------------------------------------
//  Overcurrent protection
// -----------------------------------------------------------------------------
void overcurrentStep(uint32_t now) {
    bool hard = amps >= OC_HARD_A;
    bool sustained = amps >= OC_SUSTAINED_A;

    if (sustained) {
        if (ocSince == 0) ocSince = now;
    } else {
        ocSince = 0;
    }

    bool trip = !ocActive && !ocLatched &&
                (hard || (sustained && now - ocSince >= timing->ocConfirmMs));
    if (trip) {
        ocActive = true;
        ocTripMs = now;
        ocTrips++;
        Serial.printf("[%8lu ms] !!! OVERLOAD %.1f A (%s limit %.0f A), trip %u of %u\n",
                      (unsigned long)now, amps, hard ? "hard" : "sustained",
                      hard ? OC_HARD_A : OC_SUSTAINED_A, ocTrips, OC_MAX_RETRIES);
        // Protection acts at once: no confirmation, dwell or stagger.
        // Critical is never shed by an overload — that is a load problem, not a battery problem.
        for (int8_t i = TIER_COUNT - 1; i >= 0; i--)
            if (!tiers[i].isCritical) setTier(tiers[i], false, "overload", now);
        if (ocTrips >= OC_MAX_RETRIES) {
            ocLatched = true;
            Serial.println("!!! Overload keeps recurring: FAULT LATCHED. Send 'reset' to clear.");
        }
    }

    if (ocActive && !ocLatched && now - ocTripMs >= timing->ocCooldownMs && !sustained) {
        ocActive = false;
        Serial.printf("[%8lu ms] Overload cooldown over, restores permitted\n", (unsigned long)now);
    }

    if (!ocActive && !ocLatched && ocTrips > 0 && now - ocTripMs >= timing->ocRetryClearMs) {
        ocTrips = 0;
    }
}

// -----------------------------------------------------------------------------
//  Priority controller
// -----------------------------------------------------------------------------
void controlStep(uint32_t now) {
    overcurrentStep(now);
    bool loadsHeld = ocActive || ocLatched;

    // Sheds: lowest priority first
    for (int8_t i = TIER_COUNT - 1; i >= 0; i--) {
        Tier& t = tiers[i];
        if (!t.on) continue;
        if (soc <= t.shedPct) {
            if (t.pendingSince == 0) t.pendingSince = now;
            if (now - t.pendingSince >= timing->shedConfirmMs && dwellOk(t, now) && staggerOk(now)) {
                char why[40];
                snprintf(why, sizeof(why), "SoC <= %.0f%%", t.shedPct);
                setTier(t, false, why, now);
            }
        } else {
            t.pendingSince = 0;
        }
    }

    // Restores: highest priority first, and never above a tier that is still off
    bool eligibleOff = false;
    for (uint8_t i = 0; i < TIER_COUNT; i++) {
        Tier& t = tiers[i];
        if (t.on) continue;
        bool higherOn = (i == 0) || tiers[i - 1].on;
        float restoreAt = booting ? t.shedPct : t.restorePct;
        uint32_t confirm = booting ? 0 : timing->restoreConfirmMs;
        bool wants = soc >= restoreAt && soc > t.shedPct && higherOn &&
                     !(loadsHeld && !t.isCritical);
        if (wants) {
            eligibleOff = true;
            if (t.pendingSince == 0) t.pendingSince = now;
            if (now - t.pendingSince >= confirm && dwellOk(t, now) && staggerOk(now)) {
                char why[40];
                if (booting) snprintf(why, sizeof(why), "start-up");
                else         snprintf(why, sizeof(why), "SoC >= %.0f%%", t.restorePct);
                setTier(t, true, why, now);
            }
        } else {
            t.pendingSince = 0;
        }
    }

    if (booting && (!eligibleOff || now - bootMs > 10000)) booting = false;
}

// -----------------------------------------------------------------------------
//  Status text
// -----------------------------------------------------------------------------
const char* statusText() {
    if (ocLatched)      return "FAULT: RESET";
    if (ocActive)       return "OVERLOAD TRIP";
    bool c = tiers[0].on, e = tiers[1].on, n = tiers[2].on;
    if (c && e && n)    return "ALL LOADS ON";
    if (c && e)         return "SHED NON-ESS";
    if (c)              return "CRITICAL ONLY";
    return "LVD: ALL OFF";
}

void lcdLine(uint8_t row, const char* text) {
    char buf[17];
    snprintf(buf, sizeof(buf), "%-16s", text);
    if (strcmp(buf, lcdShown[row]) == 0) return;   // no rewrite, no flicker
    lcd.setCursor(0, row);
    lcd.print(buf);
    strcpy(lcdShown[row], buf);
}

void updateLcd(uint32_t now) {
    char row[24];
    snprintf(row, sizeof(row), "%4.1fV%5.1fA %3.0f%%", vTerminal, amps, soc);
    lcdLine(0, row);

    uint8_t pages = (source == SRC_SENSORS) ? 2 : 3;
    uint8_t page = (now / LCD_PAGE_MS) % pages;
    if (page == 0) {
        // Critical is only ever off when everything is, which keeps this within 16 chars
        if (!tiers[0].on) snprintf(row, sizeof(row), "ALL TIERS OFF");
        else snprintf(row, sizeof(row), "C:ON E:%s N:%s",
                      tiers[1].on ? "ON" : "OFF", tiers[2].on ? "ON" : "OFF");
    } else if (page == 1) {
        snprintf(row, sizeof(row), "%s", statusText());
    } else {
        snprintf(row, sizeof(row), "%s", source == SRC_INJECT_SOC ? "SRC: INJECT SoC" : "SRC: INJECT V/A");
    }
    lcdLine(1, row);
}

void printStatusLine(uint32_t now) {
    Serial.printf("[%8lu ms] V=%6.2f (rest %6.2f)  I=%5.2f A  SoC=%5.1f%%  |  CRIT:%-3s ESS:%-3s NON:%-3s |  shed %.1f A  |  %s%s\n",
                  (unsigned long)now, vTerminal, vRested, amps, soc,
                  tiers[0].on ? "ON" : "OFF", tiers[1].on ? "ON" : "OFF", tiers[2].on ? "ON" : "OFF",
                  shedAmps(), statusText(), source == SRC_SENSORS ? "" : "  (injected)");
    if (csvEnabled) {
        Serial.printf("#CSV,%lu,%.2f,%.2f,%.1f,%.2f,%d,%d,%d,%.1f,%s\n",
                      (unsigned long)now, vTerminal, vRested, soc, amps,
                      tiers[0].on, tiers[1].on, tiers[2].on, shedAmps(), statusText());
    }
}

void printStatusDump() {
    Serial.println();
    Serial.println("================ STATUS ================");
    Serial.printf("Battery profile : %s\n", BATT.name);
    Serial.printf("Usable window   : %.2f V (0%%) .. %.2f V (100%%)\n",
                  BATT.curve[0].volts, BATT.curve[BATT.curveLen - 1].volts);
    Serial.printf("Capacity        : %.0f Ah, R_int %.3f ohm\n", BATT.capacityAh, BATT.internalR);
    Serial.printf("Divider         : %.0fk / %.0fk, ratio %.2f, full scale %.1f V\n",
                  DIVIDER_R_TOP / 1000.0f, DIVIDER_R_BOTTOM / 1000.0f, DIVIDER_RATIO,
                  ADC_FULL_SCALE_V * DIVIDER_RATIO);
    Serial.printf("Timing          : %s  (shed %lu ms, restore %lu ms, dwell %lu ms)\n", timing->name,
                  (unsigned long)timing->shedConfirmMs, (unsigned long)timing->restoreConfirmMs,
                  (unsigned long)timing->minDwellMs);
    Serial.printf("Input           : %s\n", source == SRC_SENSORS ? "sensors" :
                  source == SRC_INJECT_SOC ? "injected SoC" : "injected V/A");
    Serial.printf("Now             : V=%.2f  rest=%.2f  I=%.2f A  SoC=%.1f%%\n", vTerminal, vRested, amps, soc);
    Serial.printf("Overcurrent     : %s, trips %u/%u (sustained %.0f A, hard %.0f A)\n",
                  ocLatched ? "LATCHED" : ocActive ? "cooling down" : "normal",
                  ocTrips, OC_MAX_RETRIES, OC_SUSTAINED_A, OC_HARD_A);
    Serial.println("Tier           State  Shed<=  Restore>=  Rated   Sheds  Appliances");
    for (uint8_t i = 0; i < TIER_COUNT; i++) {
        const Tier& t = tiers[i];
        Serial.printf("%-14s %-5s  %5.0f%%  %8.0f%%  %4.1f A  %5lu  %s\n",
                      t.name, t.on ? "ON" : "OFF", t.shedPct, t.restorePct, t.ratedA,
                      (unsigned long)t.shedCount, t.appliances);
        Serial.printf("%-14s        = %5.1f V  = %6.1f V\n", "", socToVolts(t.shedPct), socToVolts(t.restorePct));
    }
    Serial.println("========================================");
    Serial.println();
}

void printHelp() {
    Serial.println();
    Serial.println("Commands:");
    Serial.println("  soc <pct>      inject a state of charge, e.g. 'soc 45'");
    Serial.println("  sim <V> <A>    inject terminal volts and amps, e.g. 'sim 44.2 8'");
    Serial.println("  real           return to the physical sensors");
    Serial.println("  status         full dump: profile, thresholds, per-tier state");
    Serial.println("  fast | slow    presentation / field timings");
    Serial.println("  reset          clear the overload latch and shed counters");
    Serial.println("  csv on|off     toggle #CSV telemetry");
    Serial.println("  ?              this help");
    Serial.println("Demo script: soc 100 -> soc 60 -> soc 30 -> soc 5 -> soc 100");
    Serial.println();
}

// -----------------------------------------------------------------------------
//  Serial console
// -----------------------------------------------------------------------------
void handleCommand(char* line) {
    char* cmd = strtok(line, " \t");
    if (!cmd) return;
    char* a1 = strtok(nullptr, " \t");
    char* a2 = strtok(nullptr, " \t");

    if (!strcmp(cmd, "soc") && a1) {
        injSoc = constrain(atof(a1), 0.0f, 100.0f);
        source = SRC_INJECT_SOC;
        Serial.printf("> Injected SoC %.1f%%\n", injSoc);
    } else if (!strcmp(cmd, "sim") && a1 && a2) {
        injVolts = atof(a1);
        injAmps = atof(a2);
        source = SRC_INJECT_VA;
        Serial.printf("> Injected %.2f V, %.2f A\n", injVolts, injAmps);
    } else if (!strcmp(cmd, "real")) {
        source = SRC_SENSORS;
        Serial.println("> Using physical sensors");
    } else if (!strcmp(cmd, "status")) {
        printStatusDump();
    } else if (!strcmp(cmd, "fast")) {
        timing = &TIMING_DEMO;
        Serial.printf("> Timing: %s\n", timing->name);
    } else if (!strcmp(cmd, "slow")) {
        timing = &TIMING_FIELD;
        Serial.printf("> Timing: %s\n", timing->name);
    } else if (!strcmp(cmd, "reset")) {
        ocLatched = ocActive = false;
        ocTrips = 0;
        ocSince = 0;
        for (uint8_t i = 0; i < TIER_COUNT; i++) tiers[i].shedCount = 0;
        Serial.println("> Overload latch and shed counters cleared");
    } else if (!strcmp(cmd, "csv") && a1) {
        csvEnabled = !strcmp(a1, "on");
        Serial.printf("> CSV %s\n", csvEnabled ? "on" : "off");
    } else if (!strcmp(cmd, "?") || !strcmp(cmd, "help")) {
        printHelp();
    } else {
        Serial.println("> Unknown command, send '?' for help");
    }
}

void pollSerial() {
    static char buf[48];
    static uint8_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\r' || c == '\n') {
            if (len) {
                buf[len] = '\0';
                handleCommand(buf);
                len = 0;
            }
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = tolower(c);
        }
    }
}

// -----------------------------------------------------------------------------
//  Alarm and fan
// -----------------------------------------------------------------------------
void updateBuzzer(uint32_t now) {
    bool sound = false;
    if (ocActive || ocLatched)        sound = (now / ALARM_OVERLOAD_MS) % 2 == 0;
    else if (!tiers[0].on && !booting) sound = (now / ALARM_LVD_MS) % 2 == 0;
    else if (soc < LOW_SOC_WARN_PCT)  sound = (now % ALARM_LOWSOC_PERIOD) < ALARM_LOWSOC_CHIRP;
    ledcWrite(BUZ_LEDC_CH, sound ? 128 : 0);
}

void updateFan(uint32_t now) {
    // Wokwi has no DC motor, so a servo sweeping continuously stands in for the
    // ceiling fan. Its power comes through the Essential relay's COM/NO contacts;
    // the sweep also stops in firmware so the simulation shows it either way.
    if (!tiers[1].on) {
        ledcWrite(FAN_LEDC_CH, 0);
        return;
    }
    if (now - lastFanMs < 15) return;
    lastFanMs = now;
    fanAngle += fanDir * 6;
    if (fanAngle >= 180) { fanAngle = 180; fanDir = -1; }
    if (fanAngle <= 0)   { fanAngle = 0;   fanDir = 1; }
    uint32_t us = 500 + (uint32_t)fanAngle * 2000 / 180;
    ledcWrite(FAN_LEDC_CH, us * 65535UL / 20000UL);
}

// -----------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);

    for (uint8_t i = 0; i < TIER_COUNT; i++) {
        pinMode(tiers[i].pin, OUTPUT);
        writeRelay(tiers[i]);   // everything off until the first reading
    }

    ledcSetup(BUZ_LEDC_CH, BUZZER_FREQ_HZ, 8);
    ledcAttachPin(PIN_BUZZER, BUZ_LEDC_CH);
    ledcWrite(BUZ_LEDC_CH, 0);
    ledcSetup(FAN_LEDC_CH, 50, 16);
    ledcAttachPin(PIN_FAN_PWM, FAN_LEDC_CH);
    ledcWrite(FAN_LEDC_CH, 0);

    analogReadResolution(12);
    analogSetPinAttenuation(PIN_BATT_SENSE, ADC_11db);
    analogSetPinAttenuation(PIN_CURR_SENSE, ADC_11db);

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print("Solar Load Prio");
    lcd.setCursor(0, 1);
    lcd.print(ACTIVE_BATTERY_PROFILE == PROFILE_LEADACID_12V ? "12V lead-acid" : "48V bank");

    Serial.println();
    Serial.println("Smart Solar Load Priority Management System");
    printStatusDump();
    printHelp();

    // Prime the filter before the first decision
    for (uint8_t i = 0; i < 25; i++) {
        sampleSensors();
        delay(SAMPLE_PERIOD_MS);
    }
    delay(1000);
    lcd.clear();
    bootMs = millis();
}

void loop() {
    uint32_t now = millis();
    pollSerial();

    if (now - lastSampleMs >= SAMPLE_PERIOD_MS) {
        lastSampleMs = now;
        sampleSensors();
    }
    if (now - lastControlMs >= CONTROL_PERIOD_MS) {
        lastControlMs = now;
        updateInputs();
        controlStep(now);
    }
    if (now - lastLcdMs >= LCD_REFRESH_MS) {
        lastLcdMs = now;
        updateLcd(now);
    }
    if (now - lastStatusMs >= STATUS_PERIOD_MS) {
        lastStatusMs = now;
        printStatusLine(now);
    }
    updateBuzzer(now);
    updateFan(now);
}
