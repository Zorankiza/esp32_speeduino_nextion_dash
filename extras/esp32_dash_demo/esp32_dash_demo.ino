// =====================================================================
//  AX Race Dash - DEMO + SETTINGS  (ESP32 -> Nextion NX8048T070)
//  No Speeduino needed yet. Tests every element of the HMI, and runs
//  the 4 settings pages (saved in ESP32 flash).
//
//  Wiring: ESP32 GPIO5  (TX)  -> Nextion RX (yellow)
//          ESP32 GPIO4  (RX)  <- Nextion TX (blue)
//          GND common, Nextion 5V from a supply that can give 1 A+
//
//  On the dash: HOLD the gear number ~1.5 s to open settings.
//
//  Serial Monitor (USB, 115200, "No line ending") - type a number:
//    0 = AUTO (all tests in a loop)   1 = boot sweep   2 = gears
//    3 = warnings   4 = shift light   5 = drive sim    6 = no-data
// =====================================================================

#include <Arduino.h>
#include <Preferences.h>

#define NEX Serial2
const int NEX_RX = 4, NEX_TX = 5;   // RX = pin wired to Nextion TX (blue), TX = pin wired to Nextion RX (yellow)
const uint32_t NEX_BAUD = 115200;

// ---- colours (RGB565, same as the design) ----
const uint16_t C_WHITE = 61342, C_AMBER = 62855, C_RED = 57929, C_GREEN = 16112, C_LABEL = 40213;

// ---- picture IDs (must match the import order in the Nextion Editor) ----
const int PIC_TOG_C = 27, PIC_TOG_F = 28, PIC_TOG_BAR = 29, PIC_TOG_PSI = 30;
const int PIC_TOG_KMH = 31, PIC_TOG_MPH = 32, PIC_DEMO_OFF = 33, PIC_DEMO_ON = 34;

// ---- gearbox, only used by the drive simulation ----
const float RATIO[7] = {0, 3.36, 2.07, 1.43, 1.13, 0.92, 0.78};
const float FINAL_DRIVE = 4.10;
const float TYRE_CIRC_M = 1.90;

// =====================================================================
//  Types (all structs ABOVE the first function - Arduino IDE rule)
// =====================================================================
struct Settings {                 // everything on the settings pages, stored in flash
  uint8_t bright = 80;
  bool tempF = false, psi = false, mph = false, demo = true;
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
  {"tOil", "hOil",  0,   7, 1},   // bar  (band range is always metric)
  {"tWat", "hWat", 40, 120, 0},   // degC
  {"tBat", "hBat", 10,  16, 1},   // V
  {"tAfr", "hAfr", 10,  18, 1},   // AFR
  {"tFue", "hFue",  0, 100, 0},   // %
  {"tAir", "hAir",  0,  80, 0},   // degC
};
enum { OIL, WAT, BAT, AFR, FUE, AIR };
const float NORMAL[6] = {4.2, 88, 13.9, 12.8, 62, 34};
const float ALARM[6]  = {0.6, 112, 11.6, 16.4, 8, 68};

// The last command for every dash field = what the dash SHOULD show.
// While a settings page is open nothing is sent; when the dash page
// comes back, everything is re-sent (resyncDash).
struct Cache { String cmd; };
Cache cRpm, cRpmCol, cBar, cGear, cSpd, cVal[6], cPos[6], cCol[6], cUOil, cUWat, cUAir, cUSpd;

struct Peaks { float rpm = 0, kmh = 0, water = 0, oil = 99; };
Peaks PK;
bool trackPeaks = false;
float curRpm = 0;
float lastTile[6], lastKmh = -1;   // last metric values, to re-draw after a unit change
bool lastWarn[6], tileValid[6];

int curPage = 0;           // page shown on the Nextion (it tells us on every page change)
bool shiftOn = false;
bool abortTest = false;
char pendingKey = 0;
int heldCode = -1;         // +/- button being held
uint32_t heldSince = 0, lastRepeat = 0, lastStatus = 0;

// =====================================================================
//  Nextion output
// =====================================================================
void nexCmd(const String &c) {
  NEX.print(c);
  NEX.write(0xFF); NEX.write(0xFF); NEX.write(0xFF);
}

void dashSend(Cache &c, const String &cmd) {
  if (c.cmd == cmd) return;
  c.cmd = cmd;
  if (curPage == 0) nexCmd(cmd);
}
void setTxt(Cache &c, const char *obj, const String &v) { dashSend(c, String(obj) + ".txt=\"" + v + "\""); }
void setNum(Cache &c, const String &prefix, int v)      { dashSend(c, prefix + String(v)); }

