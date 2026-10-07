// =====================================================================
//  AX Race Dash - LIVE  (Speeduino --Bluetooth--> ESP32 --> Nextion NX8048T070)
//
//  Board: original ESP32 / ESP32-WROOM-32 (needs Bluetooth Classic - S3 / C3 will NOT work)
//  Arduino IDE: Board "ESP32 Dev Module",
//               Partition Scheme "Huge APP (3MB No OTA)" (or "Minimal SPIFFS" - both fit)
//  Power: solid 5 V supply + 470-1000uF at the ESP32 - Bluetooth current spikes
//         caused "gauges reset every second" brown-outs in your earlier build.
//
//  Wiring: ESP32 GPIO5 (TX) -> Nextion RX (yellow)
//          ESP32 GPIO4 (RX) <- Nextion TX (blue)
//          GND common, Nextion 5V from its own 1 A+ supply
//
//  Serial Monitor (115200): type  d  = toggle raw Speeduino packet dump
//                                  n  = toggle bytes received from the Nextion
//                                  s  = print decoded values once
//
//  Everything you may need to change is in the CONFIG section below.
// =====================================================================

#include <Arduino.h>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <stdarg.h>

// =====================================================================
//  CONFIG
// =====================================================================
// ---- Nextion ----
#define NEX Serial2
const int NEX_RX = 4, NEX_TX = 5;
const uint32_t NEX_BAUD = 115200;

// ---- Bluetooth link to the HC-05/HC-06 on the Speeduino ----
const char *ESP_BT_NAME = "AXDash";
const bool  BT_USE_ADDRESS = true;                         // connect by MAC (your working setup)
const char *ECU_BT_NAME = "HC-05";                         // only used if BT_USE_ADDRESS = false
uint8_t     ECU_BT_ADDR[6] = {0x78, 0xD8, 0x5D, 0x10, 0x22, 0x77};  // 78:D8:5D:10:22:77 (from esp32_bt_scan)
const char *ECU_BT_PIN = "1234";

// ---- Speeduino request: legacy 'A' command (same as your working dash) ----
// 'A' returns a raw live-data block, byte 0 = first data byte, no header.
// Offsets checked against speeduino.ini 202501.x (same as your repo).
const int  POLL_MS         = 100;    // 10 Hz - faster polling over BT caused misaligned frames before
const int  RESP_TIMEOUT_MS = 250;    // BT SPP round trip can be slow
const int  FLUSH_QUIET_MS  = 30;     // RX must be silent this long before a new request
const int  OFS_IAT    = 6;    // U08, degC + 40
const int  OFS_CLT    = 7;    // U08, degC + 40
const int  OFS_BATT   = 9;    // U08, volts * 10
const int  OFS_AFR    = 10;   // U08, AFR * 10
const int  OFS_RPM    = 14;   // U16 little-endian
const int  OFS_VSS    = 104;  // U16 km/h        (-1 = not used, shows "--")
const int  OFS_GEAR   = 106;  // U08, 0 = none   (-1 = not used)
const int  OFS_OIL    = -1;   // U08 oil pressure: set 108 if you have an oil pressure sensor on Speeduino
const bool OIL_RAW_IS_PSI = true;                          // Speeduino reports oil pressure in psi
const int  RESP_MIN   = (OFS_OIL > OFS_GEAR ? OFS_OIL : OFS_GEAR) + 1;   // bytes we need from each reply

// ---- fuel level: resistive sender on GPIO34 (same divider as your repo) ----
//   3.3V --[150 ohm]--+--[sender 300 ohm empty / 35 ohm full]-- GND,  1uF GPIO34 -> GND
const int   FUEL_PIN = 34;                                 // -1 = no sender, show "--"
const float FUEL_DIVIDER_FIXED_OHMS = 150.0f;
const float FUEL_EMPTY_OHMS = 300.0f, FUEL_FULL_OHMS = 35.0f;

// ---- gear: from Speeduino (needs VSS + gear ratios in TunerStudio) or calculated here ----
const bool  CALC_GEAR = true;                              // use ratios below if the ECU gives 0
const float RATIO[7] = {0, 3.36, 2.07, 1.43, 1.13, 0.92, 0.78};
const float FINAL_DRIVE = 4.10;
const float TYRE_CIRC_M = 1.90;

