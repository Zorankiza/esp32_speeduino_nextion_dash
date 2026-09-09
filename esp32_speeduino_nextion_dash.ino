/*
  REAL Speeduino -> Nextion dashboard firmware.
  ------------------------------------------------------------------------
  This replaces the sweep/test loop from esp32_rpm_full_test.ino with real
  data pulled from a Speeduino ECU over Bluetooth Classic (SPP), using the
  same Nextion object names, colors, thresholds and "only send when
  changed" apply*() functions built and tested in that file.

  CONNECTIONS:
    Nextion  <- UART2 (GPIO4 RX2 / GPIO5 TX2), NOT Serial/USB anymore.
                nextionBaud must match Page0's Preinitialize Event
                "bauds=" setting.
                  ESP32 RX2 (4) <- Nextion TX
                  ESP32 TX2 (5) -> Nextion RX
                (moved off GPIO16/17: those are only free as GPIO on the
                plain WROOM module - on a WROVER-variant board they're
                permanently wired to PSRAM and can't act as a UART at
                all, which is the likely reason 16/17 gave no reply)
    Speeduino <- Bluetooth Classic SPP, via a Bluetooth-serial module
                (e.g. HC-05/06) wired to Speeduino's TX/RX pins. The ESP32
                connects to that module as the Bluetooth MASTER - no extra
                GPIO wiring needed on the ESP32 side, this all happens over
                the radio.

  SPEEDUINO_BT_MAC below is set to 78:D8:5D:10:22:77 - the address you
  found with esp32_bt_scan.ino. If you ever swap the Bluetooth module,
  re-run that scan sketch and update the address here.

  SPEEDUINO PROTOCOL: sending the single ASCII byte 'A' makes Speeduino
  reply with a raw binary block of live data, no header/length/CRC, byte 0
  starting immediately. Offsets below are confirmed against Speeduino's
  own reference/speeduino.ini + logger.cpp source for the current stable
  release (202501.7) and current dev branch - offsets 0-124 are identical
  between them, so this should hold for any reasonably recent firmware.
  If your firmware is old (pre ~2020) or you see obviously wrong values,
  check reference/speeduino.ini in YOUR firmware's exact source tree
  against these offsets - Speeduino only ever appends new fields at the
  end, it doesn't move existing ones, but very old versions may predate
  some of these.

    offset  size  field           notes
    ------  ----  --------------  -----------------------------------------
    4       U16   MAP             kPa (not currently used/displayed)
    6       U08   IAT (raw)       actual C = raw - 40
    7       U08   coolant (raw)   actual C = raw - 40
    9       U08   battery volts   raw already = volts*10 (e.g. 125 = 12.5V)
    10      U08   AFR             raw already = AFR*10 (e.g. 147 = 14.7)
    14      U16   RPM             little-endian, straight rpm value
    24      S08   advance         ignition timing, degrees BTDC directly
                                   (signed - negative = retarded past TDC,
                                   no offset math needed, unlike IAT/CLT)
    25      U08   TPS (raw)       actual % = raw * 0.5
    104     U16   VSS (speed)     km/h, little-endian
    106     U08   gear            raw gear number; 0 if no gear input wired

  CHANNELS -> NEXTION:
    RPM       -> j2 (bar) / n0 (number, pco+borderc) / t0 (label, pco)
                 + bt0-bt6 shift lights + flashing redline above 7500rpm
    Coolant   -> j0 (bar) / n2 (number, pco+borderc) / t4 (label, pco)
    AFR       -> j1 (bar) / t12 (decimal text, pco) / t5 (label, pco)
    TPS       -> j3 (bar) / n4 (number) - value only, no color
    IAT       -> j4 (bar) / n5 (number, pco+borderc) / t7 (label, pco)
    Battery   -> j5 (bar) / t13 (decimal text, pco) / t8 (label, pco)
    Speed     -> n1 (number) - value only, no color, raw km/h
    Gear      -> t2 (text) - value only, no color, raw gear number as string
    Ignition  -> j6 (bar) / n7 (number) - value only, no color, -20 to 45 deg
                 (t9 label not touched, same as TPS's t6)
    Fuel      -> j8 (bar) / n9 (number, pco+borderc) / t11 (label, pco)
                 red under 20%, green rest - NOT from Speeduino, read from
                 a resistive sender (300ohm empty/35ohm full) through its
                 own voltage divider into GPIO34. See the "Fuel sender"
                 section further down for the wiring and math.

  All color zones/thresholds are identical to esp32_rpm_full_test.ino - if
  you tune one, tune it there too (or just keep this file as the only
  copy going forward, your call).
*/

