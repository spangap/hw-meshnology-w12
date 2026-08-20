/**
 * detect.cpp — is the hardware under this firmware a Meshnology W12?
 *
 * The board's one self-assertion: its own name when the OLED and an LR2021 both
 * answer on W12 pins, NULL otherwise. See hw-lilygo-tdeck/esp-idf/src/detect.cpp
 * for the contract both callers hold it to, and detect_probe.h for why
 * flashmon's detector carries a hand-kept copy (as `detect_hw_meshnology_w12`).
 *
 * The modem is what settles this board, not the panel. A Heltec WiFi LoRa 32 V4
 * has the same 16 MB flash, the same OLED on 17/18 with its reset on 21, and the
 * same LoRa header on 8/9/10/11/12/13 — it differs by the part on the end of it,
 * an SX1262 there against the LR2021 here. So the OLED only gets this far and
 * the radio decides.
 *
 * The rails: Vext gates the OLED's power and the OLED needs a reset pulse before
 * it will answer, so both are driven here. They are released ONLY when the probe
 * fails — on the board this actually is, the firmware wants them exactly as this
 * left them, and the detector is reset into real firmware straight after.
 */
#include "detect_probe.h"
#include "w12.h"

/* OLED — I2C on its own pins, at whichever of the two strapped addresses. */
#define DETECT_OLED_SDA   17
#define DETECT_OLED_SCL   18
#define DETECT_OLED_RST   21

/* LoRa header (straddle.yaml's CONFIG_LORA0_*, written out: those symbols only
 * exist when iface-lora is staged, and this must probe without it). */
#define DETECT_LORA_SCK    9
#define DETECT_LORA_MOSI  10
#define DETECT_LORA_MISO  11
#define DETECT_LORA_CS     8
#define DETECT_LORA_RST   12
#define DETECT_LORA_BUSY  13

extern "C" const char* detect_hw(void)
{
    if (!detect_flash_mb(16)) return NULL;

    detect_rail_drive(BOARD_VEXT_CTRL_PIN, BOARD_VEXT_ON_LEVEL);
    detect_rail_drive(DETECT_OLED_RST, 0);         /* pulse the OLED reset */
    esp_rom_delay_us(5000);
    gpio_set_level((gpio_num_t)DETECT_OLED_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Anchor: the SSD1315 OLED, an SSD1306 part that answers at 0x3C or 0x3D
     * depending on one strap. A bare ACK is all it offers. */
    if (!detect_ack2(DETECT_OLED_SDA, DETECT_OLED_SCL, 0x3C, 0x3D)) {
        detect_dbg("no OLED on 17/18 — not a W12");
        detect_rail_release(DETECT_OLED_RST);
        detect_rail_release(BOARD_VEXT_CTRL_PIN);
        return NULL;
    }
    if (!detect_radio_is(DETECT_LORA_SCK, DETECT_LORA_MOSI, DETECT_LORA_MISO,
                         DETECT_LORA_CS, DETECT_LORA_RST, DETECT_LORA_BUSY, "lr2021")) {
        detect_dbg("OLED answered but no LR2021 — not a W12");
        detect_rail_release(DETECT_OLED_RST);
        detect_rail_release(BOARD_VEXT_CTRL_PIN);
        return NULL;
    }

    detect_found("hw_meshnology_w12");
    return "hw-meshnology-w12";
}