// ---- picture IDs - must match the Picture list in the Nextion Editor ----
const int PIC_RPM_ON = 2;                                  // normal RPM bar image (j0.ppic)
const int PIC_GEAR_R = 4, PIC_GEAR_N = 5;                  // gear n uses PIC_GEAR_N + n
const int PIC_TOG_C = 27, PIC_TOG_F = 28, PIC_TOG_BAR = 29, PIC_TOG_PSI = 30;
const int PIC_TOG_KMH = 31, PIC_TOG_MPH = 32;

// ---- colours (RGB565) ----
const uint16_t C_WHITE = 61342, C_AMBER = 62855, C_RED = 57929, C_GREEN = 16112;

// ---- timing ----
const uint32_t TILE_MS       = 200;   // tiles (temps, battery, AFR, fuel) refresh
const uint32_t STALE_MS      = 600;   // no packet for this long -> show "--"
const uint32_t PAGE_POLL_MS  = 1000;  // safety net: ask the Nextion which page it shows
const uint32_t BRI_POLL_MS   = 150;   // safety net: read the brightness slider on page 1

#define DEG "\xB0"                    // degree sign in the Nextion's ISO-8859-1 font

// =====================================================================
//  Types (all structs ABOVE the first function - Arduino IDE rule)
// =====================================================================
struct Settings {
  uint8_t bright = 80;
  bool tempF = false, psi = false, mph = false;
  float barStart = 1000, barFull = 8500, shiftOn = 7600, shiftOff = 7400, flashMs = 80;
  float waterHot = 105, airHot = 60, oilLow = 1.0, oilCheckRpm = 2000;
  float battLow = 12.0, battHigh = 15.0, fuelLow = 15, afrLean = 15.0;
};
Settings S;
Preferences prefs;
bool settingsDirty = false;

enum Kind { K_RPM, K_MS, K_TEMP, K_BAR, K_VOLT, K_PCT, K_AFR };
struct Param { uint8_t page; const char *obj; float *v; float step, lo, hi; Kind kind; };
Param PARAMS[] = {
  // page 2 - shift light   (buttons send codes 0x20..0x29)
  {2, "tP0", &S.barStart,    100,  0,    6000,  K_RPM},
  {2, "tP1", &S.barFull,     100,  3000, 12000, K_RPM},
  {2, "tP2", &S.shiftOn,     50,   2000, 12000, K_RPM},
  {2, "tP3", &S.shiftOff,    50,   1500, 12000, K_RPM},
  {2, "tP4", &S.flashMs,     10,   40,   300,   K_MS},
  // page 3 - warnings      (buttons send codes 0x30..0x3F)
  {3, "tW0", &S.waterHot,    1,    70,   130,   K_TEMP},
  {3, "tW1", &S.airHot,      1,    20,   90,    K_TEMP},
  {3, "tW2", &S.oilLow,      0.1,  0.2,  5.0,   K_BAR},
  {3, "tW3", &S.oilCheckRpm, 100,  0,    6000,  K_RPM},
  {3, "tW4", &S.battLow,     0.1,  10.0, 13.5,  K_VOLT},
  {3, "tW5", &S.battHigh,    0.1,  13.5, 16.5,  K_VOLT},
  {3, "tW6", &S.fuelLow,     1,    0,    50,    K_PCT},
  {3, "tW7", &S.afrLean,     0.1,  13.0, 18.0,  K_AFR},
};
const int N_PARAMS = sizeof(PARAMS) / sizeof(PARAMS[0]);

struct Tile { const char *t; const char *h; float lo, hi; uint8_t dec; };
const Tile TILES[6] = {
  {"tOil", "hOil",  0,   7, 1},   // bar  (bands are always metric)
  {"tWat", "hWat", 40, 120, 0},   // degC
  {"tBat", "hBat", 10,  16, 1},   // V
  {"tAfr", "hAfr", 10,  18, 1},   // AFR
  {"tFue", "hFue",  0, 100, 0},   // %
  {"tAir", "hAir",  0,  80, 0},   // degC
};
enum { OIL, WAT, BAT, AFR, FUE, AIR };

// Last command sent to each dash field = what the dash SHOULD show.
// Fixed-size char buffers instead of String: no heap churn on every update.
struct Cache { char cmd[40]; };
Cache cRpm, cRpmCol, cBar, cGear, cSpd, cVal[6], cPos[6], cCol[6], cUOil, cUWat, cUAir, cUSpd;