#include "BluetoothSerial.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth Classic is not enabled for this build - in Arduino IDE, Tools > Partition Scheme, pick one with enough app space (e.g. "Default" or "Huge APP"), and make sure you're targeting a classic ESP32 board (not S3/C3, which don't support Bluetooth Classic/SPP).
#endif

HardwareSerial NextionSerial(2); // UART2, remapped below to GPIO4(RX2)/5(TX2)
const int NEXTION_RX_PIN = 4; // ESP32 RX2 <- Nextion TX
const int NEXTION_TX_PIN = 5; // ESP32 TX2 -> Nextion RX

BluetoothSerial SerialSpeeduino;

// Bluetooth-serial module's MAC address (78:D8:5D:10:22:77)
uint8_t SPEEDUINO_BT_MAC[6] = { 0x78, 0xD8, 0x5D, 0x10, 0x22, 0x77 };

const uint32_t nextionBaud = 115200; // must match Page0's Preinitialize Event "bauds="

const char* BAR_OBJNAME  = "j2";
const char* NUM_OBJNAME  = "n0";
const char* TEXT_OBJNAME = "t0";

const char* TEMP_BAR_OBJNAME  = "j0";
const char* TEMP_NUM_OBJNAME  = "n2";
const char* TEMP_TEXT_OBJNAME = "t4";

const char* AFR_BAR_OBJNAME   = "j1";
const char* AFR_TEXT_OBJNAME  = "t12";
const char* AFR_LABEL_OBJNAME = "t5";

const char* TPS_BAR_OBJNAME = "j3";
const char* TPS_NUM_OBJNAME = "n4";

const char* IAT_BAR_OBJNAME  = "j4";
const char* IAT_NUM_OBJNAME  = "n5";
const char* IAT_TEXT_OBJNAME = "t7";

const char* BATTERY_BAR_OBJNAME   = "j5";
const char* BATTERY_TEXT_OBJNAME  = "t13";
const char* BATTERY_LABEL_OBJNAME = "t8";

const char* SPEED_OBJNAME = "n1"; // value only, no color, raw km/h
const char* GEAR_OBJNAME  = "t2"; // value only, no color, raw gear number as text

// Ignition advance: value only, no color, per your instruction - t9 (label)
// isn't touched at all, same as TPS's t6.
const char* IGN_BAR_OBJNAME = "j6";
const char* IGN_NUM_OBJNAME = "n7";

// Fuel level: same full color pattern as coolant/IAT (bar+number+label).
// NOT from Speeduino - read directly from a resistive sender via its own
// ADC pin, see the "Fuel sender" section below.
const char* FUEL_BAR_OBJNAME   = "j8";
const char* FUEL_NUM_OBJNAME   = "n9";
const char* FUEL_LABEL_OBJNAME = "t11";

const int TACH_MAX_RPM = 8000;
const int COOLANT_MAX_C = 120;
const int IAT_MAX_C = 100;

const int AFR_MIN_X10 = 100; // 10.0
const int AFR_MAX_X10 = 200; // 20.0

const int BATTERY_MIN_X10 = 80;  // 8.0V
const int BATTERY_MAX_X10 = 160; // 16.0V

const int IGN_MIN_DEG = -20; // degrees BTDC (negative = retarded past TDC)
const int IGN_MAX_DEG = 45;

// --- fuel color zone threshold ---
const int FUEL_RED_MAX_PCT = 20; // < this -> red, else green

// --- your exact color values (decimal, as shown in the Nextion Editor) ---
const long COLOR_BLACK  = 6404;
const long COLOR_RED    = 64197;
const long COLOR_BLUE   = 15964;
const long COLOR_GREEN  = 20113;
const long COLOR_YELLOW = 65504;
const long COLOR_WHITE  = 65535;

// --- RPM color zone thresholds ---
const int RPM_YELLOW = 6000;
const int RPM_RED    = 6500;

