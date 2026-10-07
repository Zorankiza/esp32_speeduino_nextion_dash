# Settings pages: Nextion build sheet (add-on)

Do the dash-page changes first, then add 4 new pages: **page1 … page4**. Create them in that order, because `page 1` means the 2nd page.
Untested on hardware. Coordinates are exact to the images.

## A. New pictures and fonts

Replace picture **0** with the new `00_bg.png` (Picture tab → select 0 → *Replace*). Oil, water, air and speed units are now live text fields.
Then add **19 … 35** in file order:

| ID | File | | ID | File |
|---|---|---|---|---|
| 19 | 19_s1_bg | | 27 | 27_tog_C |
| 20 | 20_s1_bg_down | | 28 | 28_tog_F |
| 21 | 21_s2_bg | | 29 | 29_tog_bar |
| 22 | 22_s2_bg_down | | 30 | 30_tog_psi |
| 23 | 23_s3_bg | | 31 | 31_tog_kmh |
| 24 | 24_s3_bg_down | | 32 | 32_tog_mph |
| 25 | 25_s4_bg | | 33 | 33_tog_demo_off (unused) |
| 26 | 26_s4_bg_down | | 34 | 34_tog_demo_on (unused) |
| | | | 35 | 35_knob |

New fonts. Use **encoding iso-8859-1** so the ° sign exists:

| ID | TTF | Height | Used for |
|---|---|---|---|
| 2 | BarlowCondensed-SemiBold | 32 | dash unit labels, link status |
| 3 | BarlowCondensed-Bold | 48 | values on settings pages |

## B. Dash page (page0) changes

| Name | Type | x | y | w | h | Attributes |
|---|---|---|---|---|---|---|
| tUOil | Text | 144 | 120 | 62 | 36 | crop, picc=0, font=2, pco=40213, xcen=0, ycen=1, txt_maxl=5 |
| tUWat | Text | 144 | 258 | 62 | 36 | same |
| tUAir | Text | 724 | 396 | 62 | 36 | same |
| tUSpd | Text | 464 | 406 | 84 | 38 | same |
| mGear | Hotspot | 300 | 156 | 200 | 220 | Touch Press: `tmHold.en=1` · Touch Release: `tmHold.en=0` |
| tmHold | Timer | – | – | – | – | tim=1500, en=0 · Timer Event: `tmHold.en=0` then `page 1` |

page0 **Preinitialize Event** (replace):
```
tsw 255,0
tsw mGear,1
```
page0 **Postinitialize Event** (new):
```
printh 23 45 00
```
mGear must sit **on top of** pGear (create it after pGear) to receive the touch.

## C. Things every settings page has

- Page: **sta = image, pic = its bg** (19 / 21 / 23 / 25)
- **Postinitialize Event**: `printh 23 45 0N`, where N is the page number (01 … 04)
- **Nav buttons**: *Button* components, **sta = crop image, picc = page bg, picc2 = page bg_down, txt = ""**. picc2 makes them light up when pressed. Do **not** add a button on the page's own (yellow) tab.

| Name | x | y | w | h | Touch Release Event |
|---|---|---|---|---|---|
| bTab1 | 18 | 52 | 164 | 70 | `page 1` |
| bTab2 | 18 | 130 | 164 | 70 | `page 2` |
| bTab3 | 18 | 208 | 164 | 70 | `page 3` |
| bTab4 | 18 | 286 | 164 | 70 | `page 4` |
| bBack | 18 | 392 | 164 | 70 | `page 0` |

- **− / + buttons** are the same kind of crop Button, with two events:
  - Touch Press: `printh 23 50 XX`
  - Touch Release: `printh 23 52 XX`
  - XX is the code in the tables. The ESP32 changes the value, repeats while you hold, and saves when you go back to the dash.
- **Value texts**: Text, crop, picc = page bg, font = 3, pco = 61342, txt = "".

## D. page1: DISPLAY (pic 19, down 20)

| Name | Type | x | y | w | h | Attributes / events |
|---|---|---|---|---|---|---|
| hBri | Slider | 224 | 121 | 542 | 44 | sta=crop image, picc=19, pic2=35, wid=28, hig=44, minval=10, maxval=100 |
| tBri | Text | 676 | 70 | 70 | 48 | font 3, xcen=2 (right), txt_maxl=3 |
| pTemp | Picture | 526 | 226 | 240 | 56 | pic=27 |
| pPres | Picture | 526 | 294 | 240 | 56 | pic=29 |
| pSpd | Picture | 526 | 362 | 240 | 56 | pic=31 |
| mTempC | Hotspot | 526 | 226 | 120 | 56 | Touch Press: `printh 23 50 10` |
| mTempF | Hotspot | 646 | 226 | 120 | 56 | Touch Press: `printh 23 50 11` |
| mBar | Hotspot | 526 | 294 | 120 | 56 | Touch Press: `printh 23 50 12` |
| mPsi | Hotspot | 646 | 294 | 120 | 56 | Touch Press: `printh 23 50 13` |
| mKmh | Hotspot | 526 | 362 | 120 | 56 | Touch Press: `printh 23 50 14` |
| mMph | Hotspot | 646 | 362 | 120 | 56 | Touch Press: `printh 23 50 15` |

