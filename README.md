# Smart Solar Load Priority Management System

ESP32 controller that sheds and restores three load tiers by battery **state of
charge**, so a solar battery bank lasts as long as possible for the loads that
matter.

| Tier | Appliances (residential default) | Rated | Shed at | Restored at |
|---|---|---|---|---|
| **CRITICAL** | Security light, corridor light, phone/radio charging | 2.5 A | ≤ 10 % (32.2 V) | ≥ 25 % |
| **ESSENTIAL** | Ceiling fan, room lighting, refrigerator | 5.8 A | ≤ 33 % (37.1 V) | ≥ 42 % |
| **NON-ESSENTIAL** | Decorative lights, TV, toilet light, extra bulbs | 4.5 A | ≤ 66 % (44.2 V) | ≥ 73 % |

Default battery: the supervisor's specification, usable window **30.0 V = 0 %
to 51.5 V = 100 %** (21.5 V). The gap between each shed and restore point is
hysteresis, which stops the relays chattering.

## Traceability to the supervisor feedback

| Feedback | Where it is addressed |
|---|---|
| Reference the battery to a real one; use the usable portion; work in % | `BatteryProfile` + SoC curve in `include/config.h`; every decision in `src/main.cpp` is on SoC % |
| Critical / Essential / Non-essential, tied to real appliances | `tiers[]` table at the top of `src/main.cpp` |
| Demonstration must be legible (100 % all on, then each tier drops) | LCD shows V, A, **SoC %** and tier states; every shed/restore prints its own line with the cause; `soc <pct>` console command |
| Components must be cheap and buyable | Commodity parts only: ESP32, 16×2 I²C LCD, 4-ch relay board, ACS712, resistor divider (see *Bill of materials*) |
| Show a real load, not only LEDs | Fan load powered through the Essential relay's COM/NO contacts |

## Behaviour

- **Load compensation**: `V_rested = V_terminal + I × R_internal` before the SoC lookup, so switching a load on does not look like lost charge.
- **Confirmation, dwell, stagger**: a condition must hold before acting; each relay has a minimum time between changes; no two relays switch in the same window.
- **Overcurrent**: 20 A sustained (after a confirmation window) or 26 A instantly sheds Essential and Non-essential. Critical is never shed by an overload. Loads stay off for a cooldown; after 3 trips the fault latches until `reset`.
- **Alarm (GPIO 4)**: fast beeps = overload, slow toll = critical tier shed (low-voltage disconnect), occasional chirp = SoC below 20 %.
- **Filtering**: 20 ms sampling, exponential moving average, calibrated `analogReadMilliVolts()`.

Two timing sets exist. `fast` (presentation) and `slow` (field) switch between them at runtime:

| | Presentation | Field |
|---|---|---|
| Shed confirm | 0.6 s | 3 s |
| Restore confirm | 1.5 s | 30 s |
| Min dwell | 1.2 s | 20 s |
| Overload cooldown | 4 s | 30 s |

## Serial console (115200 baud)

| Command | Effect |
|---|---|
| `soc 45` | Inject a state of charge |
| `sim 44.2 8` | Inject terminal volts and amps |
| `real` | Return to the physical sensors |
| `status` | Profile, window, thresholds (in % and V), per-tier state and shed counts |
| `fast` / `slow` | Timing sets |
| `reset` | Clear overload latch and shed counters |
| `csv on` / `csv off` | Toggle `#CSV` telemetry rows (for plotting SoC vs time) |
| `?` | Help |

**Demonstration script:** `soc 100` → `soc 60` (non-essential off) → `soc 30`
(essential off, fan stops) → `soc 5` (critical off, alarm tolls) → `soc 100`
(tiers return one by one, critical first).

CSV columns: `ms, V_terminal, V_rested, SoC, amps, crit, ess, non, shed_amps, status`.

## Wiring

| GPIO | Function |
|---|---|
| 34 | Battery bus via 180 kΩ / 10 kΩ divider (ratio 19, full scale 62.7 V) |
| 32 | Load current (slide pot in Wokwi; ACS712-30A behind a 10k/15k divider on the bench) |
| 5 | CRITICAL relay + green LED (220 Ω) |
| 18 | ESSENTIAL relay + yellow LED (220 Ω) |
| 19 | NON-ESSENTIAL relay + red LED (220 Ω) |
| 4 | Buzzer |
| 21 / 22 | LCD SDA / SCL (I²C 0x27) |
| 23 | Wokwi only: servo standing in for the fan |

Wokwi has no DC motor part, so the simulation uses a servo, powered from 5 V
through the Essential relay's COM/NO contacts, as the fan. On the bench, a
12 V DC fan goes on those same contacts.

## Configuration

Every number is in `include/config.h`:

- `ACTIVE_BATTERY_PROFILE`: `PROFILE_PROJECT_48V` (default), `PROFILE_LFP_48V` (typical LiFePO₄ curve, which is flat between 20 % and 80 %), or `PROFILE_LEADACID_12V` (bench rig; 0 % = 50 % depth of discharge; selects a 47 kΩ / 10 kΩ divider).
- `CURRENT_SENSE_MODE`: `CURRENT_SENSE_POT` (Wokwi) or `CURRENT_SENSE_ACS712` (bench).
- `RELAY_ACTIVE_LOW`: set to 1 for most real relay boards.
- `DEMO_MODE`: timing set used at boot.
- Tier thresholds: keep at least 8 points between a tier's shed and restore values.

The LiFePO₄ and lead-acid curves are typical published values, not
measurements. Discharging the actual bank and logging voltage replaces them
with a measured curve (`SocPoint[]` takes it directly).

## Build and simulate

```bash
pio run
```

Then start the simulation in VS Code with the Wokwi extension (`wokwi.toml` points at the built firmware).
