// =============================================================================
//  config.h — every tunable number in the controller lives here.
//  src/main.cpp contains logic only. Re-targeting the system to a different
//  battery bank, sensor or appliance mix is an edit to this file alone.
// =============================================================================
#pragma once
#include <stdint.h>

// -----------------------------------------------------------------------------
//  Pins (ESP32 DevKit-C)
// -----------------------------------------------------------------------------
#define PIN_BATT_SENSE        34   // battery bus, via resistor divider
#define PIN_CURR_SENSE        32   // total load current (pot in Wokwi, ACS712 on bench)
#define PIN_RELAY_CRITICAL     5   // relay + green LED
#define PIN_RELAY_ESSENTIAL   18   // relay + yellow LED (relay contacts also switch the fan)
#define PIN_RELAY_NONESS      19   // relay + red LED
#define PIN_BUZZER             4
#define PIN_FAN_PWM           23   // Wokwi only: servo standing in for the ceiling fan
#define PIN_I2C_SDA           21
#define PIN_I2C_SCL           22
#define LCD_I2C_ADDR        0x27

// Most cheap relay boards are active-LOW. Wokwi's relay module is active-HIGH.
// Note: the tier LEDs share the relay pin, so they invert with this setting.
#define RELAY_ACTIVE_LOW       0

// -----------------------------------------------------------------------------
//  Battery profiles
//  State of charge is interpolated from a voltage->SoC curve, so a 2-point
//  (linear) profile and a multi-point measured profile use the same code.
//  Curve points must be in ascending voltage order, rested (no-load) volts.
// -----------------------------------------------------------------------------
struct SocPoint { float volts; float pct; };

struct BatteryProfile {
    const char*     name;
    float           nominalV;
    float           capacityAh;
    const SocPoint* curve;
    uint8_t         curveLen;
    float           internalR;   // ohms, for load compensation
    float           chargeV;     // absorb voltage, sets divider headroom
};

// Supervisor's specification: usable window 30.0 V (0 %) to 51.5 V (100 %).
static const SocPoint CURVE_PROJECT_48V[] = {
    { 30.0f,   0.0f },
    { 51.5f, 100.0f },
};

// 16S LiFePO4, typical published rested curve (3.40 V/cell full, 2.50 V/cell
// empty). Flat between ~20 % and ~80 %, which is why a straight line is wrong
// for lithium. Replace with the bench-measured curve once it exists.
static const SocPoint CURVE_LFP_48V[] = {
    { 40.00f,   0.0f },
    { 48.00f,  10.0f },
    { 51.20f,  20.0f },
    { 52.00f,  40.0f },
    { 52.16f,  50.0f },
    { 52.32f,  60.0f },
    { 52.80f,  70.0f },
    { 53.12f,  80.0f },
    { 54.40f, 100.0f },
};

// 12 V lead-acid (bench rig). 0 % is defined at 50 % depth of discharge,
// because taking lead-acid below that shortens its life sharply.
static const SocPoint CURVE_LEADACID_12V[] = {
    { 12.20f,   0.0f },
    { 12.32f,  20.0f },
    { 12.42f,  40.0f },
    { 12.50f,  60.0f },
    { 12.62f,  80.0f },
    { 12.73f, 100.0f },
};

#define CURVE_LEN(c) (uint8_t)(sizeof(c) / sizeof((c)[0]))

static const BatteryProfile PROFILE_PROJECT_48V_DEF  = { "Project 48V (30.0-51.5V)", 48.0f, 200.0f, CURVE_PROJECT_48V,  CURVE_LEN(CURVE_PROJECT_48V),  0.030f, 56.0f };
static const BatteryProfile PROFILE_LFP_48V_DEF      = { "LiFePO4 16S 48V",          51.2f, 100.0f, CURVE_LFP_48V,      CURVE_LEN(CURVE_LFP_48V),      0.020f, 56.8f };
static const BatteryProfile PROFILE_LEADACID_12V_DEF = { "Lead-acid 12V (50% DoD)",  12.0f,   7.0f, CURVE_LEADACID_12V, CURVE_LEN(CURVE_LEADACID_12V), 0.025f, 14.4f };

#define PROFILE_PROJECT_48V   1
#define PROFILE_LFP_48V       2
#define PROFILE_LEADACID_12V  3

// ---- Select the battery here -------------------------------------------------
#define ACTIVE_BATTERY_PROFILE  PROFILE_PROJECT_48V

#if   ACTIVE_BATTERY_PROFILE == PROFILE_PROJECT_48V
  #define BATT PROFILE_PROJECT_48V_DEF
#elif ACTIVE_BATTERY_PROFILE == PROFILE_LFP_48V
  #define BATT PROFILE_LFP_48V_DEF
#elif ACTIVE_BATTERY_PROFILE == PROFILE_LEADACID_12V
  #define BATT PROFILE_LEADACID_12V_DEF
