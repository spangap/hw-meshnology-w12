# hw-meshnology-w12 internals

```
serviceRunStart()                              [main task, before spangapInit]
 ↓
detect_hw()          16 MB flash? → no  → NULL, device halts awake
 ↓ yes               OLED acks 0x3C/0x3D on 17/18 with Vext up + RST pulsed?
 ↓ yes               LR2021 answers on the LoRa header?   → no → NULL
 ↓ yes → "hw-meshnology-w12", published as sys.hw
W12Board::onStart()  Vext LOW · GNSS EN LOW · GNSS RST LOW → 100 ms → HIGH
                     · LoRa CS HIGH
 ↓
spangapInit()                                  [storage, fs, log, cli up]
 ↓
serviceRunInit()
   W12Board::onInit    → sys.board
   W12Battery::onInit  → ADC + 60 s sampler (two-shot: arm, then read)
   tinylcd, gps, iface-lora … at their own init_order positions
 ↓
iface-lora radioBegin()                        [radio task, per (re)configure]
   femBandSelect(freq > 1500 MHz)   GPIO 4/3 supply gates, ceiling, drive range
   irqDioNum = 8   BEFORE begin()   (begin is what programs the IRQ DIO)
   begin()
   lr2021ApplyDio() AFTER begin()   (begin resets every DIO to "nothing")
   setRxBoostedGainMode(level)      (only accepted from standby)
```

Three rules the ladder depends on:

- **`onStart` is before `spangapInit`.** The OLED and the GNSS receiver are both
  behind Vext and both have a consumer that starts early. A rail brought up in
  `onInit` is a panel that initialises against no VCC.
- **Every LR2021 DIO fact is re-applied on every `begin()`.** `begin()` resets
  the part, and a reset returns DIO5..DIO11 to "no function". The IRQ DIO is the
  exception that must be set *before* the call, because `begin()` is what
  programs it.
- **`detect_hw` identifies this board by its modem.** Flash size, panel and LoRa
  header are all shared with the Heltec V4.

## 1. Inventory — what this straddle adds

Relative to a build with no board straddle:

| Added | Where |
|---|---|
| `W12Board` (Service) — Vext, GNSS enable/reset, LoRa CS park; `sys.board` | `esp-idf/src/w12.cpp` |
| `W12Battery` (Service) — ADC bring-up, two-shot 60 s sampler, `battery.millivolt` / `battery.percent` | `esp-idf/src/w12.cpp` |
| `detect_hw()` — this board's self-assertion | `esp-idf/src/detect.cpp` |
| Board pin constants | `esp-idf/include/w12.h` |
| Kconfig VALUES for iface-lora, tinylcd, gps and IDF | `straddle.yaml` `kconfig:` |
| A `Hardware` section on the System settings page | `straddle.yaml` `settings:` |
| `spangap/tinylcd` + `spangap/gps` staged | `straddle.yaml` `additional_installs:` |

Nothing else in the image references this straddle. `detect_hw` is reached
through spangap-core's weak declaration and its `-u detect_hw` link line; the
two Services through the generated boot registry.

## 2. LR2021 facts this board is the first to need

The W12 is the first board in the workspace with an LR2021, so several things
that were latent in [iface-lora](../iface-lora) are exercised here for the first
time. They are that straddle's code, but this is the board that makes them
matter.

**The IRQ comes out of a DIO the board chooses.** The chip bonds out
DIO5..DIO11 and any of them can be the interrupt. RadioLib assumes DIO5; this
board wired DIO8 and does not bond DIO5 out at all. Left at the default the
radio initialises cleanly, reports itself found, transmits — and never signals a
reception, because the interrupt is aimed at a pad that is not connected.
`CONFIG_LORA0_LR_IRQ_DIO=8` is what avoids that, and it is applied before
`begin()` because `begin()` is what programs the DIO's function.

**The front end is switched by the radio, not by the MCU.** iface-lora's
`lora_fem.cpp` handles a FEM on MCU GPIOs — rail, enable, direction — sensed at
boot. Nothing here is on an MCU pin except the two supply gates, so there is
nothing to sense: the board *declares* its front end
(`CONFIG_LORA0_FEM_GAIN_DB`) and hands the driver a DIO map
(`CONFIG_LORA0_LR_RFSW_*`), and the chip applies the row itself on every mode
change. That is what keeps every `standby()`/`startReceive()`/`startTransmit()`
call site in the driver ignorant of the front end.

