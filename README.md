# ESP32 + Nextion + Speeduino Car Dashboard

A custom digital instrument cluster for a car running a [Speeduino](https://speeduino.com/) ECU. A Nextion 7" (800x480) touchscreen displays live engine data, driven by an ESP32 that pulls data from Speeduino over Bluetooth and pushes it to the screen over UART.

## Hardware

- **Nextion 7" HMI display** (800x480), programmed with a page containing the components listed in [Nextion component map](#nextion-component-map) below.
- **ESP32 Dev Module** — a classic ESP32 (WROOM), **not** an S3/S2/C3 and **not** a WROVER variant. Bluetooth Classic (SPP) is required, which S3/C3 don't support, and WROVER's extra PSRAM permanently claims GPIO16/17, which this project avoids using for exactly that reason.
- **Speeduino ECU**, any reasonably recent firmware (tested against reference offsets from the 202501.7 stable release; Speeduino only ever appends new fields to its serial protocol, never moves existing ones, so this should hold for most modern builds).
- **A Bluetooth-serial module** (e.g. HC-05/06) wired to Speeduino's secondary serial port, acting as the Bluetooth peer the ESP32 connects to.
- **A resistive fuel level sender** (this build: 300Ω empty / 35Ω full) plus a 150Ω fixed resistor and a 1µF capacitor for the fuel level voltage divider.
- A stable 5V power supply for the ESP32. **This matters more than it sounds** — see [Power supply](#power-supply-warning) below.

## Wiring

```
Nextion  <-> ESP32 UART2
  Nextion TX -> ESP32 GPIO4  (RX2)
  Nextion RX <- ESP32 GPIO5  (TX2)
  Common GND between ESP32 and Nextion

Speeduino <-> ESP32
  Connected via Bluetooth Classic (SPP) through a Bluetooth-serial module
  wired to Speeduino's TX/RX pins. No direct GPIO wiring needed on the
  ESP32 side - this happens over the radio.

Fuel sender <-> ESP32
  3.3V --[150 ohm resistor]--+--[fuel sender, 300-35 ohm]-- GND (chassis)
                              |
                           GPIO34 (ADC1 input)
  + 1uF capacitor from GPIO34 to GND for noise filtering
  ESP32 GND must be tied to the same chassis ground as the sender.
```

Nextion's UART is **not** on the ESP32's USB pins (GPIO1/3) — it's on UART2 (GPIO4/5) instead, so the normal USB Serial Monitor stays free for debugging without interfering with the display.

### Power supply warning

Enabling Bluetooth on the ESP32 causes sharp current spikes (300-450mA momentarily) well above its steady-state draw. A marginal power source (some USB power banks in particular) can brown out under that spike — this looked exactly like "gauges reset every second" during development and was fixed by switching to a proper regulated supply. Add bulk capacitance (470-1000µF electrolytic + 100nF ceramic) right at the ESP32's 5V/GND pins if you see anything like that.

## Repository layout

**`esp32_speeduino_nextion_dash.ino`** — the real firmware. Reads live data from Speeduino over Bluetooth, reads the fuel sender directly, and drives every gauge on the Nextion display. This is what actually goes in the car.

Supporting test/debug sketches (useful for bringing up a new board or diagnosing a wiring issue, not needed for normal operation):

| File | Purpose |
|---|---|
| `esp32_nextion_uart2_test.ino` | Isolated link test — confirms the ESP32 can talk to the Nextion display on GPIO4/5 (sends `connect`, expects a `comok` reply), independent of Bluetooth/Speeduino. |
| `esp32_bt_scan.ino` | One-off Bluetooth Classic scanner — lists nearby devices with their MAC addresses, used to find the Bluetooth-serial module's address for `SPEEDUINO_BT_MAC`. |
| `esp32_bt_connect_test.ino` | Connects to Speeduino over Bluetooth and prints decoded RPM/coolant/IAT/battery/AFR/TPS/speed/gear to the Serial Monitor once a second — confirms the Bluetooth link and byte offsets independent of the Nextion display. |
| `esp32_rpm_full_test_uart2.ino` | Full gauge sweep test with **no Bluetooth at all** — every gauge ping-pongs through its range on its own. Used to isolate display/wiring/power issues from Speeduino-specific ones (if this runs clean but the real firmware doesn't, the problem is in the Bluetooth/Speeduino path, not the display). |

Older/earlier-stage sketches (`esp32_basic_rpm_sweep.ino`, `esp32_basic_rpm_ring_sweep.ino`, `esp32_basic_rpm_progressbar_sweep.ino`, `esp32_nextion_link_debug.ino`, `esp32_nextion_gauge_test.ino`, `esp32_nextion_slider_trigger.ino`, `esp32_segbar_test.ino`, `esp32_rpm_full_test.ino`) document the project's development history (font-glyph ring gauge → abandoned for flicker; segmented bar font approach → abandoned in favor of native Progress Bar widgets) and are kept for reference but superseded by the files above.

## Nextion component map

Every component below lives on Page 0. Bars (`j`) are Nextion's native Progress Bar widget (fixed 0-100 range — real-unit scaling happens in firmware); Numbers (`n`) are integer-only; Text (`t`) components are used wherever a real decimal needs to be displayed.

| Channel | Bar | Number/Text | Label | Color zones |
|---|---|---|---|---|
| RPM | j2 | n0 | t0 | green / yellow ≥6000 / red ≥6500, + shift lights + flashing redline ≥7500 |
| Coolant temp | j0 | n2 | t4 | blue <50°C / green 50-95°C / red ≥95°C |
| AFR | j1 | t12 (decimal) | t5 | yellow <13.0 / green 13.0-16.0 / red ≥16.0 |
| TPS | j3 | n4 | t6 (untouched) | none |
| IAT | j4 | n5 | t7 | blue <20°C / green 20-60°C / red ≥60°C |
| Battery voltage | j5 | t13 (decimal) | t8 | red <13.0V / green 13.0-15.0V / red ≥15.0V |
| Speed | — | n1 | — | none, raw km/h |
| Gear | — | t2 | — | none, raw gear number |
| Ignition advance | j6 | n7 | t9 (untouched) | none, -20 to 45° |
| Fuel level | j8 | n9 | t11 | red <20% / green ≥20% |

Shift lights: `bt0`-`bt6`, Dual State Buttons, cumulative (once on, stays on) and spread across 5000-7000rpm; all seven flash together above 7500rpm.

Exact color values used throughout (decimal, as shown in the Nextion Editor): black=6404, red=64197, blue=15964, green=20113, yellow=65504, white=65535.

## Speeduino serial protocol

The firmware talks to Speeduino using its plain legacy protocol: sending the single ASCII byte `'A'` gets back a raw binary block of live data with no header, length, or CRC — byte 0 of the reply is the first data byte. Offsets used here were checked directly against Speeduino's `reference/speeduino.ini` and `logger.cpp` source (stable 202501.7 release and current dev branch agree on all of them):

| Offset | Size | Field | Notes |
|---|---|---|---|
| 4 | U16 | MAP | kPa (not currently displayed) |
| 6 | U08 | IAT | actual °C = raw − 40 |
| 7 | U08 | Coolant | actual °C = raw − 40 |
| 9 | U08 | Battery voltage | raw already = volts × 10 |
| 10 | U08 | AFR | raw already = AFR × 10 |
| 14 | U16 (LE) | RPM | straight value |
| 24 | S08 | Ignition advance | degrees BTDC directly (signed, no offset math) |
| 25 | U08 | TPS | actual % = raw × 0.5 |
| 104 | U16 (LE) | Speed (VSS) | km/h |
| 106 | U08 | Gear | raw gear number; reads 0 if no gear input is configured on Speeduino |

If you're on much older Speeduino firmware and see obviously wrong values, check `reference/speeduino.ini` in your exact firmware's source against this table.

## Setup

1. Flash `esp32_speeduino_nextion_dash.ino` to the ESP32 (Arduino IDE, board = "ESP32 Dev Module", any partition scheme with enough app space for Bluetooth — "Default" or "Huge APP" both work).
2. Wire the Nextion display to GPIO4/5 as above, and set Page0's Preinitialize Event `bauds=` in the Nextion Editor to match `nextionBaud` in the firmware (115200 by default).
3. Find your Bluetooth-serial module's MAC address with `esp32_bt_scan.ino`, then set `SPEEDUINO_BT_MAC` in the main firmware to that address.
4. Wire the fuel sender as shown above and confirm `FUEL_DIVIDER_FIXED_OHMS`/`FUEL_EMPTY_OHMS`/`FUEL_FULL_OHMS` match your actual resistor and sender readings.
5. Power the ESP32 from a supply that can handle Bluetooth's current spikes (see [Power supply warning](#power-supply-warning)) — this was the single biggest source of "mystery" bugs during development.

## Known limitations / notes for future work

- Fuel level assumes a **linear** relationship between resistance and tank level, based on two measured points (empty/full). Real senders aren't always linear across the whole range — if it reads noticeably off around half-tank, measure the resistance at a known half-full level and switch to a multi-point lookup table.
- Gear display shows Speeduino's raw gear number with no letter mapping (e.g. no "N" for neutral) — add one in `applyGear()` if wanted.
- Bluetooth reconnect (`connectSpeeduinoIfNeeded()`) blocks briefly (up to a few seconds) while attempting to reconnect, which can cause a short stutter in gauge updates if the link drops mid-drive. Acceptable for now; could be moved to a separate FreeRTOS task if it becomes noticeable.
- MAP (boost/vacuum) is already parsed-and-available at offset 4 but not wired to any gauge (not used — this build runs Alpha-N).