struct Peaks { float rpm = 0, kmh = 0, water = 0, oil = 99; };
Peaks PK;

// live data from the ECU task (core 0) to the display loop (core 1)
struct EcuData {
  float rpm = 0, kmh = 0, clt = 0, iat = 0, batt = 0, afr = 0, oilBar = 0;
  int gear = 0;
  uint32_t stamp = 0;            // millis() of the last good packet
};
EcuData ECU;
portMUX_TYPE ecuMux = portMUX_INITIALIZER_UNLOCKED;
enum LinkState { LINK_SEARCHING, LINK_CONNECTED };
volatile LinkState linkState = LINK_SEARCHING;
volatile uint16_t pktPerSec = 0;
volatile uint32_t pktErrors = 0;
volatile bool dumpRaw = false;
bool dumpNex = false;

BluetoothSerial SerialBT;

bool trackPeaks = false;
float curRpm = 0;
float lastTile[6], lastKmh = -1;
bool lastWarn[6], tileValid[6];
bool showingNoData = false;

int curPage = 0;
bool shiftOn = false;
int heldCode = -1;
uint32_t heldSince = 0, lastRepeat = 0, lastStatus = 0, pageFilledAt = 0;

// =====================================================================
//  Nextion output
// =====================================================================
void nexCmd(const char *c) {
  static const uint8_t END[3] = {0xFF, 0xFF, 0xFF};
  NEX.write((const uint8_t *)c, strlen(c));
  NEX.write(END, 3);
}

void nexCmdf(const char *fmt, ...) {               // printf-style command
  char buf[64];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  nexCmd(buf);
}

void sendTxt(const char *obj, const char *v) { nexCmdf("%s.txt=\"%s\"", obj, v); }

// Dash fields: only send when the command changed, and only while page 0 is shown
void dashSend(Cache &c, const char *cmd) {
  if (strcmp(c.cmd, cmd) == 0) return;
  strlcpy(c.cmd, cmd, sizeof(c.cmd));
  if (curPage == 0) nexCmd(cmd);
}
void setTxt(Cache &c, const char *obj, const char *v) {
  char b[40]; snprintf(b, sizeof(b), "%s.txt=\"%s\"", obj, v); dashSend(c, b);
}
void setNum(Cache &c, const char *obj, const char *attr, int v) {
  char b[40]; snprintf(b, sizeof(b), "%s.%s=%d", obj, attr, v); dashSend(c, b);
}

// =====================================================================
//  Units
// =====================================================================
const char *degUnit() { return S.tempF ? DEG "F" : DEG "C"; }
float toTemp(float c) { return S.tempF ? c * 9.0f / 5.0f + 32.0f : c; }

void updateUnits() {
  setTxt(cUOil, "tUOil", S.psi ? "psi" : "bar");
  setTxt(cUWat, "tUWat", degUnit());
  setTxt(cUAir, "tUAir", degUnit());
  setTxt(cUSpd, "tUSpd", S.mph ? "mph" : "km/h");
}

// =====================================================================
//  Dash elements
// =====================================================================
void showRpmNumber(int rpm) {
  int red = S.shiftOn - 400, amber = S.shiftOn - 2000;
  char v[8]; snprintf(v, sizeof(v), "%d", rpm);
  setTxt(cRpm, "tRpm", v);
  setNum(cRpmCol, "tRpm", "pco", rpm >= red ? C_RED : rpm >= amber ? C_AMBER : C_WHITE);
}

void showBar(int val) { setNum(cBar, "j0", "val", constrain(val, 0, 100)); }

void startShift(bool on) {
  shiftOn = on;
  if (curPage != 0) return;                     // resyncDash() restores it later
  if (on) { nexCmd("j0.val=100"); nexCmd("tmShift.en=1"); }
  else    { nexCmd("tmShift.en=0"); nexCmdf("j0.ppic=%d", PIC_RPM_ON); }
}

void showRpm(int rpm) {
  curRpm = rpm;
  if (trackPeaks) PK.rpm = max(PK.rpm, (float)rpm);
  showRpmNumber(rpm);
  if (!shiftOn && rpm >= S.shiftOn) { strlcpy(cBar.cmd, "j0.val=100", sizeof(cBar.cmd)); startShift(true); }
  else if (shiftOn && rpm < S.shiftOff) startShift(false);
  if (shiftOn) return;
  float span = max(S.barFull - S.barStart, 500.0f);
  int seg = constrain((int)lround((rpm - S.barStart) / span * 40), 0, 40);
  showBar(seg * 5 / 2);                         // whole segments only
}