#else
  #error "Unknown ACTIVE_BATTERY_PROFILE"
#endif

// -----------------------------------------------------------------------------
//  Battery voltage divider — real resistor values, not a bare multiplier.
//  48 V: 180k / 10k -> ratio 19.0 -> full scale 62.7 V (clears 58 V absorb).
//  12 V: 47k  / 10k -> ratio 5.7  -> full scale 18.8 V.
// -----------------------------------------------------------------------------
#if ACTIVE_BATTERY_PROFILE == PROFILE_LEADACID_12V
  #define DIVIDER_R_TOP     47000.0f
#else
  #define DIVIDER_R_TOP    180000.0f
#endif
#define DIVIDER_R_BOTTOM    10000.0f
#define DIVIDER_RATIO       ((DIVIDER_R_TOP + DIVIDER_R_BOTTOM) / DIVIDER_R_BOTTOM)
#define ADC_FULL_SCALE_V    3.3f

// -----------------------------------------------------------------------------
//  Current sensing
// -----------------------------------------------------------------------------
#define CURRENT_SENSE_POT     1   // Wokwi: 0-3.3 V sweeps 0-POT_MAX_AMPS
#define CURRENT_SENSE_ACS712  2   // bench: ACS712-30A behind a 0.6x divider

#define CURRENT_SENSE_MODE    CURRENT_SENSE_POT

#define POT_MAX_AMPS          30.0f
#define ACS712_ZERO_V         2.50f    // output at 0 A (calibrate on the bench)
#define ACS712_V_PER_A        0.066f   // 30 A variant
#define ACS712_OUT_DIVIDER    0.6f     // 10k / 15k divider: 5 V output -> 3.0 V ADC

// -----------------------------------------------------------------------------
//  ADC filtering
// -----------------------------------------------------------------------------
#define SAMPLE_PERIOD_MS      20
#define EMA_ALPHA             0.15f    // ~250 ms settling at 20 ms
#define CONTROL_PERIOD_MS     100

// -----------------------------------------------------------------------------
//  Tier thresholds (percent state of charge) and rated currents.
//  Keep at least 8 points between a shed point and its restore point, or the
//  hysteresis band is narrower than the sag the load itself causes.
// -----------------------------------------------------------------------------
#define CRIT_SHED_PCT         10.0f
#define CRIT_RESTORE_PCT      25.0f
#define CRIT_RATED_A           2.5f

#define ESS_SHED_PCT          33.0f
#define ESS_RESTORE_PCT       42.0f
#define ESS_RATED_A            5.8f

#define NON_SHED_PCT          66.0f
#define NON_RESTORE_PCT       73.0f
#define NON_RATED_A            4.5f

#define LOW_SOC_WARN_PCT      20.0f    // chirp below this

// -----------------------------------------------------------------------------
//  Overcurrent protection
// -----------------------------------------------------------------------------
#define OC_SUSTAINED_A        20.0f    // trips after OC confirm window
#define OC_HARD_A             26.0f    // trips instantly
#define OC_MAX_RETRIES        3        // then latch until 'reset'

// -----------------------------------------------------------------------------
//  Timing sets. DEMO_MODE 1 = presentation timings, 0 = field timings.
//  Switchable at runtime with the serial commands 'fast' / 'slow'.
// -----------------------------------------------------------------------------
#define DEMO_MODE 1

struct TimingSet {
    const char* name;
    uint32_t shedConfirmMs;     // shed condition must hold this long
    uint32_t restoreConfirmMs;  // restore condition must hold this long
    uint32_t minDwellMs;        // floor between changes of any one relay
    uint32_t staggerMs;         // no two relays switch within this window
    uint32_t ocConfirmMs;       // sustained overcurrent must hold this long
    uint32_t ocCooldownMs;      // loads held off after a trip
    uint32_t ocRetryClearMs;    // trip-free time that clears the retry count
};

static const TimingSet TIMING_DEMO  = { "DEMO (fast)",   600,  1500,  1200,  300,  300,  4000,  20000 };
static const TimingSet TIMING_FIELD = { "FIELD (slow)", 3000, 30000, 20000, 2000, 1000, 30000, 300000 };

// -----------------------------------------------------------------------------
//  Alarm cadences (ms)
// -----------------------------------------------------------------------------
#define BUZZER_FREQ_HZ        2000
#define ALARM_OVERLOAD_MS      150     // fast on/off
#define ALARM_LVD_MS           800     // slow toll
#define ALARM_LOWSOC_PERIOD   2500     // occasional chirp
#define ALARM_LOWSOC_CHIRP      60

// -----------------------------------------------------------------------------
//  Display / telemetry
// -----------------------------------------------------------------------------
#define LCD_REFRESH_MS         250
#define LCD_PAGE_MS           2000
#define STATUS_PERIOD_MS      1000