// =====================================================================
//  Units
// =====================================================================
String degUnit()  { return S.tempF ? String("\xB0") + "F" : String("\xB0") + "C"; }   // \xB0 = degree sign in Nextion's ISO-8859-1 font
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
  setTxt(cRpm, "tRpm", String(rpm));
  setNum(cRpmCol, "tRpm.pco=", rpm >= red ? C_RED : rpm >= amber ? C_AMBER : C_WHITE);
}

void showBar(int val) { setNum(cBar, "j0.val=", constrain(val, 0, 100)); }

void startShift(bool on) {
  shiftOn = on;
  if (curPage != 0) return;                    // resyncDash() restores it later
  if (on) { nexCmd("j0.val=100"); nexCmd("tmShift.en=1"); }
  else    { nexCmd("tmShift.en=0"); nexCmd("j0.ppic=2"); }
}

void showRpm(int rpm) {                        // number + bar + shift light
  curRpm = rpm;
  if (trackPeaks) PK.rpm = max(PK.rpm, (float)rpm);
  showRpmNumber(rpm);
  if (!shiftOn && rpm >= S.shiftOn) { cBar.cmd = "j0.val=100"; startShift(true); }
  else if (shiftOn && rpm < S.shiftOff) startShift(false);
  if (shiftOn) return;
  float span = max(S.barFull - S.barStart, 500.0f);
  int seg = constrain((int)lround((rpm - S.barStart) / span * 40), 0, 40);
  showBar(seg * 5 / 2);                        // whole segments only
}

void showGear(char g) {                        // 'R', 'N', '1'..'6'
  int pic = g == 'R' ? 4 : g == 'N' ? 5 : 5 + (g - '0');
  setNum(cGear, "pGear.pic=", pic);
}

void showSpeed(float kmh) {
  lastKmh = kmh;
  if (trackPeaks) PK.kmh = max(PK.kmh, kmh);
  setTxt(cSpd, "tSpd", String((int)(S.mph ? kmh * 0.621371f : kmh)));
}

// v is always metric; the text follows the unit settings, the marker uses the metric band
void showTile(int i, float v, bool warn) {
  const Tile &T = TILES[i];
  lastTile[i] = v; lastWarn[i] = warn; tileValid[i] = true;
  if (trackPeaks && i == WAT) PK.water = max(PK.water, v);
  if (trackPeaks && i == OIL && curRpm > S.oilCheckRpm) PK.oil = min(PK.oil, v);
  String txt;
  if (i == OIL && S.psi)               txt = String((int)lround(v * 14.5038f));
  else if ((i == WAT || i == AIR))     txt = String((int)lround(toTemp(v)));
  else                                 txt = String(v, (unsigned int)T.dec);
  setTxt(cVal[i], T.t, txt);
  int pos = constrain((int)lround((v - T.lo) / (T.hi - T.lo) * 100), 0, 100);
  setNum(cPos[i], String(T.h) + ".val=", pos);
  setNum(cCol[i], String(T.t) + ".pco=", warn ? C_RED : C_WHITE);
}

void showNormalTiles() { for (int i = 0; i < 6; i++) showTile(i, NORMAL[i], false); }

void resetAll() { startShift(false); }