void showGear(char g) {                         // 'R', 'N', '1'..'6'
  int pic = g == 'R' ? PIC_GEAR_R : g == 'N' ? PIC_GEAR_N : PIC_GEAR_N + (g - '0');
  setNum(cGear, "pGear", "pic", pic);
}

void showSpeed(float kmh) {
  lastKmh = kmh;
  if (trackPeaks) PK.kmh = max(PK.kmh, kmh);
  char v[8]; snprintf(v, sizeof(v), "%d", (int)(S.mph ? kmh * 0.621371f : kmh));
  setTxt(cSpd, "tSpd", v);
}

void showTile(int i, float v, bool warn) {      // v is metric
  const Tile &T = TILES[i];
  lastTile[i] = v; lastWarn[i] = warn; tileValid[i] = true;
  if (trackPeaks && i == WAT) PK.water = max(PK.water, v);
  if (trackPeaks && i == OIL && curRpm > S.oilCheckRpm) PK.oil = min(PK.oil, v);
  char txt[12];
  if (i == OIL && S.psi)          snprintf(txt, sizeof(txt), "%ld", lround(v * 14.5038f));
  else if (i == WAT || i == AIR)  snprintf(txt, sizeof(txt), "%ld", lround(toTemp(v)));
  else                            snprintf(txt, sizeof(txt), "%.*f", T.dec, v);
  setTxt(cVal[i], T.t, txt);
  setNum(cPos[i], T.h, "val", constrain((int)lround((v - T.lo) / (T.hi - T.lo) * 100), 0, 100));
  setNum(cCol[i], T.t, "pco", warn ? C_RED : C_WHITE);
}

void tileDashes(int i, uint16_t color) {        // no value: "--", marker at 0
  tileValid[i] = false;
  setTxt(cVal[i], TILES[i].t, "--");
  setNum(cPos[i], TILES[i].h, "val", 0);
  setNum(cCol[i], TILES[i].t, "pco", color);
}

void showNoData() {                             // link lost: never show frozen values
  if (shiftOn) startShift(false);
  showBar(0);
  setTxt(cRpm, "tRpm", "----");
  setNum(cRpmCol, "tRpm", "pco", C_WHITE);
  setTxt(cSpd, "tSpd", "--");
  lastKmh = -1;
  showGear('N');
  for (int i = 0; i < 6; i++) tileDashes(i, C_AMBER);
}

void resyncDash() {                             // dash page reloaded: send everything again
  nexCmdf("tmShift.tim=%d", (int)S.flashMs);
  Cache *all[] = {&cRpm, &cRpmCol, &cBar, &cGear, &cSpd, &cUOil, &cUWat, &cUAir, &cUSpd};
  for (Cache *c : all) if (c->cmd[0]) nexCmd(c->cmd);
  for (int i = 0; i < 6; i++) {
    if (cVal[i].cmd[0]) nexCmd(cVal[i].cmd);
    if (cPos[i].cmd[0]) nexCmd(cPos[i].cmd);
    if (cCol[i].cmd[0]) nexCmd(cCol[i].cmd);
  }
  if (shiftOn) startShift(true);
}

// =====================================================================
//  Settings pages
// =====================================================================
void loadSettings() {
  prefs.begin("dash", false);
  if (prefs.getBytesLength("s") == sizeof(Settings)) prefs.getBytes("s", &S, sizeof(Settings));
}
void saveSettings() {
  if (!settingsDirty) return;
  prefs.putBytes("s", &S, sizeof(Settings));
  settingsDirty = false;
  Serial.println("settings saved");
}

void fmtParam(const Param &p, char *out, size_t n) {
  float v = *p.v;
  switch (p.kind) {
    case K_TEMP: snprintf(out, n, "%ld%s", lround(toTemp(v)), degUnit()); break;
    case K_BAR:  if (S.psi) snprintf(out, n, "%ld psi", lround(v * 14.5038f)); else snprintf(out, n, "%.1f bar", v); break;
    case K_VOLT: snprintf(out, n, "%.1f V", v); break;
    case K_PCT:  snprintf(out, n, "%d %%", (int)v); break;
    case K_AFR:  snprintf(out, n, "%.1f", v); break;
    default:     snprintf(out, n, "%d", (int)v); break;      // rpm, ms
  }
}

