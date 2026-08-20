# hw-meshnology-w12 — Meshnology W12 board HAL

```
board bring-up (W12Board::onStart, before spangapInit)
 ↓
Vext gate LOW      → +3.3 V rail up: OLED, GNSS header
 ↓
GNSS EN LOW, RST LOW → 100 ms → RST HIGH   → receiver running
 ↓
LoRa CS HIGH       → LR2021 off the bus until iface-lora claims it
 ↓
spangapInit() → W12Board::onInit publishes sys.board
 ↓
iface-lora, on every begin:
   carrier > 1500 MHz?  no  → GPIO 4 HIGH, GPIO 3 LOW: GC1109 powered
    ↓ yes                     ceiling 30 dBm, gain 30 dB, chip -9..+22
   GPIO 3 HIGH, GPIO 4 LOW: RFX2402E powered
   ceiling 20 dBm, gain 22 dB, chip -19..+12
 ↓
   begin(), then the board's DIO map:
     IRQ on chip DIO8 (not RadioLib's DIO5 — DIO5 is not bonded out)
     RF switch on DIO9/10/11 (GC1109) and DIO5/6 (2.4 GHz)
 ↓
tx_power is ANTENNA dBm; the band's gain converts it to chip drive, so the
30 dBm ceiling reaches the GC1109 as the 0 dBm its 30 dB of gain needs
```

**hw-meshnology-w12** is the board-support straddle for the **Meshnology W12** —
an ESP32-S3R8 (16 MB flash, 8 MB **octal** PSRAM) carrying one Semtech
**LR2021** dual-band LoRa modem on its own SPI bus, behind a **GC1109** sub-GHz
front end that takes the board to **30 dBm at the antenna**. The board also
carries a 0.96" **SSD1315** OLED (an SSD1306 part), a GNSS header for a Quectel
**L76K**, an addressable RGB LED, a battery-sense divider and a solar/USB
charger. Board reference:
<https://meshnology.com/products/meshnology-w12-lr2021-ultra-long-range-lora-meshtastic-device-kit>.

It is a **non-buildable** component — it decides nothing about what the device
*does*. A buildable assembler (`reticulous/reticulous`) adds it and inherits the
board: `spangap build reticulous/reticulous --with spangap/hw-meshnology-w12`.
The mesh stack, the IP/web platform, `app_main`, the partition layout, the
update story and the browser SPA all come from the buildable and its other
straddles — not from here.

The board stages [tinylcd](../tinylcd) (its only screen is the mono OLED, so
the colour-TFT UI would have nothing to draw on) and [gps](../gps) (the kits
ship with the L76K populated; a bare board wants `--without spangap/gps`). The
BOOT/PRG button doubles as tinylcd's page button.

## ⚠️ Verify before trusting

The pin map below was assembled from the board's **Meshtastic** variant
(`variants/esp32s3/meshnology-w12`, which cites the board schematic
`W12-MB-V0.2` for the battery divider) cross-checked against **MeshCore**'s
(`variants/meshnology_w12`). The two published definitions agree on the radio,
the OLED and the GNSS header, and disagree where noted. The **RF front end and
the antenna connectors have since been confirmed against a rev `W12-MB-V0.2`
board**; the rest is still from the published variants. Confirm against your
actual unit before an RF or partition run:

- **The front-end path and the TX ceiling.** MeshCore programs the radio's DIO
  RF-switch table and caps chip drive to single digits, reaching the board's
  advertised 30 dBm through the GC1109's PA. Meshtastic programs no table and
  leaves the chip at its own +22 dBm. This straddle takes MeshCore's reading:
  it matches what the board is sold as, its author measured the PA's saturation
  point, and the masks transcribe the GC1109 datasheet's truth table exactly.
  If TX comes out around 0 dBm at the antenna instead of +30, the table is not
  reaching the part — with all three control lines low the GC1109 is in
  *shutdown*, not bypass. Check `lora` in the console for the DIO map, and see
  [INTERNALS.md](INTERNALS.md).
