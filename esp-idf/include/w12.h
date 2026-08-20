/**
 * w12.h — Meshnology W12 board support for reticulous.
 *
 * The W12 is an ESP32-S3R8 (16 MB flash, 8 MB *octal* PSRAM) carrying one
 * Semtech LR2021 on its own SPI bus, behind a GC1109 sub-GHz front end that
 * takes the board to 30 dBm, with a 0.96" SSD1315 OLED and a GNSS header on a
 * Vext-gated peripheral rail. Board reference:
 * https://meshnology.com/products/meshnology-w12-lr2021-ultra-long-range-lora-meshtastic-device-kit
 *
 * What this module provides:
 *   - Compile-time constants for the board's own pins: the Vext rail, the GNSS
 *     reset/enable lines and the battery sense.
 *   - The board bring-up Services (W12Board / W12Battery).
 *
 * The OLED's pins belong to spangap/tinylcd (CONFIG_TINYLCD_*), the radio's and
 * both front-end supply gates to iface-lora (CONFIG_LORA*), and the GNSS
 * receiver's UART to spangap/gps
 * (CONFIG_GPS_*), all supplied as board VALUES from this straddle's
 * straddle.yaml `kconfig:` block. So this header carries only the board's own
 * pins. Runtime LoRa parameters (freq, BW, SF, …) live in storage at s.lora.*.
 */
#pragma once

#include "sdkconfig.h"
#include "service.h"

/* The board's name as a person reads it — published to sys.board at init and
 * shown in the Hardware section of Settings, so the UI never spells a board
 * name of its own. */
#define BOARD_NAME              "Meshnology W12"

/* Vext peripheral power-enable. Gates the +3.3 V rail carrying the OLED and the
 * GNSS header behind a P-MOSFET on GPIO 45: drive the gate LOW to turn the rail
 * ON. The LR2021 is powered directly, not off Vext, so the radio works
 * regardless of this pin.
 *
 * GPIO 45 is also the VDD_SPI strapping pin, sampled at reset and weakly pulled
 * down, so the rail is already on by the time any code runs; driving it here is
 * what keeps it on rather than what turns it on. */
#define BOARD_VEXT_CTRL_PIN     45
#define BOARD_VEXT_ON_LEVEL     0   /* 0 = pull low to enable (active-low) */

/* The two front-end supply gates (GPIO 4 sub-GHz, GPIO 3 at 2.4 GHz) are NOT
 * here: they belong to iface-lora's CONFIG_LORA0_FEM_PWR_PIN /
 * _FEM_HF_PWR_PIN, which raises the one the configured carrier needs and drops
 * the other on every begin. Only the radio knows what frequency it is about to
 * program, so only the radio can pick. Both nets are pulled up on the board, so
 * a build without iface-lora leaves both amplifiers powered and harmless.
 *
 * Which path is live within a band is the chip's own business, driven from its
 * DIOs (CONFIG_LORA0_LR_RFSW_*). */

/* GNSS receiver (a Quectel L76K on the board's header, populated on the kits
 * that ship with one). Its UART is spangap/gps's CONFIG_GPS_*; these two lines
 * are the board's, because gps has no symbol for either. Both are active low:
 * the supply gate must be pulled LOW for the receiver to run, and reset must be
 * released HIGH. */
#define BOARD_GPS_EN_PIN        48   /* LOW = receiver powered */
#define BOARD_GPS_RST_PIN       42   /* LOW = held in reset */

/* Battery sense: VBAT through a 390 k / 100 k divider into GPIO 1 (ADC1_CH0),
 * behind a high-side switch on GPIO 2. The switch is a P-MOSFET with an NPN
 * inverting its gate, so GPIO 2 is active HIGH — undriven, the switch stays off
 * and the ADC reads a hard zero. Divider ratio is 490/100; the pin sees ~857 mV
 * at a full 4.2 V cell, which is why the channel runs at 2.5 dB attenuation
 * rather than the 12 dB a full-scale divider would want. */
#define BOARD_BAT_ADC           1
#define BOARD_BAT_EN_PIN        2
#define BOARD_BAT_EN_ACTIVE     1   /* 1 = drive high to connect the divider */

/* Present on the board but unwired — the platform has no engine for it: an
 * addressable RGB LED on GPIO 46. */
#define BOARD_RGB_LED_PIN       46

/**
 * Board bring-up. onStart is the always-on hardware bring-up: the Vext rail,
 * the GNSS enable/reset lines and the LoRa CS park. It runs in the start band, before spangapInit() — and before tinylcd's
 * task touches the Vext-powered OLED. onInit publishes sys.board once storage
 * exists to say it into.
 */
class W12Board : public Service {
public:
    void onStart() override;   /* rails + GNSS lines + CS park */
    void onInit()  override;   /* publishes sys.board */
};

/**
 * Battery monitor bring-up (onInit): configures the GPIO 1 ADC, publishes an
 * initial battery.millivolt / battery.percent, and arms a once-a-minute
 * esp_timer to keep them fresh. init band (needs storage up). No task of its
 * own — the periodic timer callback does the sampling, and it is also what
 * closes the divider's switch for the length of a reading.
 */
class W12Battery : public Service {
public:
    void onInit() override;
};