long colorForRpm(long rpm) {
  if (rpm >= RPM_RED)    return COLOR_RED;
  if (rpm >= RPM_YELLOW) return COLOR_YELLOW;
  return COLOR_GREEN;
}

// --- coolant color zone thresholds ---
const int COOLANT_GREEN_START = 50;
const int COOLANT_RED_START   = 95;

long colorForCoolant(long tempC) {
  if (tempC >= COOLANT_RED_START)   return COLOR_RED;
  if (tempC >= COOLANT_GREEN_START) return COLOR_GREEN;
  return COLOR_BLUE;
}

// --- AFR color zones (compared in tenths) ---
const int AFR_GREEN_START_X10 = 130; // >= 13.0 -> green
const int AFR_RED_START_X10   = 160; // >= 16.0 -> red

long colorForAfr(long valueX10) {
  if (valueX10 >= AFR_RED_START_X10)   return COLOR_RED;
  if (valueX10 >= AFR_GREEN_START_X10) return COLOR_GREEN;
  return COLOR_YELLOW;
}

// --- IAT color zones ---
const int IAT_GREEN_START = 20;
const int IAT_RED_START   = 60;

long colorForIat(long tempC) {
  if (tempC >= IAT_RED_START)   return COLOR_RED;
  if (tempC >= IAT_GREEN_START) return COLOR_GREEN;
  return COLOR_BLUE;
}

// --- battery color zones (compared in tenths); red-green-red ---
const int BATTERY_RED_LOW_MAX_X10  = 130; // < 13.0 -> red (undercharging)
const int BATTERY_RED_HIGH_MIN_X10 = 150; // >= 15.0 -> red (overvoltage)

long colorForBattery(long voltX10) {
  if (voltX10 < BATTERY_RED_LOW_MAX_X10)  return COLOR_RED;
  if (voltX10 < BATTERY_RED_HIGH_MIN_X10) return COLOR_GREEN;
  return COLOR_RED;
}

long colorForFuel(long pct) {
  if (pct < FUEL_RED_MAX_PCT) return COLOR_RED;
  return COLOR_GREEN;
}

// --- flashing redline warning ---
const int FLASH_RPM = 7500;
const uint32_t FLASH_INTERVAL_MS = 500;

// --- shift-light buttons: name + threshold, cumulative on/off ---
struct ShiftLight {
  const char* name;
  int threshold;
  int lastState; // -1 = never sent yet
};

ShiftLight lights[] = {
  {"bt0", 5000, -1},
  {"bt1", 5333, -1},
  {"bt2", 5667, -1},
  {"bt3", 6000, -1},
  {"bt4", 6250, -1},
  {"bt5", 6500, -1},
  {"bt6", 6750, -1},
};
const int NUM_LIGHTS = sizeof(lights) / sizeof(lights[0]);

// --- last-sent state, so we only write to Nextion when something changes ---
int lastSentPct = -1;
int lastSentRpm = -1;
long lastSentColor = -1;

int lastSentCoolantPct = -1;
int lastSentCoolantC = -1;
long lastSentCoolantColor = -1;

int lastSentAfrPct = -1;
int lastSentAfrValX10 = -1;
long lastSentAfrColor = -1;

int lastSentTpsPct = -1;
int lastSentTpsVal = -1;

int lastSentIatPct = -1;
int lastSentIatC = -1;
long lastSentIatColor = -1;

int lastSentBatteryPct = -1;
int lastSentBatteryValX10 = -1;
long lastSentBatteryColor = -1;

long lastSentSpeed = -1;
long lastSentGear = -1;

int lastSentIgnPct = -1;
long lastSentIgnVal = -1000; // outside the valid -20..45 range - never collides with a real reading

int lastSentFuelPct = -1;
long lastSentFuelColor = -1;

void nextionEnd() {
  NextionSerial.write(0xFF);
  NextionSerial.write(0xFF);
  NextionSerial.write(0xFF);
}

void nextionSetVal(const char* component, long value) {
  NextionSerial.printf("%s.val=%ld", component, value);
  nextionEnd();
}

void nextionSetPco(const char* component, long color) {
  NextionSerial.printf("%s.pco=%ld", component, color);
  nextionEnd();
}