**The DIO map is programmed with raw commands, not with RadioLib's helper.**
`LR2021::setRfSwitchTable` takes a pin array of `Module::RFSWITCH_MAX_PINS`,
which is **5** — and this board uses five of the chip's seven DIOs, with no room
for a board that uses six. RadioLib 7.7.1 also indexes its per-DIO
configuration by the caller's array position rather than by the DIO number, so
any DIO past the fifth is programmed with a zero mask; the fix is on the
library's master branch but is in no release. iface-lora therefore issues
`SetDioFunction` (0x0112) and `SetDioRfSwitchConfig` (0x0113) itself over
`Module::SPIwriteStream`, which is exactly what the library does underneath.
One constraint survives from the datasheet either way: **DIO5 accepts only the
pull-up in sleep**, and a different pull makes the chip refuse the command.

**Boosted RX gain is a level, not a flag.** `SX126x::setRxBoostedGainMode` takes
a bool; the LR2021's takes 0..7, and only from standby. `s.lora.<i>.rx_boosted_gain`
stays one switch to the operator, and "on" picks the top of the range — the
setting exists to buy sensitivity, so a middle rung would be a number nobody
asked for. `radioBegin` applies it while the chip is still in the standby
`begin()` left it in.

**A frequency change must go through a full `begin()`.** Crossing the part's
LF/HF boundary re-points the whole front end, and the incremental setters answer
a live cross-band move with an error rather than a retune. iface-lora's
`applyConfig` already stops and restarts the radio on any config change, so this
costs nothing here — but it is why the incremental path must not be "optimised"
into a `setFrequency` for this family.

**No TCXO.** A plain crystal on XTA/XTB, so `CONFIG_LORA0_TCXO_MV=0`.
`radioBegin`'s TCXO-off retry (which turns "radio absent" into a working radio
and a warning) never arms on this board, because there is no TCXO voltage to
fall back from. A `begin()` that fails here fails for a different reason.

## 3. Two bands, two of everything

The LR2021 has separate sub-GHz and 2.4 GHz ports, and this board puts a
different amplifier on each: a GC1109 and an RFX2402E. Nothing is shared but the
number the operator types, so **every** term in the power path is the band's,
and `femBandSelect` settles all of them from the carrier at the top of each
`radioBegin`:

| | sub-GHz | 2.4 GHz |
|---|---|---|
| supply gate | GPIO 4 high, GPIO 3 low | GPIO 3 high, GPIO 4 low |
| chip RF-switch modes | DIO9/10/11 | DIO5/6 |
| declared gain | 30 dB | 22 dB |
| antenna ceiling | 30 dBm | 20 dBm |
| chip drive range | −9 … +22 dBm | −19 … +12 dBm |

The chip's drive range is the part that bites: a value perfectly legal on one
port is refused outright on the other, so the conversion has to know the band
before it clamps. That is why `femBandSelect` runs before `rfChipDbm` rather
than beside it, and why `radioStart` calls it too — the ceiling it publishes is
what the operator's `tx_power` is measured against, and that check happens
before `begin()`.

**Why the numbers are these numbers.** The sub-GHz pair is the GC1109
datasheet's own: 860–930 MHz, small-signal gain **30 dB**, saturated output
**+30 dBm** — which is the same statement twice, since 30 dB of gain reaches
+30 dBm from 0 dBm of drive. The pair is a guard as much as a conversion, and
the two numbers have to move together: raising the ceiling without raising the
gain raises the chip drive by the same amount, into a part the LR2021 can
comfortably overdrive. On the 2.4 GHz side the RFX2402E saturates around
+20 dBm out, and 22 dB against a 20 dBm ceiling asks for −2 dBm — inside the
chip's narrow HF range with room to spare.

Both gains are applied **flat**, which is exact through each part's linear
region and shy at the top, where a real PA compresses: MeshCore's measurement on
this board puts the GC1109's full saturation nearer +3 dBm of drive than 0. Err
low, always — the alternative overstates every rung below the ceiling and
overdrives the part to reach the last one. The other end of the range is the
board's floor: the chip's own floor plus the gain, about **21 dBm** at the
antenna sub-GHz and **3 dBm** at 2.4 GHz.

The amplifier is what puts it there, and this board steps around it:
`LORA0_LR_RFSW_TX_BYPASS=0x50` is the transmit row with CPS (DIO10) dropped, so
the signal takes the GC1109's bypass path instead of its PA and reaches about
**−10 … +20 dBm**. The driver picks the path per frame from the power it was
asked for — there is no setting — so the board spans −10 … +30 and a peer in
the same room gets microwatts while one across the valley gets the full PA.
`declared-bypass` in `LORA0_TX_CAL` is what the bypass state radiates; the
mechanism is iface-lora's, described in its INTERNALS §4b.

Whichever floor is in force, it is published as `lora.<n>.tx_power_min` rather
than left to be discovered: a `tx_power` under the board's real range is clamped
up with a warning, the adaptive controller never asks below it, and what a frame
announces is what it actually radiated — on either side of the front end. A node
that settled below its floor would otherwise tell every neighbour a power it was
not using, and each of them would compute its path loss wrong by the difference.

