# Smart Solar Load Priority System — Plain-English Presentation Guide

Sep 29, 2026 · @mustapha

## The problem in one minute

A solar battery charges by day and is drained at night. If every appliance stays on, the battery runs flat and the whole house goes dark at once, security light included.

This project is a small controller that works like a sensible person at the switchboard. As the battery empties, it switches off the least important things first, so the important ones last much longer.

The headline result, estimated from the load schedule: on a 200 Ah, 48 V battery, the last stretch of charge keeps the security light and phone charging running for an estimated **18 hours**. With everything left on, the same charge lasts about **3.5 hours**.

## The big idea: three groups of appliances

Every appliance in the house is placed in one of three groups, based on what happens if it is switched off for an hour.

| Group | The question | House examples | Current drawn |
| --- | --- | --- | --- |
| **Critical** | Does someone get hurt, or does something spoil? | Security light, corridor light, phone and radio charging | 2.5 A |
| **Essential** | Is the house much harder to live in, but nobody is harmed? | Ceiling fan, room lighting, refrigerator | 5.8 A |
| **Non-essential** | Is it comfort, entertainment or decoration? | Decorative lights, TV, toilet light, extra bulbs in a lit room | 4.5 A |

The groups are based on consequences, not on how much power something uses. In a hospital, a fridge holding vaccines becomes Critical, even though it is the same kind of fridge as the one at home.

Each group is wired through its own relay: an electrically controlled switch the controller can flip.

## How it knows how full the battery is

The controller reads the battery's voltage and turns it into a percentage, like the fuel gauge in a car.

A battery is never used all the way down to zero volts. Only part of its range is safe to use, the "usable portion". For this project that range runs from **30.0 V (empty, 0 %)** to **51.5 V (full, 100 %)**, a span of 21.5 V. So a reading of 40.75 V sits halfway, and the screen shows 50 %.

Two details make the gauge more honest:

- **It allows for voltage sag.** Switching on a heavy load makes the voltage dip for a moment, like a tap losing pressure when a second tap opens. The controller adds that dip back, so a fridge starting up doesn't look like the battery emptying.
- **It smooths the reading.** It takes a reading 50 times a second and averages them, so one noisy reading can't trigger a switch.

The settings for other battery types, lithium and a small 12 V lead-acid battery for the bench model, are already written in. Changing battery is one line in the settings file.

## What happens as the battery drains

As the charge falls, each group switches off at its own level, least important first. When the sun recharges the battery, they come back in reverse order, most important first.

&#91;embedded content: battery level ladder · 4 stages\]

The dashed lines show where each group comes back on. That point is always higher than where it switched off, and the next section explains why.

## Why it doesn't flicker on and off

The first version switched loads off and back on at a single voltage, and that fails in practice. Switching a load off lets the voltage recover a little, so the controller switched it back on. The load pulled the voltage down again, and the cycle repeated thousands of times a second. A relay treated like that burns out within minutes.

The fix works like a home thermostat, which doesn't switch the heater on and off at exactly the same temperature:

- **A gap between "off" and "back on".** Non-essential loads switch off at 66 % but only come back at 73 %. The battery has to genuinely recover before anything returns.
- **Waiting before acting.** The low reading must last a moment before the controller acts, so a brief dip is ignored. Switching back on waits longer, because switching back too early is what causes the cycling.
- **A rest period per switch.** Each relay has a minimum time between changes, whatever the readings say.
- **One switch at a time.** Two groups never switch at the same instant, which avoids a big power surge.

| Timing | Presentation mode | Real-house mode |
| --- | --- | --- |
| Wait before switching off | 0.6 s | 3 s |
| Wait before switching back on | 1.5 s | 30 s |
| Minimum rest per switch | 1.2 s | 20 s |

The presentation mode exists because real-house timings look broken in a live demo: nothing happens for half a minute. Typing `slow` switches to real-house timings, which is worth showing and explaining.

## Safety: overloads and the alarm

If the house draws too much current at once, the controller acts like a smart circuit breaker.

- **Above 20 A for a moment:** it switches off the Essential and Non-essential groups. The short wait means a motor's normal start-up surge doesn't set it off.
- **Above 26 A:** it switches them off instantly, with no wait.
- **The Critical group stays on.** An overload means too many appliances, not a flat battery, so there's no reason to cut the security light.
- **Cool-down, then retry:** the groups stay off for a while and then come back. If the overload happens three times in a row, the controller treats it as a real fault. It stops retrying until someone types `reset`.

The buzzer tells you what's wrong without looking at the screen:

| Sound | Meaning |
| --- | --- |
| Fast beeping | Overload: too much current |
| Slow, steady toll | Battery nearly empty: even Critical is off |
| Short chirp every few seconds | Battery below 20 %: a warning |
| Silent | All normal |

## What the audience will see in the demo

The demo runs in the Wokwi circuit simulator, and the same program runs on real hardware.

- **The screen** shows voltage, current and battery percentage on the top line, for example `44.2V  6.4A  78%`. The bottom line alternates between which groups are on (`C:ON E:ON N:OFF`) and a plain status such as `SHED NON-ESS`.
- **Three lights:** green for Critical, yellow for Essential, red for Non-essential. A light that is on means that group has power.
- **The fan** is a real load on the Essential group's relay. When Essential switches off, the fan stops. The simulator has no fan part, so a small motor stands in for it on screen; the bench model uses a real 12 V fan.
- **Two sliders** stand in for the battery and the household load, so the audience can see the input change.
- **The buzzer** sounds as described above.