void nextionSetBorderc(const char* component, long color) {
  NextionSerial.printf("%s.borderc=%ld", component, color);
  nextionEnd();
  NextionSerial.printf("ref %s", component); // borderc needs an explicit redraw
  nextionEnd();
}

void nextionSetText(const char* component, const char* text) {
  NextionSerial.printf("%s.txt=\"%s\"", component, text);
  nextionEnd();
}

int valueToPct(long value, long maxValue) {
  if (value < 0) value = 0;
  if (value > maxValue) value = maxValue;
  int pct = (int)((100 * value + maxValue / 2) / maxValue);
  if (pct > 100) pct = 100;
  return pct;
}

void applyColorZone(long rpm) {
  long color = colorForRpm(rpm);
  if (color != lastSentColor) {
    nextionSetPco(BAR_OBJNAME, color);
    nextionSetPco(NUM_OBJNAME, color);
    nextionSetBorderc(NUM_OBJNAME, color);
    nextionSetPco(TEXT_OBJNAME, color);
    lastSentColor = color;
  }
}

void applyShiftLights(long rpm) {
  bool flashing = (rpm >= FLASH_RPM);
  bool flashOn = ((millis() / FLASH_INTERVAL_MS) % 2) == 0;

  for (int i = 0; i < NUM_LIGHTS; i++) {
    int state;
    if (flashing) {
      state = flashOn ? 1 : 0;
    } else {
      state = (rpm >= lights[i].threshold) ? 1 : 0;
    }
    if (state != lights[i].lastState) {
      nextionSetVal(lights[i].name, state);
      lights[i].lastState = state;
    }
  }
}

void applyRpm(long rpm) {
  int pct = valueToPct(rpm, TACH_MAX_RPM);
  if (pct != lastSentPct) {
    nextionSetVal(BAR_OBJNAME, pct);
    lastSentPct = pct;
  }
  if (rpm != lastSentRpm) {
    nextionSetVal(NUM_OBJNAME, rpm);
    lastSentRpm = rpm;
  }
  applyColorZone(rpm);
  applyShiftLights(rpm);
}

void applyCoolant(long tempC) {
  int pct = valueToPct(tempC, COOLANT_MAX_C);
  if (pct != lastSentCoolantPct) {
    nextionSetVal(TEMP_BAR_OBJNAME, pct);
    lastSentCoolantPct = pct;
  }
  if (tempC != lastSentCoolantC) {
    nextionSetVal(TEMP_NUM_OBJNAME, tempC);
    lastSentCoolantC = tempC;
  }

  long color = colorForCoolant(tempC);
  if (color != lastSentCoolantColor) {
    nextionSetPco(TEMP_BAR_OBJNAME, color);
    nextionSetPco(TEMP_NUM_OBJNAME, color);
    nextionSetBorderc(TEMP_NUM_OBJNAME, color);
    nextionSetPco(TEMP_TEXT_OBJNAME, color);
    lastSentCoolantColor = color;
  }
}

void applyAfr(long valueX10) {
  int pct = valueToPct(valueX10 - AFR_MIN_X10, AFR_MAX_X10 - AFR_MIN_X10);
  if (pct != lastSentAfrPct) {
    nextionSetVal(AFR_BAR_OBJNAME, pct);
    lastSentAfrPct = pct;
  }
  if (valueX10 != lastSentAfrValX10) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", valueX10 / 10.0);
    nextionSetText(AFR_TEXT_OBJNAME, buf);
    lastSentAfrValX10 = valueX10;
  }

  long color = colorForAfr(valueX10);
  if (color != lastSentAfrColor) {
    nextionSetPco(AFR_BAR_OBJNAME, color);
    nextionSetPco(AFR_TEXT_OBJNAME, color);
    nextionSetPco(AFR_LABEL_OBJNAME, color);
    lastSentAfrColor = color;
  }
}

void applyIat(long tempC) {
  int pct = valueToPct(tempC, IAT_MAX_C);
  if (pct != lastSentIatPct) {
    nextionSetVal(IAT_BAR_OBJNAME, pct);
    lastSentIatPct = pct;
  }
  if (tempC != lastSentIatC) {
    nextionSetVal(IAT_NUM_OBJNAME, tempC);
    lastSentIatC = tempC;
  }

  long color = colorForIat(tempC);
  if (color != lastSentIatColor) {
    nextionSetPco(IAT_BAR_OBJNAME, color);
    nextionSetPco(IAT_NUM_OBJNAME, color);
    nextionSetBorderc(IAT_NUM_OBJNAME, color);
    nextionSetPco(IAT_TEXT_OBJNAME, color);
    lastSentIatColor = color;
  }
}