The 2.4 GHz path has no bypass mask, so there the +3 dBm floor stands.

Every figure here is **declared, not measured** — grade `none` in
`lora.<n>.cal`. The amplified ends are the chip's range plus a datasheet gain
and the bypass ends are it less a datasheet insertion loss, which is the part of
the curve a flat model fits best, but nobody has put this board on an analyser
at either end.

The **RF-switch masks are the datasheet's truth table**, transcribed: shutdown
is CSD 0; receive is CSD 1, CTX 0; transmit-bypass is CSD 1, CTX 1, CPS 0; and
full transmit is all three high. So RX drives DIO11 alone (0x40) and TX drives
DIO9/10/11 together (0x70). Bypass is a mode this board never selects — it
would cost the PA and give back 1 dB of insertion loss.

**The antenna side is two connectors.** The board carries one U.FL per front
end, silkscreened `2.4G` and `868/915`, with no antenna-selection switch between
them: the case's single SMA is fed from whichever one the pigtail is plugged
into, and nothing in software moves it. Changing band across 1500 MHz therefore
switches everything the firmware owns and stops at the connector — the pigtail
is a manual step. The sub-GHz port's own silkscreen naming **both** sub-GHz
bands is also the vendor stating that 868 and 915 are one configuration of one
board.

## 4. Pitfalls

- **`CONFIG_SPANGAP_MAX_FIRMWARE_KB` relocates `/state`.** Changing 6144
  moves the runtime partition, and a device flashed across the change
  factory-resets on its next boot. Removing it is worse: the floor then defaults
  to the whole flash container, `app` expands to fill all 16 MB, and the image
  has no `/state` at all.
- **GPIO 45 is the VDD_SPI strapping pin.** It is sampled at reset and weakly
  pulled down, so the Vext rail is already on before any code runs. Driving it
  in `onStart` keeps it on; it does not turn it on. Nothing may drive it high
  during a reset.
- **Both PA supply gates are pulled up on the board**, and neither is driven by
  this straddle. They are published to iface-lora
  (`CONFIG_LORA0_FEM_PWR_PIN` / `_FEM_HF_PWR_PIN`) because only the radio knows
  which band it is about to program. A build without iface-lora therefore leaves
  both amplifiers powered — harmless, and the only sensible default for a board
  with no radio driver in it.
- **The battery divider reads a hard zero when its switch is open.** GPIO 2
  gates a high-side switch through an inverting NPN, so it is active high and
  undriven means disconnected. `batteryArm` closes it and `batterySample` opens
  it again a settle later — two timer shots rather than one callback with a
  sleep in it, because the esp_timer task is shared and a 10 ms stall in a
  callback holds every other timer in the system.
- **2.5 dB attenuation, not 12.** A full 4.2 V cell arrives at the pin as
  ~857 mV through the 390 k / 100 k divider. At the 12 dB step the whole usable
  range would sit in the bottom quarter of the ADC's scale.
- **The Heltec V4 collision is mutual.** Both boards' probes now name their
  modem. Loosening either one back to "any radio answers" makes both boards
  identify as whichever probe runs first.

## 5. Sources

The pin map came from two published board definitions, not from hardware:

- Meshtastic `variants/esp32s3/meshnology-w12` — cites the board schematic
  `W12-MB-V0.2` for the battery divider; the source for the Vext polarity, the
  divider ratio and attenuation, and the DIO8 IRQ.
- The **GC1109 datasheet** (Geo-chip, rev 0.9.2) — the source for the sub-GHz
  band limits (860–930 MHz), the gain and saturated-power pair, and the
  CSD/CTX/CPS truth table the RF-switch masks transcribe.
- **A board in hand**, rev `W12-MB-V0.2` (silkscreen date 20260316) — the
  authority for the two silkscreened U.FL connectors (`2.4G`, `868/915`), for
  the sub-GHz chain being U.FL → L/C match → 16-pin 3×3 mm QFN → LR2021 with
  **no SAW filter** populated, and for the 32.000 MHz crystal beside the LR2021
  that confirms there is no TCXO to power.
- The vendor's **retail packaging and factory demo document**. The box carries a
  band tick-box (`☐2.4G ☑915M ☐868M`), which — given the board above is one
  design — records the antenna and firmware default packed rather than a
  variant. The demo firmware's console (`freq 915.28 (902-928 MHz)`,
  `pwr 22 (0-22 dBm)`) also confirms the SSD1315 panel and shows the factory
  program driving the bare chip with no front-end gain model at all.
- MeshCore `variants/meshnology_w12` — the source for the DIO RF-switch table,
  the PA supply gates, the low chip-drive ceiling and the GNSS lines.

Where they disagree, [README.md](README.md)'s "Verify before trusting" section
says which reading this straddle took and what the symptom of the wrong one is.