hBri **Touch Move Event** (brightness follows your finger, no ESP32 needed):
```
dim=hBri.val
covx hBri.val,tBri.txt,0,0
```
hBri **Touch Release Event** (also tells the ESP32 so it gets saved):
```
dim=hBri.val
covx hBri.val,tBri.txt,0,0
printh 23 42
prints hBri.val,1
```

## E. page2: SHIFT LIGHT (pic 21, down 22)

| Row | − button (x 536) | + button (x 704) | value text (x 604) | y | Setting |
|---|---|---|---|---|---|
| 0 | bM0 · code **20** | bP0 · code **21** | tP0 | 83 | Bar starts at |
| 1 | bM1 · code **22** | bP1 · code **23** | tP1 | 161 | Bar full at |
| 2 | bM2 · code **24** | bP2 · code **25** | tP2 | 239 | Shift light ON |
| 3 | bM3 · code **26** | bP3 · code **27** | tP3 | 317 | Shift light OFF |
| 4 | bM4 · code **28** | bP4 · code **29** | tP4 | 395 | Flash speed |

Sizes: buttons **w 64, h 56**. Values tP0…tP4 are **w 96, h 56**, xcen=1 (center), ycen=1.

## F. page3: WARNINGS (pic 23, down 24)

Buttons **w 56, h 48**. Values **w 129, h 48**, xcen=1.

| Cell | Setting | − button (x, y) · code | + button (x, y) · code | value text (x, y) |
|---|---|---|---|---|
| 0 | Water hot | bWM0 (228, 108) · **30** | bWP0 (421, 108) · **31** | tW0 (288, 108) |
| 1 | Air hot | bWM1 (513, 108) · **32** | bWP1 (706, 108) · **33** | tW1 (573, 108) |
| 2 | Oil low | bWM2 (228, 204) · **34** | bWP2 (421, 204) · **35** | tW2 (288, 204) |
| 3 | Oil check above (rpm) | bWM3 (513, 204) · **36** | bWP3 (706, 204) · **37** | tW3 (573, 204) |
| 4 | Batt low | bWM4 (228, 300) · **38** | bWP4 (421, 300) · **39** | tW4 (288, 300) |
| 5 | Batt high | bWM5 (513, 300) · **3A** | bWP5 (706, 300) · **3B** | tW5 (573, 300) |
| 6 | Fuel low | bWM6 (228, 396) · **3C** | bWP6 (421, 396) · **3D** | tW6 (288, 396) |
| 7 | AFR lean | bWM7 (513, 396) · **3E** | bWP7 (706, 396) · **3F** | tW7 (573, 396) |

## G. page4: PEAKS & STATUS (pic 25, down 26)

| Name | Type | x | y | w | h | Attributes / events |
|---|---|---|---|---|---|---|
| tPk0 | Text | 228 | 108 | 249 | 50 | max rpm · font 3, xcen=0, txt_maxl=12 |
| tPk1 | Text | 513 | 108 | 249 | 50 | top speed · font 3, xcen=0, txt_maxl=12 |
| tPk2 | Text | 228 | 204 | 249 | 50 | max water · font 3, xcen=0, txt_maxl=12 |
| tPk3 | Text | 513 | 204 | 249 | 50 | min oil · font 3, xcen=0, txt_maxl=12 |
| bReset | Button | 214 | 268 | 562 | 64 | crop 25 / 25-down 26 · Touch Release: `printh 23 50 40` |
| tLink | Text | 228 | 378 | 250 | 38 | font 2, xcen=0, txt_maxl=16 |
| tHz | Text | 228 | 416 | 250 | 36 | font 2, pco=40213, xcen=0, txt_maxl=24 |

The bottom-right box of page 4 (demo / WiFi update switch) is **not used** by the current firmware. Leave it empty; pictures 33/34 can stay in the list so the IDs don't shift.

## H. How it talks (for reference)

Nextion → ESP32 sends 3 bytes `23 type code`:

| type | Meaning |
|---|---|
| `45` E | page entered (code = page) |
| `50` P | button pressed (code) |
| `52` R | button released (code) |
| `42` B | brightness (code = value) |

The ESP32 stores everything in flash (`Preferences`). It refills each page when you open it, re-sends the whole dash when you come back, and saves when you leave settings.