void applyBattery(long voltX10) {
  int pct = valueToPct(voltX10 - BATTERY_MIN_X10, BATTERY_MAX_X10 - BATTERY_MIN_X10);
  if (pct != lastSentBatteryPct) {
    nextionSetVal(BATTERY_BAR_OBJNAME, pct);
    lastSentBatteryPct = pct;
  }
  if (voltX10 != lastSentBatteryValX10) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", voltX10 / 10.0);
    nextionSetText(BATTERY_TEXT_OBJNAME, buf);
    lastSentBatteryValX10 = voltX10;
  }

  long color = colorForBattery(voltX10);
  if (color != lastSentBatteryColor) {
    nextionSetPco(BATTERY_BAR_OBJNAME, color);
    nextionSetPco(BATTERY_TEXT_OBJNAME, color);
    nextionSetPco(BATTERY_LABEL_OBJNAME, color);
    lastSentBatteryColor = color;
  }
}

// value-only, no color, per your instruction
void applyTps(long pct) {
  if (pct != lastSentTpsPct) {
    nextionSetVal(TPS_BAR_OBJNAME, pct);
    lastSentTpsPct = pct;
  }
  if (pct != lastSentTpsVal) {
    nextionSetVal(TPS_NUM_OBJNAME, pct);
    lastSentTpsVal = pct;
  }
}

// value-only, no color - raw km/h straight from Speeduino's VSS output
void applySpeed(long kph) {
  if (kph != lastSentSpeed) {
    nextionSetVal(SPEED_OBJNAME, kph);
    lastSentSpeed = kph;
  }
}

// value-only, no color - raw gear number as text (0 if no gear input wired
// on Speeduino)
void applyGear(long gear) {
  if (gear != lastSentGear) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%ld", gear);
    nextionSetText(GEAR_OBJNAME, buf);
    lastSentGear = gear;
  }
}

// Ignition advance in degrees BTDC, -20..45 - value only, no color, per
// your instruction. t9 (label) isn't touched at all, same as TPS's t6.
void applyIgnition(long deg) {
  int pct = valueToPct(deg - IGN_MIN_DEG, IGN_MAX_DEG - IGN_MIN_DEG);
  if (pct != lastSentIgnPct) {
    nextionSetVal(IGN_BAR_OBJNAME, pct);
    lastSentIgnPct = pct;
  }
  if (deg != lastSentIgnVal) {
    nextionSetVal(IGN_NUM_OBJNAME, deg); // Nextion Number supports negative values fine
    lastSentIgnVal = deg;
  }
}

// Fuel level, already a smoothed 0-100 percentage by the time it gets here
// (see readFuelPercentSmoothed()) - red under 20%, green otherwise.
void applyFuel(long pct) {
  if (pct != lastSentFuelPct) {
    nextionSetVal(FUEL_BAR_OBJNAME, pct);
    nextionSetVal(FUEL_NUM_OBJNAME, pct);
    lastSentFuelPct = pct;
  }

  long color = colorForFuel(pct);
  if (color != lastSentFuelColor) {
    nextionSetPco(FUEL_BAR_OBJNAME, color);
    nextionSetPco(FUEL_NUM_OBJNAME, color);
    nextionSetBorderc(FUEL_NUM_OBJNAME, color);
    nextionSetPco(FUEL_LABEL_OBJNAME, color);
    lastSentFuelColor = color;
  }
}

// ---------------------------------------------------------------------
// Speeduino link (Bluetooth Classic SPP)
// ---------------------------------------------------------------------

const uint32_t BT_RECONNECT_INTERVAL_MS = 5000; // how often to retry if not connected
uint32_t lastBtAttempt = 0;

void connectSpeeduinoIfNeeded() {
  if (SerialSpeeduino.connected(0)) return;
  if (millis() - lastBtAttempt < BT_RECONNECT_INTERVAL_MS) return;
  lastBtAttempt = millis();
  SerialSpeeduino.connect(SPEEDUINO_BT_MAC); // blocks briefly (up to a few sec) while it tries
}