void fmtPeak(int k, char *out, size_t n) {
  strlcpy(out, "--", n);                        // default when there is no peak yet
  switch (k) {
    case 0: if (PK.rpm > 0) snprintf(out, n, "%d", (int)PK.rpm); break;
    case 1: if (PK.kmh > 0) snprintf(out, n, "%d %s", (int)(S.mph ? PK.kmh * 0.621371f : PK.kmh), S.mph ? "mph" : "km/h"); break;
    case 2: if (PK.water > 0) snprintf(out, n, "%ld%s", lround(toTemp(PK.water)), degUnit()); break;
    case 3: if (PK.oil < 99) {
              if (S.psi) snprintf(out, n, "%ld psi", lround(PK.oil * 14.5038f));
              else       snprintf(out, n, "%.1f bar", PK.oil);
            }
            break;
  }
}

void fillStatus() {                             // page 4 live part
  static const char *PK_OBJ[4] = {"tPk0", "tPk1", "tPk2", "tPk3"};
  char s[24];
  for (int k = 0; k < 4; k++) { fmtPeak(k, s, sizeof(s)); sendTxt(PK_OBJ[k], s); }
  bool fresh = millis() - ECU.stamp < STALE_MS;
  if (linkState != LINK_CONNECTED) { sendTxt("tLink", "SEARCHING"); nexCmdf("tLink.pco=%u", C_AMBER); }
  else if (!fresh)                 { sendTxt("tLink", "NO DATA");   nexCmdf("tLink.pco=%u", C_RED); }
  else                             { sendTxt("tLink", "CONNECTED"); nexCmdf("tLink.pco=%u", C_GREEN); }
  snprintf(s, sizeof(s), "%u Hz " "\xB7" " %lu errors", (unsigned)pktPerSec, (unsigned long)pktErrors);
  sendTxt("tHz", s);
}

void fillPage(int pg) {
  char v[16];
  if (pg == 1) {
    nexCmdf("hBri.val=%d", S.bright);
    snprintf(v, sizeof(v), "%d", S.bright); sendTxt("tBri", v);
    nexCmdf("pTemp.pic=%d", S.tempF ? PIC_TOG_F : PIC_TOG_C);
    nexCmdf("pPres.pic=%d", S.psi ? PIC_TOG_PSI : PIC_TOG_BAR);
    nexCmdf("pSpd.pic=%d", S.mph ? PIC_TOG_MPH : PIC_TOG_KMH);
  }
  for (int i = 0; i < N_PARAMS; i++)
    if (PARAMS[i].page == pg) { fmtParam(PARAMS[i], v, sizeof(v)); sendTxt(PARAMS[i].obj, v); }
  if (pg == 4) fillStatus();
}

void adjustByCode(int code) {
  int idx = code >= 0x30 ? 5 + (code - 0x30) / 2 : (code - 0x20) / 2;
  if (idx < 0 || idx >= N_PARAMS) return;
  Param &p = PARAMS[idx];
  float dir = (code & 1) ? 1 : -1;
  *p.v = constrain(roundf((*p.v + dir * p.step) / p.step) * p.step, p.lo, p.hi);
  // keep pairs sensible
  if (S.shiftOff > S.shiftOn - 50) S.shiftOff = S.shiftOn - 50;
  if (S.barStart > S.barFull - 1000) S.barStart = max(0.0f, S.barFull - 1000);
  if (S.battHigh < S.battLow + 0.5f) { if (p.v == &S.battLow) S.battLow = S.battHigh - 0.5f; else S.battHigh = S.battLow + 0.5f; }
  settingsDirty = true;
  fillPage(p.page);                             // refresh the page (linked values may have moved)
}

