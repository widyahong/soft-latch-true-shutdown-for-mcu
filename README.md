# Soft-Latch True Shutdown for LDO-Powered MCUs (ESP32 example)

A P-MOSFET high-side switch cuts power to a regulator that has **no enable pin**.
A button turns it on, the MCU holds it on through an N-MOSFET (Q2, 2N7000), and the MCU
releases it to shut down. Off-state current is leakage only. The same button is also a normal digital input.

---

## 1. Universal concept (any MCU)

You need: 1 GPIO output (`LATCH`), 1 GPIO input (`SENSE`), a regulator (LDO or buck).

| State | Q1 (P-FET) gate | Why |
|---|---|---|
| Off | pulled to VIN by R1 | Vgs = 0, Q1 off, regulator unpowered |
| Button pressed (MCU off) | pulled to ~0.6 V through D1 + SW1 | Q1 on, MCU boots |
| MCU running | held ~0 V by Q2 (`LATCH` = 1) | Latched, button may be released |
| Shutdown | `LATCH` = 0, Q2 off, R1 pulls gate up | Q1 off, power gone |

**Button sensing with a single-pole button:** D1 and D2 form a small diode network on node B.
D1 lets the button pull the gate low. D2 lets the button pull `SENSE` low and *blocks* the ~VIN that
appears on node B while everything is off, so it never reaches the MCU pin or back-feeds the 3V3 rail.

**Is it efficient? Yes, the diodes are not in the power path.** Load current flows only through Q1
(Rds(on) of tens of mΩ, e.g. 0.1 A × 30 mΩ ≈ 3 mV drop). D1 and D2 carry µA-level signal current,
and only while the button is pressed. The real standby losses are the regulator's own quiescent current
and the pull resistors, see section 4.

---

## 2. Circuit

```
 VIN (+6 V .. +12.6 V) ───┬───────────────────────────┐
 (2S/3S Li-ion)           │                           │ S
                         R1                       ┌───┴───┐
                        220k                      │  Q1   │  IRF4905
                          │                       │       │  (P-MOSFET)
                          │      node G           │       │
                          ├───────────────────────┤ G     │
                          │                       └───┬───┘
                          │                           │ D
                          │                           └──►|──┬──────► VSW  (to U1 IN)
                          │                              D3   │
                          │                           1N5819 │
                          │   USB-UART adapter 5V pin ──►|───┘
                          │                              D4 1N5819
                  ┌───────┴───────────┐
                  │                   │ D
                  │               ┌───┴───┐
                 D1 ▼ 1N4148      │  Q2   │  2N7000 (N-MOSFET)
           (anode at node G)      │       │
                  │   GPIO26 ─R4──┤ G     │
                  │  (LATCH)  1k  │       │
                  │         │     └───┬───┘
                  │        R5         │ S
                  │        1M        GND
                  │         │
                  │        GND                   3V3 ── R2 10k ──┬── GPIO33 (SENSE)
                  │                                              │
                  │                                       node S │
                  │                                             D2 ▼ BAT43
                  │                                              │
                  └──────────────── node B ──────────────────────┘
                                       │
                                      SW1  momentary, normally open
                                       │
                                      GND

 Power path after the switch (low quiescent version):

 VSW ──┬──────┐ VIN   ┌────────────────┐ VOUT ──┬──────┬──────┬──────► 3V3 pin
       │      └───────┤ U1             ├────────┘      │      │        (ESP32)
      C1              │ MCP1702-3302E  │               C3     C4
     10µF             └───────┬────────┘              47µF   100nF  R6 47k (optional, see 4)
      25V                    GND                       │      │
       │                      │                        │      │
      GND                    GND                      GND    GND
 (+C2 100nF parallel to C1)   (TO-92: check pin order in the datasheet)
```

Optional, only if VIN > 18 V: Zener `1N4744A` (15 V, 1 W) across Q1: **cathode to VIN, anode to node G**.

### Bill of materials (all through-hole)