// Bluetooth Classic SPP has real, variable latency (tens of ms is normal
// for an HC-05/06-style module) - polling every 40ms with only a "drain
// whatever's sitting in the buffer right now" flush was too aggressive:
// stragglers from the PREVIOUS response could still be in flight over the
// air, arrive AFTER that one-shot flush, and land mixed in with the START
// of the next response. That shifts every byte offset by a few bytes on a
// random subset of polls, so RPM/coolant/etc parse as noisy garbage that's
// different every single cycle - which is exactly "everything flashing":
// every apply*() sees a "changed" value 25 times a second and redraws.
//
// Fix: poll slower, wait longer for the reply, and flush by waiting for
// the RX buffer to go quiet for a stretch (not just momentarily empty)
// before sending the next 'A'.
const uint32_t SPEEDUINO_POLL_INTERVAL_MS = 100; // ~10Hz - plenty smooth for gauges, much safer over BT
const uint32_t SPEEDUINO_RESPONSE_TIMEOUT_MS = 250; // generous - BT SPP round-trip can be slow
const uint32_t SPEEDUINO_FLUSH_QUIET_MS = 30; // RX must be empty for this long before we consider it flushed
const int SPEEDUINO_RESPONSE_MIN_BYTES = 107; // need through offset 106 (gear)
uint8_t speeduinoBuf[140];
uint32_t lastPollTime = 0;

// Keeps reading and discarding bytes until none have arrived for
// SPEEDUINO_FLUSH_QUIET_MS straight - a plain "while(available()) read()"
// can return while stragglers are still travelling over the radio.
void flushSpeeduinoRx() {
  uint32_t quietSince = millis();
  while (millis() - quietSince < SPEEDUINO_FLUSH_QUIET_MS) {
    if (SerialSpeeduino.available()) {
      SerialSpeeduino.read();
      quietSince = millis(); // saw a byte - reset the quiet timer
    }
  }
}

// sends 'A', waits for the reply, returns true if we got at least the
// bytes we need. Returns false (and leaves gauges alone) on any
// timeout/short read - never parses a partial/garbage buffer.
bool requestSpeeduinoData() {
  if (!SerialSpeeduino.connected(0)) return false;

  flushSpeeduinoRx();
  SerialSpeeduino.write('A');

  uint32_t start = millis();
  int received = 0;
  while (millis() - start < SPEEDUINO_RESPONSE_TIMEOUT_MS && received < SPEEDUINO_RESPONSE_MIN_BYTES) {
    if (SerialSpeeduino.available()) {
      speeduinoBuf[received++] = SerialSpeeduino.read();
    }
  }
  return received >= SPEEDUINO_RESPONSE_MIN_BYTES;
}

// Cheap extra safety net on top of the length check above: even a
// full-length read can still be misaligned garbage. RPM > 12000 is not a
// real reading on anything this dash would be bolted to, so treat it as a
// bad frame and skip the cycle rather than let a bogus number trigger the
// flashing-redline warning or a nonsense gauge jump.
const long RPM_SANITY_MAX = 12000;

// ---------------------------------------------------------------------
// Fuel sender - NOT from Speeduino. A bare resistive sender (300 ohm
// empty, 35 ohm full, per your measurement) wired as a voltage divider:
//   3.3V --[150 ohm fixed resistor]--+--[sender, 300-35 ohm]-- GND
//                                     |
//                                  GPIO34 (ADC1, safe from Bluetooth's
//                                  ADC2 conflicts, input-only pin)
// Add a 1uF cap from GPIO34 to GND at the board for basic noise
// filtering; the exponential smoothing below handles the much bigger
// problem of fuel physically sloshing around while driving.
// ---------------------------------------------------------------------
const int FUEL_ADC_PIN = 34;
const float FUEL_DIVIDER_FIXED_OHMS = 150.0f; // the fixed resistor in the divider - update this if you change the resistor
const float FUEL_EMPTY_OHMS = 300.0f;
const float FUEL_FULL_OHMS  = 35.0f;
const uint32_t FUEL_READ_INTERVAL_MS = 200; // how often to take a raw ADC sample
const float FUEL_SMOOTHING_ALPHA = 0.02f;   // heavy smoothing - fuel sloshes a lot