The sliders are fiddly for hitting an exact number, so the demo uses typed commands. Each command below sets the battery level directly, and the controller reacts as if the battery really were at that level.

1. Type `soc 100`: all three lights on, fan spinning. *"The battery is full, so everything runs."*
2. Type `soc 60`: the red light goes out. *"Below 66 %, decoration, TV and spare bulbs go off. Nobody notices yet."*
3. Type `soc 30`: the yellow light goes out and the fan stops. *"Below 33 %, the fan, room lights and fridge go off to protect the essentials."*
4. Type `soc 5`: the green light goes out and the buzzer tolls. *"Below 10 %, everything is off to protect the battery from damage."*
5. Type `soc 100`: the groups return one at a time, most important first. *"When the sun returns, power comes back in order of importance."*

Optional extras: push the load slider past 26 A to show an overload trip, then type `status` to show the full settings table.

## The parts and what they cost

The control electronics cost about **₦24,000**, and about **₦47,000** with a bench model to demonstrate on. Every part is a common item sold in local electronics markets such as Alaba and Computer Village, with no imports needed.

| Part | What it does, in plain words | Approx. ₦ |
| --- | --- | --- |
| ESP32 board | The brain: reads the sensors and decides what to switch | 7,000 |
| 16×2 LCD screen | Shows voltage, current, percentage and status | 3,500 |
| 4-channel relay board | The switches that connect or cut each group | 3,500 |
| ACS712 current sensor | Measures how much current the house is drawing | 2,500 |
| Voltage converter (48 V to 5 V) | Powers the electronics from the battery | 1,800 |
| Buzzer, LEDs, resistors | Alarm and indicator lights | ≈ 630 |
| Board, box, wires, terminals | Holds it together | ≈ 5,000 |

More expensive options, such as commercial battery-management systems (₦150,000+), were avoided on purpose. They would do the job for us, leaving nothing to demonstrate. Prices are indicative and should be checked before quoting them in the report.

## How the supervisor's feedback shaped the design

Every major change traces back to something the supervisor asked for in the review.

| The supervisor said | Before | After |
| --- | --- | --- |
| "Reference your voltage to a real battery, and take the usable portion" | Fixed cut-offs at 11, 12 and 13 V, tied to no real battery | A named battery with a 30.0 V to 51.5 V usable range; every decision is made in percent |
| "Critical, essential, and non-essential" | Groups called High, Medium and Low, with no appliances attached | Three groups, each listing the real appliances on it |
| "When the battery drops to the percentage that you set, you will see the non-essential load go off" | The screen showed only raw volts | The screen shows the percentage, and every switch is logged with its cause |
| "Whatever component you are using, you should be able to get it physically, at a cheap price" | Parts never costed | About ₦24,000 of locally available parts |
| "Do you have to use LEDs only? You may add a motor" | Three indicator lights only | A fan switched through the Essential relay |

Fixing these also uncovered problems the supervisor didn't mention. The worst was the on-off cycling explained above, and a wiring mistake that left the Critical light permanently on. That hid the one event the demo most needs to show.

## Honest limits and what could come next

The system works as designed, but a few things are still estimates rather than measurements.

- **The battery gauge is based on voltage alone.** Voltage is a rough guide. Counting the current flowing in and out over time, as phone batteries do, would be more accurate.
- **The battery curves are from published figures.** Draining the real battery and logging its voltage every minute would replace them with measured data.
- **It only watches the battery.** It can't tell 40 % at 7 a.m. with a sunny day ahead from 40 % at 6 p.m. with night coming. Measuring the solar panel too would fix that.
- **Some loads depend on time, not charge.** The toilet light should come on when someone uses it; a motion sensor (about ₦1,500) would do that. The security light matters at night, not at 2 p.m.
- **The ESP32 has Wi-Fi that isn't used yet.** A phone dashboard showing battery level and history would cost nothing extra in parts.
- **No manual override yet.** The user can't force a group on or off. A real installation would need one.

## Likely questions from the panel

**Why 30.0 V as empty? Isn't that very low for a 48 V battery?** It is the range the supervisor specified, so it's the default. It is low: a real lithium battery's own protection cuts out at about 40 V, and 30 V would damage a lead-acid bank. Settings for both real battery types are already built in, and switching is one line. The switching logic works in percent, so nothing else changes.

**Why not switch back on at the same point you switch off?** The load would cycle on and off thousands of times a second and destroy the relay. The gap between off and on prevents that.

**What if the fridge's motor starts and the voltage dips?** The controller waits for a low reading to last before acting, and it allows for the dip a load causes, so a start-up surge is ignored.

**Why doesn't an overload switch off the Critical group?** An overload means too many appliances are running, not that the battery is empty. Cutting the non-critical groups removes the excess while the security light stays on.

**Can this be used somewhere other than a house?** Yes. The supervisor suggested a hospital. There, vaccine fridges, oxygen concentrators and theatre lights become Critical; ward fans and lab machines Essential; waiting-room TVs Non-essential. Only the appliance list changes.

**Is it affordable?** The controller costs about ₦24,000 in local parts, compared with ₦150,000 or more for a commercial battery-management system.