| Ref | Value / model | Package | Notes |
|---|---|---|---|
| Q1 | IRF4905 | TO-220 | P-MOSFET, -55 V, Vgs max ±20 V, 20 mΩ at -10 V. At 2S (6–8.4 V) it is not fully "saturated" but at ≤ 0.3 A the drop is still mV. IRF9540N also works |
| Q2 | 2N7000 (or BS170) | TO-92 | N-MOSFET, only pulls a ~50 µA load, 3.3 V drive is enough |
| U1 | MCP1702-3302E/TO | TO-92 | 3.3 V, 250 mA, Iq ≈ 2 µA, VIN max 13.2 V (fits 2S, 3S is tight). See section 3 for >250 mA |
| D1 | 1N4148 | DO-35 | ~0.6 V drop is fine here |
| D2 | BAT43 | DO-35 | Schottky: low drop keeps `SENSE` under ESP32 VIL (~0.8 V), low leakage |
| D3 | 1N5819 | DO-41 | Battery side: Q1 drain → VSW. Stops USB power from reaching Q1 / VIN |
| D4 | 1N5819 | DO-41 | USB side: adapter 5V pin → VSW. Stops battery power from reaching the adapter / USB |
| R7 | 1 kΩ | axial 0.25 W | Series on adapter TX → ESP32 RX (ESD / ringing) |
| R8 | 4.7 kΩ | axial 0.25 W | Series on ESP32 TX → adapter RX, limits leakage into an unpowered adapter |
| R1 | 220 kΩ | axial 0.25 W | Gate pull-up. 34 µA at 7.4 V while latched; 1 MΩ saves more but gives the button only ~7 µA |
| R2 | 10 kΩ | axial 0.25 W | `SENSE` pull-up to 3V3 (no current unless button is pressed) |
| R4 | 1 kΩ | axial 0.25 W | Series resistor on `LATCH` |
| R5 | 1 MΩ | axial 0.25 W | Keeps Q2 off while GPIO floats at boot, only 3.3 µA when `LATCH` = 1 |
| R6 | 47 kΩ, optional | axial 0.25 W | Bleeder so 3V3 collapses in ~2 s. Costs 70 µA while running; omit if re-powering fast is never a problem |
| C1 | 10 µF / 25 V | radial electrolytic | Regulator input. C1–C4 are regulator decoupling, not button debounce (debounce is software, `DEBOUNCE_MS`) |
| C2, C4 | 100 nF / 50 V | radial ceramic | Decoupling |
| C3 | 47 µF / 10 V | radial electrolytic | Regulator output (MCP1702 needs ≥ 1 µF) |
| SW1 | 6×6 mm tactile | THT | Momentary, normally open |
| MCU | ESP32-DevKitC (headers) | THT headers | Powered via **3V3 pin**, its own USB port left unused (section 5) |
| USB-UART | CP2102 / CH340 / FT232 module with 5V pin | THT header | Gives serial **and** powers the MCU by itself |

---

## 3. Regulator choice (efficiency story)

| Option | Iq (deep sleep matters) | Max current | Efficiency at 7.4 V → 3.3 V | Use for |
|---|---|---|---|---|
| **MCP1702-3302E/TO** (default) | ~2 µA | 250 mA | 3.3/7.4 = 45% (LDO) | Sleep demos, light loads |
| LD1117V33 | **~5 mA** | 800 mA | 45%, but 5 mA wasted always | Not recommended: it alone is 500× a deep-sleep budget |
| **TRACO TSR 1-2433** (buck, SIP-3, drop-in for 78xx) | check datasheet (sub-mA) | 1 A | ~85–90% | Long active time or loads above 250 mA. Add ≥ 100 µF at VOUT |

An LDO can never beat Vout/Vin, so at 2S/3S a buck wins whenever the MCU is active for long.
The P-FET switch still gives true shutdown with either, because it cuts the regulator input.
If you later add loads above ~250 mA (sensors, motors, radios), use the buck instead of the MCP1702.

USB path note: after D4 the USB rail is ~4.7 V. That is fine for the MCP1702 (dropout 0.65 V at 250 mA)
but below the TSR 1-2433's minimum input (check its datasheet), so use the MCP1702 if USB power must work.

Li-ion note: this circuit has no under-voltage cut-off. Use a 2S/3S protection board (BMS) on the pack.

---

## 4. Standby budget (what really costs current)

| Item | While latched | While off |
|---|---|---|
| R1 220 kΩ via Q2 | 34 µA (7.4 V), 57 µA (12.6 V) | 0 |
| R5 1 MΩ (`LATCH` high) | 3.3 µA | 0 |
| MCP1702 Iq | ~2 µA | 0 |
| R6 47 kΩ (optional) | 70 µA | 0 |
| D1, D2, SW1 | 0 | 0 |
| ESP32 deep sleep (chip) | ~10 µA | 0 |

Roughly 40–50 µA of circuit overhead (without R6), versus ~5 mA with an LD1117.
Off state: only Q1/Q2/D leakage (nA range).

---

## 5. USB-UART as a second power source (no back-feed either way)

Goal: plugging in the USB-UART adapter powers the MCU **without pressing the button**,
and no energy flows from VIN into USB or from USB into VIN.

The two sources are OR-ed at the regulator input (VSW) with one Schottky each:

```
 Battery ──► Q1 ──►|── D3 ──┐
                            ├──► VSW ──► U1 ──► 3V3
 Adapter 5V pin ──►|── D4 ──┘

 Adapter TX  ────R7 1k────── ESP32 GPIO3 (RX0)
 Adapter RX  ────R8 4.7k──── ESP32 GPIO1 (TX0)
 Adapter GND ─────────────── GND          (adapter 3V3 pin: leave unconnected)
```

