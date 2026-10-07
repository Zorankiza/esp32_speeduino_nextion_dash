# AX Race Dash: Nextion build sheet (NX8048T070, 800×480)

You can only make the `.HMI` file and the `.tft` upload file in Nextion Editor, so you build the project there.
Everything below is already measured. Plan for about 20–30 minutes.
Untested on hardware: check the font sizes on the real screen and adjust y by a few px if needed.

## 1. New project
Nextion Editor → New → Basic → **NX8048T070** → orientation **0° horizontal**. One page: `page0`.

## 2. Pictures (Tools → Picture → add in this order so the IDs match)
| ID | File | Use |
|---|---|---|
| 0 | 00_bg.png | page background |
| 1 | 01_rpm_off.png | j0 bpic |
| 2 | 02_rpm_on.png | j0 ppic |
| 3 | 03_rpm_shift.png | shift flash |
| 4–11 | 04_gear_R … 11_gear_6 | gear (R, N, 1–6) |
| 12 | 12_tick.png | slider cursor |
| 13–18 | 13_slider_bg_* | only for slider fallback (step 5) |

## 3. Fonts (Tools → Font Generator; install the TTFs from /fonts in Windows first)
| ID | TTF | Target | Notes |
|---|---|---|---|
| 0 | BarlowCondensed-Bold | height **72** | tile values. A digit should be about **42 px** tall |
| 1 | BarlowCondensed-Bold | height **84** | RPM + speed. A digit should be about **52 px** tall |

The font height must be ≤ the height of the component that uses it. If the digits look too small or too big, regenerate at a different height.
The design uses the `-` and `.` characters, so keep them in the font.

## 4. Page
`page0`: **sta = image, pic = 0**.
Preinitialize event:
```
tsw 255,0
```
(turns off touch for everything; this is a display only)

Program.s, before `page 0`:
```
baud=115200
bkcmd=0
```

## 5. Components (x, y, w, h exactly)
| Name (objname) | Type | x | y | w | h | Key attributes |
|---|---|---|---|---|---|---|
| j0 | Progress bar | 10 | 10 | 780 | 46 | sta=image, bpic=1, ppic=2, val=0 |
| tRpm | Text | 268 | 70 | 264 | 84 | sta=crop image, picc=0, font=1, pco=62855, xcen=1 (center), ycen=1, txt_maxl=5, txt="" |
| pGear | Picture | 300 | 156 | 200 | 220 | pic=5 (N) |
| tSpd | Text | 270 | 380 | 190 | 84 | crop, picc=0, font=1, pco=61342, xcen=2 (right), ycen=1, txt_maxl=3 |
| tOil | Text | 24 | 88 | 116 | 76 | crop, picc=0, font=0, pco=61342, xcen=2, ycen=1, txt_maxl=4 |
| tWat | Text | 24 | 226 | 116 | 76 | same |
| tBat | Text | 24 | 364 | 116 | 76 | same |
| tAfr | Text | 604 | 88 | 116 | 76 | same |
| tFue | Text | 604 | 226 | 116 | 76 | same |
| tAir | Text | 604 | 364 | 116 | 76 | same |
| hOil | Slider | 24 | 165 | 172 | 22 | sta=crop image, picc=0, pic2=12, wid=4, hig=18, minval=0, maxval=100 |
| hWat | Slider | 24 | 303 | 172 | 22 | same |
| hBat | Slider | 24 | 441 | 172 | 22 | same |
| hAfr | Slider | 604 | 165 | 172 | 22 | same |
| hFue | Slider | 604 | 303 | 172 | 22 | same |
| hAir | Slider | 604 | 441 | 172 | 22 | same |
| tmShift | Timer | – | – | – | – | tim=80, en=0 |

Slider fallback: if your editor version doesn't offer crop for the slider, use sta=image, pic=13…18 (the matching slice), pic2=12.

Leave every **Send Component ID** box unticked. Set **vscope = local**.

## 6. tmShift timer event
```
if(j0.ppic==3)
{
  j0.ppic=1
}else
{
  j0.ppic=3
}
```

## 7. Compile and upload
Compile → File → **TFT file output** → copy `.tft` to a FAT32 microSD (only file on card) → insert, power on, wait for "Update successed", power off, remove card.

## 8. Commands the ESP32 sends (each ends with 0xFF 0xFF 0xFF)
| What | Command |
|---|---|
| RPM bar | `j0.val=80` (0–100, quantize: `round(rpm/8500*40)*5/2`) |
| RPM number | `tRpm.txt="6850"` |
| RPM color | `tRpm.pco=61342` white / `62855` amber / `57929` red |
| Gear | `pGear.pic=7` (R=4, N=5, gear n = 5+n) |
| Speed | `tSpd.txt="112"` |
| Tile value | `tOil.txt="4.2"` |
| Tile marker | `hOil.val=62` (0–100 along the band) |
| Warning | `tWat.pco=57929` (back to normal: `61342`) |
| Shift flash on | `j0.val=100` then `tmShift.en=1` (flashes all-red ↔ dark) |
| Shift flash off | `tmShift.en=0` then `j0.ppic=2` |

Colors (RGB565): bg 2146 · panel 4292 · label 40213 · value 61342 · green 16112 · amber 62855 · red 57929 · blue 15452