void onPress(int code) {
  if (code >= 0x20 && code <= 0x3F) {           // - / + buttons
    adjustByCode(code);
    heldCode = code; heldSince = lastRepeat = millis();
    return;
  }
  switch (code) {
    case 0x10: S.tempF = false; break;
    case 0x11: S.tempF = true;  break;
    case 0x12: S.psi = false;   break;
    case 0x13: S.psi = true;    break;
    case 0x14: S.mph = false;   break;
    case 0x15: S.mph = true;    break;
    case 0x40: PK = Peaks(); Serial.println("peaks reset"); break;
    default: return;                            // anything else (e.g. old 0x41 switch) is ignored
  }
  if (code <= 0x15) {                           // units changed: dash values follow
    updateUnits();
    bool tp = trackPeaks; trackPeaks = false;
    for (int i = 0; i < 6; i++) if (tileValid[i]) showTile(i, lastTile[i], lastWarn[i]);
    if (lastKmh >= 0) showSpeed(lastKmh);
    trackPeaks = tp;
    settingsDirty = true;
  }
  fillPage(curPage);
}

void handleHold() {                             // auto-repeat while - / + is held
  if (heldCode < 0) return;
  uint32_t now = millis(), held = now - heldSince;
  if (held > 15000) { heldCode = -1; return; }  // safety if a release got lost
  uint32_t every = held < 500 ? 0 : held < 2000 ? 120 : 40;
  if (every && now - lastRepeat >= every) { adjustByCode(heldCode); lastRepeat = now; }
}

void setBrightness(int v) {
  v = constrain(v, 10, 100);
  if (v == S.bright) return;
  S.bright = v;
  settingsDirty = true;
  nexCmdf("dim=%d", v);
  if (curPage == 1) { char b[6]; snprintf(b, sizeof(b), "%d", v); sendTxt("tBri", b); }
}

void onPageEnter(int pg) {
  int prev = curPage;
  curPage = pg;
  heldCode = -1;
  pageFilledAt = millis();
  Serial.printf("page %d\n", pg);
  if (pg == 0) {
    if (prev != 0) saveSettings();
    resyncDash();
  } else {
    fillPage(pg);
  }
}

// Nextion -> ESP32: 3 bytes '#' type code   (E page / P press / R release / B brightness)
void handleFrame(uint8_t type, uint8_t code) {
  switch (type) {
    case 'E': onPageEnter(code); break;
    case 'P': onPress(code); break;
    case 'R': if (code == heldCode) heldCode = -1; break;
    case 'B': setBrightness(code); break;
  }
}

// Frames we understand:
//   '#' type code              (3 bytes, from printh in the HMI)
//   0x66 page FF FF FF         (answer to "sendme": which page is shown)
//   0x71 b0 b1 b2 b3 FF FF FF  (answer to "get hBri.val")
void pollNextion() {
  static uint8_t buf[8], n = 0, need = 0;
  while (NEX.available()) {
    uint8_t b = NEX.read();
    if (dumpNex) Serial.printf("NEX %02X\n", b);
    if (n == 0) {
      need = b == '#' ? 3 : b == 0x66 ? 5 : b == 0x71 ? 8 : 0;
      if (!need) continue;                      // not a frame start (e.g. 0x88 "ready") - skip
    }
    buf[n++] = b;
    if (n < need) continue;
    n = 0;
    if (buf[0] == '#') handleFrame(buf[1], buf[2]);
    else if (buf[0] == 0x66) { if (buf[1] != curPage) onPageEnter(buf[1]); }
    else if (buf[0] == 0x71 && curPage == 1 && millis() - pageFilledAt > 400) setBrightness(buf[1]);
  }
}

// Safety net that does not depend on the HMI event code
void pollNextionState() {
  static uint32_t lastPage = 0, lastBri = 0;
  uint32_t now = millis();
  if (now - lastPage > PAGE_POLL_MS) { lastPage = now; nexCmd("sendme"); }
  if (curPage == 1 && now - lastBri > BRI_POLL_MS && now - pageFilledAt > 400) { lastBri = now; nexCmd("get hBri.val"); }
}

// =====================================================================
//  Speeduino over Bluetooth  (runs as its own task on core 0)
// =====================================================================
uint16_t u16(const uint8_t *d, int o) { return d[o] | (d[o + 1] << 8); }

// Wait until nothing has arrived for FLUSH_QUIET_MS - late bytes from the previous
// reply must not end up at the start of the next one (that misaligns every offset).
void flushQuiet() {
  uint32_t quiet = millis();
  while (millis() - quiet < (uint32_t)FLUSH_QUIET_MS) {
    if (SerialBT.available()) { SerialBT.read(); quiet = millis(); }
    else vTaskDelay(1);
  }
}