- **USB → VIN is blocked** by D3 (and Q1's body diode never sees USB because D3 sits between Q1 and VSW).
- **VIN → USB is blocked** by D4. The adapter's 5V pin only ever sees USB.
- **Auto power-on:** USB 5V → D4 → U1 → MCU boots with no button. Firmware then sets `LATCH` as usual.
- **No UART diode network needed anymore:** the adapter's TX only goes high while USB is attached, and at that moment the MCU is powered by it.
  The only leftover path is ESP32 TX into an *unpowered* adapter while running on battery; R8 keeps that to a few hundred µA at most (usually far less).
- **LDO efficiency:** D3/D4 drop (~0.3 V) costs nothing extra with an LDO, because the input voltage above 3.3 V is dissipated anyway.
  With a buck it would cost roughly 4% at 7.4 V.
- **Both sources present:** the higher voltage wins, so the battery (6–12.6 V) carries the load and USB only supplies when the battery path is off.
  To prefer USB, use `off` (battery path released, MCU stays alive on USB). A true USB-priority switch needs an ideal-diode controller or extra P-FET.
- **Flashing:** DTR/RTS auto-reset is not wired. For a blank chip, hold GPIO0 low while the adapter powers the board, or add the usual transistor pair.
- **DevKit's own USB port:** not recommended in this design. Its on-board regulator drives the same 3V3 net and reverse leakage back toward USB
  is not guaranteed. If you use it anyway, feed U1 through a 1N5819 into the 3V3 pin and check with a meter that VBUS reads ~0 V while on battery.

Quick checks with a multimeter: battery only, adapter unplugged → adapter 5V pin ≈ 0 V.
USB only, battery disconnected → VIN pin ≈ 0 V.

---

## 6. ESP32 firmware

Pins (classic ESP32, change at the top of `firmware.ino`): `LATCH` = GPIO26, `SENSE` = GPIO33, `LED` = GPIO2 (set -1 if absent).
The pin-hint comments in the config block explain which GPIOs are RTC-capable, strapping, input-only, and what changes on S3/C3.
Only Arduino-ESP32 core headers are used (`esp_sleep.h`, `driver/gpio.h`).

### Button gestures

| Gesture | Result |
|---|---|
| Click | Run the selected demo mode |
| Double-click (within `DOUBLE_CLICK_MS`) | Cycle demo mode (active → light → deep → hib) |
| Hold ≥ `midHoldMs` (800 ms), LED solid | Release now = print status |
| Keep holding, LED blinks fast near the end | Reaches `longPressMs` (2000 ms) = true shutdown |
| Hold at power-on / wake | Ignored until released (no accidental shutdown) |
| Idle for `idleOffSec` | True shutdown (0 = disabled) |

### Demo modes (typical chip-only figures from Espressif docs; dev boards read higher)

| `mode` | What it does | Approx. current | Wake sources | Data |
|---|---|---|---|---|
| `active` | Stays awake, use `cpu` to compare | tens of mA | n/a | kept |
| `light` | `esp_light_sleep_start()` | ~0.8 mA | timer, button | kept |
| `deep` | Deep sleep, ext0 wake | ~10 µA | timer, button | RTC data kept, reboots |
| `hib` | Deep sleep, RTC periph/memory off, ext1 wake | ~5 µA | timer, button (external R2 pull-up) | RTC data lost, settings reset |
| `off` | True shutdown through the latch (if USB is attached the MCU stays up on USB with the battery path off; button re-latches) | ~0 on battery | button only | all lost |

During deep sleep and hibernation the `LATCH` pad is frozen with `gpio_hold_en()` + `gpio_deep_sleep_hold_en()` so power stays on.
Settings live in `RTC_DATA_ATTR` variables: they survive deep sleep, not hibernation or true shutdown.

### Adjustable variables (top of `firmware.ino`, or live over Serial)

`longPressMs`, `midHoldMs`, `sleepSeconds`, `idleOffSec`, `cpuMHz`, `wakeByButton`, `wakeByTimer`, `demoMode`,
plus compile-time `POWER_ON_HOLD_MS`, `DEBOUNCE_MS`, `DOUBLE_CLICK_MS`, `PIN_LATCH`, `PIN_SENSE`, `PIN_LED`.

### Serial commands (115200 baud, any line ending)

| Command | Meaning |
|---|---|
| `help` / `status` | Command list / all settings |
| `mode active\|light\|deep\|hib` | Select what click / `run` does |
| `run` | Execute the selected mode now |
| `time <sec>` | Sleep timer period |
| `cpu <80\|160\|240>` | CPU frequency in MHz |
| `wake b\|t\|bt` | Wake by button / timer / both |
| `mid <ms>`, `hold <ms>` | Medium-hold and shutdown-hold thresholds |
| `idle <sec>` | Auto shutdown after idle (0 = off) |
| `off` | True shutdown now |

Power-on timing: the ESP32 needs ~0.3–0.5 s to reach `setup()`, so hold the button briefly.
Firmware sets `LATCH` as its first instruction and (optionally) rejects taps shorter than `POWER_ON_HOLD_MS`.
When USB powers the MCU instead, that check just releases the battery path and the board keeps running on USB; a button press re-latches the battery.
Wait ~2 s after shutdown before re-powering so the 3V3 rail falls fully.

---

## 7. Porting to other MCUs

Keep the circuit as is. Needed: a GPIO that is low/high-Z at reset (R5 holds Q2 off), an input that reads node S,
and `LATCH = HIGH` as the first firmware step. Replace the sleep calls with that chip's equivalents.