- **The 2.4 GHz antenna is a different connector.** The board (rev
  `W12-MB-V0.2`) carries **two U.FL connectors, silkscreened `2.4G` and
  `868/915`**, one per front end, with no antenna-selection switch between
  them — nothing in software moves the antenna. The case has a single SMA fed
  from one of them by a U.FL pigtail. So a 2.4 GHz build needs the pigtail
  moved to the `2.4G` connector and a 2.4 GHz antenna fitted; the kits ship a
  sub-GHz whip, band-marked on its collar.
- **Vext polarity.** Driven **active low**, per Meshtastic's schematic-derived
  variant; MeshCore's declaration reads the other way but, through its
  ref-counted pin wrapper, ends up driving the same pin the same direction. If
  the OLED stays dark, flip `BOARD_VEXT_ON_LEVEL` in `esp-idf/include/w12.h`.
- **Battery divider.** 390 k / 100 k (ratio 4.90) at 2.5 dB attenuation, from
  the schematic reference in Meshtastic's variant; MeshCore states 5.42 for the
  same divider. Trim `BAT_DIV_NUM/DEN` in `esp-idf/src/w12.cpp` if a multimeter
  disagrees.
- **GNSS presence.** Meshtastic's unit had the header unpopulated. A build with
  no receiver fitted simply reports no fix; nothing else changes.

## First flash

A W12 out of the box may enumerate as **`303A:0009` "ESP32-S3"** rather than the
`303A:1001` "USB JTAG/serial debug unit" every other board here presents, and
`spangap flash` will not drive it in that state. Nothing is wrong with the chip:
0x0009 is the S3's **USB-OTG** identity (esptool's `uses_usb_otg()` is the PID
equalling the chip id), and both controllers sit behind the one PHY on
GPIO 19/20.

Which controller is on the wire is decided in software, not by this board. The
eFuses are untouched on a stock unit — `USB_PHY_SEL`, `DIS_USB_SERIAL_JTAG` and
the rest all read `0b0` — so the default routes the internal PHY to
USB-Serial-JTAG, and anything else is a runtime re-point through the RTC-domain
USB config register. That re-point is ordinary: it is how any S3 runs TinyUSB
without burning a fuse, and it is what this platform's own `usb cdc` verb does.
The factory firmware makes it, advertising a "Type-C CDC serial port" in the
vendor's manual.

**So the flash is the fix, not a power cycle.** A reset reverts the routing, but
an application that re-points the PHY every boot will keep doing so; replacing
that application is what settles it. This firmware never re-points at boot — the
console comes up on `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` and moves only when
someone types `usb cdc` — so one flash is enough and the board is an ordinary
`303A:1001` device afterwards.

esptool talks to the ROM over USB-OTG perfectly well; what it cannot do there is
reset the chip into the bootloader by itself, which is the whole of the problem.
Enter download mode by hand (hold PRG, tap RST, release PRG) and run esptool
with `--before no_reset` against the `<project>.esptool` argfile in the build's
`flasher.zip`. After that the ordinary `spangap monitor` / `spangap flash` loop
works.

## Origins

The W12 is a Heltec WiFi LoRa 32 V3/V4-style layout with the SX1262 replaced by
an LR2021 and a 30 dBm front end added. The LoRa header, the OLED pins and the
BOOT button are all where a Heltec V4 puts them, which is exactly why
`detect_hw` identifies this board by its **modem** rather than by its panel —
see below.

## What it does, and how it fits

The board contributes two Service objects that the buildable's generated init
dispatcher constructs and walks. There is nothing to call by hand: if the
straddle is in the build, the board comes up automatically.

| Service | Band | Brings up |
|---|---|---|
| `W12Board::onStart` | start | Vext rail, GNSS enable/reset, LoRa CS park |
| `W12Board::onInit` | init | publishes `sys.board` |
| `W12Battery::onInit` | init | ADC + the once-a-minute battery sampler |