// This assumes resistance-vs-fuel-level is roughly LINEAR between your two
// measured points (300 ohm empty, 35 ohm full). Real senders aren't always
// perfectly linear across the whole tank - if the gauge reads noticeably
// off around half-full, measure the resistance at a known half-tank fill
// and we can switch this to a proper multi-point lookup table instead.
float fuelFilteredPct = -1.0f; // -1 = not yet initialized
uint32_t lastFuelRead = 0;

float readFuelPercentRaw() {
  int adc = analogRead(FUEL_ADC_PIN); // 0-4095 (12-bit)
  float voltage = (adc / 4095.0f) * 3.3f;
  if (voltage < 0.01f) voltage = 0.01f;   // avoid divide-by-zero / negative resistance
  if (voltage > 3.29f) voltage = 3.29f;   // avoid divide-by-zero as voltage -> 3.3V

  float senderOhms = FUEL_DIVIDER_FIXED_OHMS * voltage / (3.3f - voltage);
  float pct = (FUEL_EMPTY_OHMS - senderOhms) / (FUEL_EMPTY_OHMS - FUEL_FULL_OHMS) * 100.0f;
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

void updateFuelLevel() {
  if (millis() - lastFuelRead < FUEL_READ_INTERVAL_MS) return;
  lastFuelRead = millis();

  float raw = readFuelPercentRaw();
  if (fuelFilteredPct < 0) {
    fuelFilteredPct = raw; // first reading - snap straight to it, don't ramp up from 0
  } else {
    fuelFilteredPct += (raw - fuelFilteredPct) * FUEL_SMOOTHING_ALPHA;
  }

  applyFuel((long)(fuelFilteredPct + 0.5f));
}

void setup() {
  analogSetPinAttenuation(FUEL_ADC_PIN, ADC_11db); // full 0-3.3V range on the fuel ADC pin

  NextionSerial.begin(nextionBaud, SERIAL_8N1, NEXTION_RX_PIN, NEXTION_TX_PIN);
  delay(300);

  NextionSerial.print("page 0");
  nextionEnd();

  // startup state - so the dashboard looks correct even before the first
  // Speeduino reply arrives
  applyRpm(0);
  applyCoolant(0);
  applyAfr(AFR_MIN_X10);
  applyTps(0);
  applyIat(0);
  applyBattery(BATTERY_MIN_X10);
  applySpeed(0);
  applyGear(0);
  applyIgnition(0);
  updateFuelLevel(); // first real fuel reading, snapped straight in (no ramp-up)

  SerialSpeeduino.begin("ESP32_Dash", true); // name only matters for BT discovery logs; true = master
}

void loop() {
  updateFuelLevel(); // independent of Speeduino/Bluetooth - runs regardless of BT state

  connectSpeeduinoIfNeeded();

  if (millis() - lastPollTime >= SPEEDUINO_POLL_INTERVAL_MS) {
    lastPollTime = millis();

    if (requestSpeeduinoData()) {
      long rpm        = speeduinoBuf[14] | ((long)speeduinoBuf[15] << 8);
      long coolantC   = (long)speeduinoBuf[7] - 40;
      long iatC       = (long)speeduinoBuf[6] - 40;
      long batteryX10 = speeduinoBuf[9];
      long afrX10     = speeduinoBuf[10];
      long tpsPct     = speeduinoBuf[25] / 2;
      long vss        = speeduinoBuf[104] | ((long)speeduinoBuf[105] << 8);
      long gear       = speeduinoBuf[106];
      long ignitionDeg = (int8_t)speeduinoBuf[24]; // signed byte, real degrees BTDC directly

      if (rpm > RPM_SANITY_MAX) {
        // almost certainly a misaligned/corrupted frame - skip this whole
        // cycle rather than display or act on any of it
        return;
      }

      applyRpm(rpm);
      applyCoolant(coolantC);
      applyAfr(afrX10);
      applyTps(tpsPct);
      applyIat(iatC);
      applyBattery(batteryX10);
      applySpeed(vss);
      applyGear(gear);
      applyIgnition(ignitionDeg);
    }
    // if requestSpeeduinoData() returned false (not connected, or a
    // timeout/short read), we simply skip this cycle - gauges keep
    // showing their last good values instead of jumping to garbage.
  }
}