// Send 'A', read the first RESP_MIN bytes of the reply. False on timeout / short read.
bool requestPacket(uint8_t *out) {
  flushQuiet();
  SerialBT.write('A');
  uint32_t t0 = millis();
  int n = 0;
  while (millis() - t0 < (uint32_t)RESP_TIMEOUT_MS && n < RESP_MIN) {
    if (SerialBT.available()) out[n++] = SerialBT.read();
    else vTaskDelay(1);
  }
  if (dumpRaw) {
    Serial.printf("RX %d bytes:", n);
    for (int i = 0; i < n; i++) Serial.printf("%s%02X", i % 16 ? " " : "\n  ", out[i]);
    Serial.println();
  }
  return n >= RESP_MIN;
}

bool decode(const uint8_t *d, EcuData &e) {
  e.rpm  = u16(d, OFS_RPM);
  e.clt  = (int)d[OFS_CLT] - 40;
  e.iat  = (int)d[OFS_IAT] - 40;
  e.batt = d[OFS_BATT] / 10.0f;
  e.afr  = d[OFS_AFR] / 10.0f;
  e.kmh  = OFS_VSS >= 0 ? u16(d, OFS_VSS) : 0;
  e.gear = OFS_GEAR >= 0 ? d[OFS_GEAR] : 0;
  e.oilBar = OFS_OIL >= 0 ? (OIL_RAW_IS_PSI ? d[OFS_OIL] / 14.5038f : d[OFS_OIL] / 100.0f) : 0;
  // sanity check - garbage from a bad packet must not reach the screen
  return e.rpm < 20000 && e.clt > -40 && e.clt < 200 && e.batt < 25 && e.kmh < 400;
}