`onStart` runs **before** `spangapInit()`. It is bare-hardware bring-up: the
OLED and the GNSS receiver are both behind the Vext rail and both have a
consumer that starts early, and the LR2021's CS must be parked HIGH before the
deselected radio can drive MISO. The two front-end supply gates are deliberately
**not** here — only the radio knows which band it is about to program, so
iface-lora owns them (see below).

The LoRa radio engine, the OLED UI, the GNSS parser, the IP/web platform and the
mesh stack are owned by other straddles ([iface-lora](../iface-lora),
[tinylcd](../tinylcd), [gps](../gps), [spangap-core](../spangap-core),
[spangap-net](../spangap-net), [rns](../rns)); this board supplies their pins
and the power/CS glue.

## Board identity (`detect_hw`)

`esp-idf/src/detect.cpp` answers one question about this board: it returns
`"hw-meshnology-w12"` when the hardware under the firmware is this board, and
NULL when it is not. What it asks:

16 MB flash, then the SSD1315 OLED acking on 17/18 (at 0x3C or 0x3D, whichever
the strap picked) with Vext powered and the panel pulsed out of reset, confirmed
by an **LR2021** on the LoRa header. Vext and the OLED reset are released
**only** when the probe fails.

The modem is the discriminator on purpose. A **Heltec WiFi LoRa 32 V4** has the
same 16 MB flash, the same OLED on the same pins with the same reset, and the
same LoRa header — the part on the end of it is the only difference, and both
boards' probes name theirs.

spangap-core calls it before the first `onStart()` — the last moment no bus is
claimed — and **halts the device awake** when the answer disagrees with the
board this image was built for. The confirmed answer is published as `sys.hw`
and announced on the console as `build: hw hw-meshnology-w12`. flashmon's
standalone detector carries a hand-kept copy of the same function, renamed
`detect_hw_meshnology_w12`; change one, change the other. See
[spangap-core/docs/init.md](../spangap-core/docs/init.md) and
[flashmon/docs/detect.md](../flashmon/docs/detect.md).

## Hardware & pin map

### LoRa LR2021 (owned by iface-lora, pins published here)

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| NSS / CS | 8 | | RST | 12 |
| SCK | 9 | | BUSY | 13 |
| MOSI | 10 | | IRQ | 14 |
| MISO | 11 | | | |

One radio (`CONFIG_LORA_COUNT=1`, `CONFIG_LORA0_RADIO_LR2021=y`) on **SPI host
2**, its own bus. **No TCXO** — a plain crystal on XTA/XTB, so
`CONFIG_LORA0_TCXO_MV=0`.

GPIO 14 is wired to the chip's **DIO8**, not the DIO5 RadioLib assumes
(`CONFIG_LORA0_LR_IRQ_DIO=8`). DIO5 is not bonded out on this board: left at the
default, every interrupt is pointed at a pin that is not there, and the radio
comes up, calls itself healthy and never reports a frame.

### LoRa front end — 30 dBm

A **GC1109** PA/LNA sits between the LR2021's sub-GHz port and the antenna, and
an **RFX2402E** does the same on the 2.4 GHz port.

The GC1109 is specified **860–930 MHz** with an integrated output match, so it
is a whole-sub-GHz-ISM part rather than a 915 one: **EU868 (863–870 MHz) is
in-band**, near the lower edge but inside it, and needs no different component.
Its datasheet figures are +30 dBm saturated, 30 dB small-signal gain, 17 dB
receive gain and 2 dB noise figure, drawing 600 mA at full transmit.

The board says the same thing twice over. Its sub-GHz U.FL connector is
silkscreened **`868/915`** — the vendor's own marking for a single dual-band
port — and the RF chain behind it, inspected on a rev `W12-MB-V0.2` unit, is
**U.FL → an L/C match in 0402 discretes → the 16-pin 3×3 mm QFN → the LR2021**,
with **no SAW filter anywhere in it**. The GC1109's optional `RX_FLT` filter is
not populated, so there is nothing band-specific in either direction: a unit
sold as 915 receives and transmits at 868 with no modification but the antenna.
The retail box's band tick-box (`☐2.4G ☑915M ☐868M`) records which antenna and
firmware default were packed, not a different board. Neither is switched from an
MCU GPIO: the **radio drives them from its own DIOs** as it changes mode, which
the board declares as five per-mode masks over DIO5..DIO11
(`CONFIG_LORA0_LR_RFSW_*`, bit 0 = DIO5):