// The dash page was (re)loaded: Nextion reset every field, so send it all again
void resyncDash() {
  nexCmd("tmShift.tim=" + String((int)S.flashMs));
  Cache *all[] = {&cRpm, &cRpmCol, &cBar, &cGear, &cSpd, &cUOil, &cUWat, &cUAir, &cUSpd};
  for (Cache *c : all) if (c->cmd.length()) nexCmd(c->cmd);
  for (int i = 0; i < 6; i++) {
    if (cVal[i].cmd.length()) nexCmd(cVal[i].cmd);
    if (cPos[i].cmd.length()) nexCmd(cPos[i].cmd);
    if (cCol[i].cmd.length()) nexCmd(cCol[i].cmd);
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

String fmtParam(const Param &p) {
  float v = *p.v;
  switch (p.kind) {
    case K_TEMP: return String((int)lround(toTemp(v))) + degUnit();
    case K_BAR:  return S.psi ? String((int)lround(v * 14.5038f)) + " psi" : String(v, 1) + " bar";
    case K_VOLT: return String(v, 1) + " V";
    case K_PCT:  return String((int)v) + " %";
    case K_AFR:  return String(v, 1);
    default:     return String((int)v);           // rpm, ms
  }
}

void fmtPeak(int k, String &out) {
  switch (k) {
    case 0: out = PK.rpm > 0 ? String((int)PK.rpm) : "--"; break;
    case 1: out = PK.kmh > 0 ? String((int)(S.mph ? PK.kmh * 0.621371f : PK.kmh)) + (S.mph ? " mph" : " km/h") : "--"; break;
    case 2: out = PK.water > 0 ? String((int)lround(toTemp(PK.water))) + degUnit() : "--"; break;
    case 3: out = PK.oil < 99 ? (S.psi ? String((int)lround(PK.oil * 14.5038f)) + " psi" : String(PK.oil, 1) + " bar") : "--"; break;
  }
}

void sendTxt(const char *obj, const String &v) { nexCmd(String(obj) + ".txt=\"" + v + "\""); }

void fillStatus() {                              // page 4 live part
  const char *pk[4] = {"tPk0", "tPk1", "tPk2", "tPk3"};
  String s;
  for (int k = 0; k < 4; k++) { fmtPeak(k, s); sendTxt(pk[k], s); }
  // TODO: replace with the real Bluetooth state when the Speeduino link is added
  if (S.demo) { sendTxt("tLink", "DEMO DATA");  nexCmd("tLink.pco=" + String(C_AMBER)); sendTxt("tHz", "simulated 25 Hz"); }
  else        { sendTxt("tLink", "NO LINK");    nexCmd("tLink.pco=" + String(C_RED));   sendTxt("tHz", "waiting for Speeduino"); }
}

void fillPage(int pg) {
  if (pg == 1) {
    nexCmd("hBri.val=" + String(S.bright));
    sendTxt("tBri", String(S.bright));
    nexCmd(String("pTemp.pic=") + (S.tempF ? PIC_TOG_F : PIC_TOG_C));
    nexCmd(String("pPres.pic=") + (S.psi ? PIC_TOG_PSI : PIC_TOG_BAR));
    nexCmd(String("pSpd.pic=") + (S.mph ? PIC_TOG_MPH : PIC_TOG_KMH));
  }
  for (int i = 0; i < N_PARAMS; i++)
    if (PARAMS[i].page == pg) sendTxt(PARAMS[i].obj, fmtParam(PARAMS[i]));
  if (pg == 4) {
    nexCmd(String("pDemo.pic=") + (S.demo ? PIC_DEMO_ON : PIC_DEMO_OFF));
    fillStatus();
  }
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
  fillPage(p.page);                              // refresh the whole page (linked values may have moved)
}

void onPress(int code) {
  if (code >= 0x20 && code <= 0x3F) {            // - / + buttons
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
    case 0x41: S.demo = !S.demo; abortTest = true; break;
    default: return;
  }
  if (code <= 0x15) {                            // units changed: dash texts follow
    updateUnits();
    bool tp = trackPeaks; trackPeaks = false;      // re-draw dash values in the new units
    for (int i = 0; i < 6; i++) if (tileValid[i]) showTile(i, lastTile[i], lastWarn[i]);
    if (lastKmh >= 0) showSpeed(lastKmh);
    trackPeaks = tp;
  }
  settingsDirty = true;
  fillPage(curPage);
}

void handleHold() {                              // auto-repeat while - / + is held
  if (heldCode < 0) return;
  uint32_t now = millis(), held = now - heldSince;
  if (held > 15000) { heldCode = -1; return; }   // safety if a release got lost
  uint32_t every = held < 500 ? 0 : held < 2000 ? 120 : 40;
  if (every && now - lastRepeat >= every) { adjustByCode(heldCode); lastRepeat = now; }
}

void onPageEnter(int pg) {
  int prev = curPage;
  curPage = pg;
  heldCode = -1;
  Serial.printf("page %d\n", pg);
  if (pg == 0) {
    if (prev != 0) saveSettings();
    resyncDash();
  } else {
    fillPage(pg);
  }
}

// Nextion -> ESP32 frames are 3 bytes: '#' type code
//   '#' 'E' page   page entered          '#' 'P' code   button pressed
//   '#' 'R' code   button released       '#' 'B' value  brightness released
void handleFrame(uint8_t type, uint8_t code) {
  switch (type) {
    case 'E': onPageEnter(code); break;
    case 'P': onPress(code); break;
    case 'R': if (code == heldCode) heldCode = -1; break;
    case 'B': S.bright = constrain(code, 10, 100); settingsDirty = true; break;
  }
}

void pollNextion() {
  static uint8_t buf[3], n = 0;
  while (NEX.available()) {
    uint8_t b = NEX.read();
    if (n == 0 && b != '#') continue;
    buf[n++] = b;
    if (n == 3) { n = 0; handleFrame(buf[1], buf[2]); }
  }
}

void serviceAll() {
  pollNextion();
  handleHold();
  if (curPage == 4 && millis() - lastStatus > 1000) { lastStatus = millis(); fillStatus(); }
  while (Serial.available()) {
    char k = Serial.read();
    if (k >= '0' && k <= '6') { pendingKey = k; abortTest = true; }
  }
}

// =====================================================================
//  Waiting: keeps the settings pages alive, pauses the test while
//  a settings page is open
// =====================================================================
bool waitMs(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms || curPage != 0) {
    serviceAll();
    if (abortTest) return false;
    if (curPage != 0) t0 = millis();
    delay(2);
  }
  return true;
}
#define WAIT(ms) if (!waitMs(ms)) return

// =====================================================================
//  TESTS
// =====================================================================
void testBootSweep() {
  Serial.println("[1] Boot sweep");
  resetAll();
  showGear('N');
  for (int dir = 0; dir < 2; dir++) {
    for (int s = 0; s <= 25; s++) {
      int p = dir == 0 ? s * 4 : 100 - s * 4;
      showBar(p);
      showRpmNumber((int)(p * S.barFull / 100));
      showSpeed(p * 2);
      for (int i = 0; i < 6; i++)
        showTile(i, TILES[i].lo + (TILES[i].hi - TILES[i].lo) * p / 100.0f, false);
      WAIT(30);
    }
    WAIT(250);
  }
  showNormalTiles();
  WAIT(800);
}

void testGears() {
  Serial.println("[2] Gears R N 1 2 3 4 5 6");
  resetAll();
  showNormalTiles();
  showRpm(1500);
  showSpeed(0);
  const char *g = "RN123456";
  for (int i = 0; g[i]; i++) {
    Serial.printf("    gear %c\n", g[i]);
    showGear(g[i]);
    WAIT(900);
  }
}

void testWarnings() {
  Serial.println("[3] Warnings");
  const char *names[6] = {"OIL low", "WATER hot", "BATT low", "AFR lean", "FUEL low", "AIR hot"};
  resetAll();
  showGear('3');
  showRpm(3500);
  showSpeed(80);
  showNormalTiles();
  WAIT(600);
  for (int i = 0; i < 6; i++) {
    Serial.printf("    %s\n", names[i]);
    for (int b = 0; b < 4; b++) {
      showTile(i, ALARM[i], b % 2 == 0);
      WAIT(300);
    }
    showTile(i, ALARM[i], true);
    WAIT(700);
    showTile(i, NORMAL[i], false);
    WAIT(300);
  }
}

void testShift() {
  Serial.printf("[4] Shift light  (on %d / off %d)\n", (int)S.shiftOn, (int)S.shiftOff);
  resetAll();
  showNormalTiles();
  showGear('2');
  int top = (int)S.shiftOn + 400;
  for (int r = 3000; r <= top; r += 100) { showRpm(r); showSpeed(r / 70); WAIT(40); }
  Serial.println("    on the limiter - bar should flash red/dark");
  for (int i = 0; i < 50; i++) { showRpm(top - 50 + (i % 2) * 100); WAIT(40); }
  Serial.println("    between OFF and ON - must KEEP flashing");
  showRpm((int)(S.shiftOn + S.shiftOff) / 2); WAIT(1200);
  Serial.println("    below OFF - flash must STOP");
  showRpm((int)S.shiftOff - 100); WAIT(800);
  for (int r = (int)S.shiftOff - 100; r >= 3000; r -= 150) { showRpm(r); showSpeed(r / 70); WAIT(40); }
  WAIT(500);
}

void testDrive() {
  Serial.println("[5] Drive simulation");
  resetAll();
  trackPeaks = true;
  float rpm = 1300, water = 84, fuel = 62, t = 0;
  int gear = 1;
  bool accel = true;
  float shiftHold = 0;
  uint32_t lastTiles = 0;
  const float dt = 0.04f;
  float upAt = min(S.shiftOn + 200, S.barFull);

  while (true) {
    t += dt;
    if (accel) {
      if (rpm >= upAt && gear < 6) {             // driver reacts to the shift light
        shiftHold += dt;
        if (shiftHold > 0.25f) {
          gear++;
          rpm *= RATIO[gear] / RATIO[gear - 1];
          shiftHold = 0;
          Serial.printf("    upshift -> %d\n", gear);
        }
      } else {
        rpm += 3400.0f / gear * dt;
      }
      if (gear == 6 && rpm >= upAt - 1200) { accel = false; Serial.println("    braking"); }
    } else {
      rpm -= 1100 * dt;
      if (rpm < 2800 && gear > 2) {
        gear--;
        rpm *= RATIO[gear] / RATIO[gear + 1];
        Serial.printf("    downshift -> %d\n", gear);
      }
      if (gear == 2 && rpm < 1800) break;
    }
    rpm = min(rpm, upAt + 100);

    float kmh = rpm / (RATIO[gear] * FINAL_DRIVE) * TYRE_CIRC_M * 60.0f / 1000.0f;
    showRpm((int)rpm / 10 * 10);
    showGear('0' + gear);
    showSpeed(kmh);

    if (millis() - lastTiles >= 200) {
      lastTiles = millis();
      water = min(water + 0.06f, 97.0f);
      fuel = max(fuel - 0.01f, 0.0f);
      float oil = 1.2f + rpm / 8000.0f * 4.5f;
      float batt = 14.1f - 0.15f * sinf(t * 3);
      float afr = accel ? 12.6f + 0.3f * sinf(t * 7) : 16.2f + 0.4f * sinf(t * 5);
      float air = 32 + kmh / 40.0f;
      showTile(OIL, oil, oil < S.oilLow && rpm > S.oilCheckRpm);
      showTile(WAT, water, water > S.waterHot);
      showTile(BAT, batt, batt < S.battLow || batt > S.battHigh);
      showTile(AFR, afr, accel && afr > S.afrLean);   // lean on overrun is normal
      showTile(FUE, fuel, fuel < S.fuelLow);
      showTile(AIR, air, air > S.airHot);
    }
    if (!waitMs(40)) { trackPeaks = false; return; }
  }
  trackPeaks = false;
  showRpm(900);
  showSpeed(0);
  showGear('N');
  WAIT(1500);
}

void testNoData() {
  Serial.println("[6] No data");
  resetAll();
  showBar(0);
  setTxt(cRpm, "tRpm", "----");
  setNum(cRpmCol, "tRpm.pco=", C_WHITE);
  setTxt(cSpd, "tSpd", "--");
  showGear('N');
  lastKmh = -1;
  for (int i = 0; i < 6; i++) {
    tileValid[i] = false;
    setTxt(cVal[i], TILES[i].t, "--");
    setNum(cPos[i], String(TILES[i].h) + ".val=", 0);
    setNum(cCol[i], String(TILES[i].t) + ".pco=", C_AMBER);
  }
  WAIT(3000);
}

// =====================================================================
int mode = 0;            // 0 = auto
int autoStep = 1;

void runTest(int n) {
  switch (n) {
    case 1: testBootSweep(); break;
    case 2: testGears();     break;
    case 3: testWarnings();  break;
    case 4: testShift();     break;
    case 5: testDrive();     break;
    case 6: testNoData();    break;
  }
}

void setup() {
  Serial.begin(115200);
  loadSettings();
  NEX.begin(NEX_BAUD, SERIAL_8N1, NEX_RX, NEX_TX);
  delay(800);                                          // let the Nextion boot
  NEX.write(0xFF); NEX.write(0xFF); NEX.write(0xFF);   // flush any junk
  nexCmd("bkcmd=0");
  nexCmd("dim=" + String(S.bright));
  updateUnits();
  nexCmd("page 0");                                    // known state; Nextion answers with '#E0' -> resync
  Serial.println("\nAX Race Dash demo. Keys: 0=auto 1=sweep 2=gears 3=warnings 4=shift 5=drive 6=no-data");
  Serial.println("Hold the gear number on the screen to open settings.");
}

void loop() {
  abortTest = false;
  if (!S.demo) {                                       // demo off and no Speeduino yet: show "--"
    runTest(6);
  } else if (mode == 0) {
    runTest(autoStep);
    if (!abortTest) autoStep = autoStep >= 6 ? 1 : autoStep + 1;
  } else {
    runTest(mode);
  }
  if (pendingKey) {
    mode = pendingKey - '0';
    autoStep = 1;
    pendingKey = 0;
    Serial.printf("-> mode %d\n", mode);
  }
}