void ecuTask(void *) {
  SerialBT.begin(ESP_BT_NAME, true);                       // true = ESP32 is the master
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  SerialBT.setPin(ECU_BT_PIN, strlen(ECU_BT_PIN));
#else
  SerialBT.setPin(ECU_BT_PIN);
#endif
  static uint8_t data[160];
  uint32_t secStart = millis();
  uint16_t count = 0;

  for (;;) {
    if (!SerialBT.connected(0)) {
      linkState = LINK_SEARCHING;
      Serial.println("BT: connecting...");
      bool ok = BT_USE_ADDRESS ? SerialBT.connect(ECU_BT_ADDR) : SerialBT.connect(ECU_BT_NAME);
      if (!ok) { Serial.println("BT: not found, retry in 2 s"); vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
      Serial.println("BT: connected");
      linkState = LINK_CONNECTED;
    }
    uint32_t t = millis();
    EcuData e;
    if (requestPacket(data) && decode(data, e)) {
      e.stamp = millis();
      portENTER_CRITICAL(&ecuMux); ECU = e; portEXIT_CRITICAL(&ecuMux);
      count++;
    } else {
      pktErrors++;
    }
    if (millis() - secStart >= 1000) { pktPerSec = count; count = 0; secStart = millis(); }
    int32_t wait = POLL_MS - (int32_t)(millis() - t);
    vTaskDelay(pdMS_TO_TICKS(wait > 1 ? wait : 1));
  }
}

// =====================================================================
//  Dash update from live data
// =====================================================================
char gearFor(const EcuData &e) {
  if (e.gear > 0 && e.gear <= 6) return '0' + e.gear;
  if (!CALC_GEAR || e.kmh < 3 || e.rpm < 500) return 'N';
  float wheelRpm = e.kmh * 1000.0f / 60.0f / TYRE_CIRC_M;
  float ratio = e.rpm / wheelRpm / FINAL_DRIVE;
  int best = 0; float bestErr = 0.15f;                     // must be within 15 % of a gear
  for (int g = 1; g <= 6; g++) {
    float err = fabsf(ratio - RATIO[g]) / RATIO[g];
    if (err < bestErr) { bestErr = err; best = g; }
  }
  return best ? '0' + best : 'N';                          // clutch in / coasting -> N
}

float readFuelPct() {                          // called every TILE_MS
  static float f = -1;
  if (FUEL_PIN < 0) return -1;
  float v = analogRead(FUEL_PIN) / 4095.0f * 3.3f;
  v = constrain(v, 0.01f, 3.29f);
  float ohms = FUEL_DIVIDER_FIXED_OHMS * v / (3.3f - v);
  float pct = constrain((FUEL_EMPTY_OHMS - ohms) / (FUEL_EMPTY_OHMS - FUEL_FULL_OHMS) * 100.0f, 0.0f, 100.0f);
  f = f < 0 ? pct : f + (pct - f) * 0.02f;               // heavy smoothing, fuel sloshes
  return f;
}

void updateDash() {
  static uint32_t lastStamp = 0, lastSlow = 0;
  uint32_t now = millis();

  EcuData e;
  portENTER_CRITICAL(&ecuMux); e = ECU; portEXIT_CRITICAL(&ecuMux);
  bool fresh = e.stamp && now - e.stamp < STALE_MS;
  if (!fresh) {
    trackPeaks = false;
    if (!showingNoData) { showNoData(); showingNoData = true; }
    return;
  }
  trackPeaks = true;

  // RPM / bar / shift light / gear / speed: only when a NEW packet arrived
  if (e.stamp != lastStamp || showingNoData) {
    lastStamp = e.stamp;
    showingNoData = false;
    showRpm((int)e.rpm / 10 * 10);
    showGear(gearFor(e));
    if (OFS_VSS >= 0) showSpeed(e.kmh);
    else { lastKmh = -1; setTxt(cSpd, "tSpd", "--"); }
  }

  // tiles: slower
  if (now - lastSlow >= TILE_MS) {
    lastSlow = now;
    if (OFS_OIL >= 0) showTile(OIL, e.oilBar, e.oilBar < S.oilLow && e.rpm > S.oilCheckRpm); else tileDashes(OIL, C_WHITE);
    showTile(WAT, e.clt, e.clt > S.waterHot);
    showTile(BAT, e.batt, e.batt < S.battLow || e.batt > S.battHigh);
    showTile(AFR, e.afr, e.rpm > 2500 && e.afr > S.afrLean);   // lean on idle/overrun is normal
    float fuel = readFuelPct();
    if (fuel >= 0) showTile(FUE, fuel, fuel < S.fuelLow); else tileDashes(FUE, C_WHITE);
    showTile(AIR, e.iat, e.iat > S.airHot);
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  loadSettings();
  if (FUEL_PIN >= 0) analogSetPinAttenuation(FUEL_PIN, ADC_11db);

  // Big TX buffer: Nextion writes go to RAM and the UART sends them in the
  // background, so the loop never waits for the 115200-baud line.
  NEX.setTxBufferSize(2048);
  NEX.setRxBufferSize(512);
  NEX.begin(NEX_BAUD, SERIAL_8N1, NEX_RX, NEX_TX);
  delay(800);                                              // let the Nextion boot
  static const uint8_t FLUSH[3] = {0xFF, 0xFF, 0xFF};
  NEX.write(FLUSH, 3);                                     // flush any junk
  nexCmd("bkcmd=0");
  nexCmdf("dim=%d", S.bright);
  updateUnits();
  showNoData(); showingNoData = true;
  nexCmd("page 0");                                        // Nextion answers '#E0' -> resync

  xTaskCreatePinnedToCore(ecuTask, "ecu", 8192, nullptr, 2, nullptr, 0);
  Serial.println("\nAX Race Dash live. Keys: d = Speeduino dump, n = Nextion dump, s = decoded values");
}

void loop() {
  pollNextion();
  pollNextionState();
  handleHold();
  if (curPage == 0) updateDash();
  if (curPage == 4 && millis() - lastStatus > 1000) { lastStatus = millis(); fillStatus(); }

  while (Serial.available()) {
    char k = Serial.read();
    if (k == 'n') { dumpNex = !dumpNex; Serial.printf("Nextion RX dump %s\n", dumpNex ? "ON" : "OFF"); }
    if (k == 'd') { dumpRaw = !dumpRaw; Serial.printf("raw dump %s\n", dumpRaw ? "ON" : "OFF"); }
    if (k == 's') {
      EcuData e; portENTER_CRITICAL(&ecuMux); e = ECU; portEXIT_CRITICAL(&ecuMux);
      Serial.printf("rpm %.0f  clt %.0f  iat %.0f  batt %.1f  afr %.1f  oil %.2f bar  vss %.0f  gear %d  | %u Hz, %lu errors\n",
                    e.rpm, e.clt, e.iat, e.batt, e.afr, e.oilBar, e.kmh, e.gear, (unsigned)pktPerSec, (unsigned long)pktErrors);
    }
  }
  delay(1);
}