| Chip DIO | Line | High in |
|---|---|---|
| DIO5 | RFX2402E 2G4_TX_EN | 2.4 GHz transmit |
| DIO6 | RFX2402E 2G4_RX_EN | 2.4 GHz receive |
| DIO9 | GC1109 CTX | sub-GHz transmit |
| DIO10 | GC1109 CPS (low = bypass) | sub-GHz transmit |
| DIO11 | GC1109 CSD (low = shutdown) | sub-GHz receive and transmit |

The two front ends' **supplies** are MCU pins, published to iface-lora rather
than driven here: GPIO 4 (sub-GHz, `CONFIG_LORA0_FEM_PWR_PIN`) and GPIO 3
(2.4 GHz, `CONFIG_LORA0_FEM_HF_PWR_PIN`). The driver raises the one the
configured carrier needs and drops the other on every begin, so the amplifier
for the band nobody is using draws nothing. Both nets are pulled up on the
board, so a build without iface-lora leaves both powered and harmless.

### Transmit power, per band

`s.lora.0.tx_power` is **antenna dBm**, and everything about it changes at
1500 MHz, because the two paths share nothing:

| | sub-GHz | 2.4 GHz |
|---|---|---|
| front end | GC1109 | RFX2402E |
| declared gain | 30 dB | 22 dB |
| antenna ceiling | 30 dBm | 20 dBm |
| chip drive at that ceiling | 0 dBm | −2 dBm |
| what the chip itself accepts | −9 … +22 dBm | −19 … +12 dBm |

iface-lora converts antenna dBm to chip drive through the band's gain and
clamps to the band's chip range, so a request for 30 dBm reaches the GC1109 as
0 dBm of drive — the LR2021 would otherwise happily deliver +22 into it. Tuning
across the boundary re-clamps `tx_power` to the new ceiling with a warning and
republishes `lora.0.tx_power_max`, so the power slider re-sizes itself.

The sub-GHz figures are the GC1109's datasheet values (small-signal gain 30 dB,
saturated output +30 dBm, so +30 dBm out from 0 dBm in). Applied flat they are
exact through the linear region and a dB or two shy at the very top, since a
real PA compresses — MeshCore's measurement on this board puts full saturation
nearer +3 dBm of drive. That is the right way to be wrong: the alternative
overstates every rung below the ceiling.

Two things to mind at full power. **Region limits** — 30 dBm is far past e.g.
EU868's 14 dBm ERP. And **harmonics**: the GC1109 specifies its second harmonic
at −11 dBm at saturation, which is far above the −36 dBm most spurious-emission
limits allow, so a board-level filter (or a lower power setting) is what makes
+30 dBm legal anywhere.

### Board-owned pins (in this straddle's `w12.h`)

| Signal | GPIO | Notes |
|---|---|---|
| Vext peripheral power EN | 45 | **active-low** — drive LOW to enable the +3.3 V rail (OLED, GNSS). Also the VDD_SPI strapping pin |
| GNSS supply gate | 48 | **active-low** — LOW = receiver powered |
| GNSS reset | 42 | **active-low** — released HIGH after the rail settles |
| Battery sense | 1 | ADC1_CH0, behind the divider switch |
| Battery divider switch | 2 | **active-high** — closed only for the length of a reading |
| RGB LED | 46 | present, unwired — the platform has no engine for it |

### OLED + page button (owned by tinylcd, pins published here)

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| OLED SDA | 17 | | OLED RST | 21 |
| OLED SCL | 18 | | page button (BOOT/PRG) | 0 |

0.96" SSD1315 128×64 — an SSD1306 part, so the `CONFIG_TINYLCD_SSD1306` default
applies — at address 0x3C, powered off Vext. The glass is **two-tone**: the top
16 rows are yellow and the rest blue, with a physical gap along the seam. That
needs no configuration, because tinylcd's page layout already keeps titles above
row 16 and body lines below it (`TINYLCD_TITLE_Y` / `TINYLCD_BODY_Y`) — the seam
falls in the space between them and reads as a title bar. It is only worth
knowing when writing a new page: a line drawn across row 16 is sliced. A short click on BOOT/PRG steps to
the next status page, a hold past 500 ms turns the screen off, and a press with
the screen off wakes it; idle policy under `s.tinylcd.standby`
([tinylcd](../tinylcd)). The page dots default to "top unless flipped"
(`CONFIG_TINYLCD_PAGE_INDICATOR=2`): the PRG button sits above the screen and
the dots track it through a flip.

### GNSS (owned by gps, pins published here)

| Signal | GPIO | Notes |
|---|---|---|
| host RX ← receiver TX | 38 | `CONFIG_GPS_RX_PIN` |
| host TX → receiver RX | 39 | `CONFIG_GPS_TX_PIN` |
| FORCE_ON / wake | 40 | `CONFIG_GPS_FORCE_PIN` |
| supply gate | 48 | board-driven (above) |
| reset | 42 | board-driven (above) |

Quectel L76K, NMEA over UART1 8N1. The PPS output (GPIO 41) is unwired — the
platform has no consumer for it.

### Memory / flash (published from `kconfig:`)

A non-buildable straddle has no `sdkconfig.defaults` of its own — it would be
ignored under `--with` — so every value that describes this hardware is
published from `straddle.yaml`'s `kconfig:` block and consumed by the owning
straddle / IDF:

| Key | Value | Why |
|---|---|---|
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | `y` | 16 MB flash |
| `CONFIG_SPANGAP_MAX_FIRMWARE_KB` | `6144` | state floor at 6 MB: `app`+`fixed` (~2.8 MB) plus growth headroom sit below it, and the runtime `/state` partition fills the remaining ~10 MB. Without it the floor defaults to the whole container and `app` eats all 16 MB — leaving **no `/state`** |
| `CONFIG_SPIRAM_MODE_OCT` | `y` | the S3R8 carries 8 MB PSRAM in **octal** mode |

Changing `CONFIG_SPANGAP_MAX_FIRMWARE_KB` relocates `/state`, so a device
flashed across the change factory-resets on its next boot. Warn users first.

## Storage variables

This board owns no settings. It publishes three read-only runtime keys:

| Key | Kind | Meaning |
|---|---|---|
| `sys.board` | runtime | `"Meshnology W12"`, published once at init |
| `battery.millivolt` | runtime | VBAT in mV, refreshed every minute |
| `battery.percent` | runtime | 0..100 from the cell's open-circuit-voltage curve |

Runtime LoRa parameters live at `s.lora.*` ([iface-lora](../iface-lora)), the
display's at `s.tinylcd.*` ([tinylcd](../tinylcd)) and the receiver's at
`s.gps.*` ([gps](../gps)).

## Dependencies

- [spangap-core](../spangap-core) — base runtime (storage, log, CLI, fs, ITS).
- [iface-lora](../iface-lora) — owns the LR2021 radio engine; this board parks
  its CS, powers its front end and supplies its pins and DIO map via Kconfig.
- [tinylcd](../tinylcd) — staged by this board (`additional_installs`); owns the
  OLED paged UI and the page button, pins supplied via Kconfig.
- [gps](../gps) — staged by this board (`additional_installs`); owns the NMEA
  receiver, UART pins supplied via Kconfig while this board drives its rails.

## Read next

- [INTERNALS.md](INTERNALS.md) — the bring-up ordering rules, the LR2021 facts
  that are not obvious from RadioLib, and the board pitfalls.
