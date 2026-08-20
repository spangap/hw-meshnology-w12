/**
 * w12.cpp — Meshnology W12 board support, end to end.
 *
 * Single owner of all W12 hardware bring-up. See w12.h for the API contract and
 * the pin map. Layout:
 *
 *   1. Vext rail, GNSS lines and the LoRa CS park (W12Board::onStart).
 *   2. Battery monitor (W12Battery).
 *
 * The LR2021 sits on its own SPI bus, separate from the flash bus and from the
 * OLED's I2C, and is powered directly rather than off Vext — so there is no
 * shared-bus probe to race during bring-up. The CS park is still required so
 * the deselected radio stays off the bus until iface-lora owns the pin.
 */
#include "w12.h"
#include "log.h"            /* warn (battery adc) */
#include "storage.h"        /* battery.* ephemerals, sys.board */

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>

/* =========================================================================
 * 1. Vext rail, GNSS lines and the LoRa CS park
 *
 * All of it before spangapInit(), because the two consumers are both earlier
 * than they look: tinylcd initialises a panel that is dark until Vext is up,
 * and the GNSS receiver needs its supply and its reset settled long before the
 * UART is opened. The radio's own supply is not gated at all, and its two
 * front-end supply gates are iface-lora's — it raises the one the configured
 * carrier needs, which is a thing only the radio knows.
 * ========================================================================= */

namespace {

void driveOut(int pin, int level)
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode         = GPIO_MODE_OUTPUT;
    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    gpio_set_level((gpio_num_t)pin, level);
}

void w12PowerInit(void)
{
    driveOut(BOARD_VEXT_CTRL_PIN, BOARD_VEXT_ON_LEVEL);
    /* Exempt the gate from light-sleep isolation: CONFIG_PM_SLP_DISABLE_GPIO
     * switches every pin to its sleep config on sleep entry, which floats this
     * active-low gate and cuts the rail mid-sleep — the OLED loses VCC on the
     * first light-sleep entry (e.g. the moment `usb down` releases the console's
     * no-sleep lock) and comes back uninitialised. */
    gpio_sleep_sel_dis((gpio_num_t)BOARD_VEXT_CTRL_PIN);

    /* GNSS: supply on, then reset released. Both active low, so this is two
     * lows and then a high. Harmless on a board whose header is unpopulated —
     * the pins simply go nowhere. */
    driveOut(BOARD_GPS_EN_PIN, 0);
    driveOut(BOARD_GPS_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));   /* 3.3 V rail settle */
    gpio_set_level((gpio_num_t)BOARD_GPS_RST_PIN, 1);

    /* Park the LR2021's CS HIGH (deselected) so it doesn't drive MISO before
     * loraInit() claims the pin. The pin comes from iface-lora's Kconfig;
     * defined only when that straddle is staged. */
#if defined(CONFIG_LORA0_CS_PIN)
    driveOut(CONFIG_LORA0_CS_PIN, 1);
#endif
}

}  // namespace

/* =========================================================================
 * 2. Battery monitor — VBAT via the GPIO 1 divider (see BOARD_BAT_ADC).
 *
 * Always compiled (no UI dependency): once a minute the divider's high-side
 * switch closes, a second timer fires a settle later to sample the ADC and open
 * the switch again, and two ephemerals the rest of the system reacts to are
 * published —
 *   battery.millivolt  — true VBAT in mV (pin reading × divider)
 *   battery.percent    — 0..100, via the open-circuit-voltage curve below
 * tinylcd's status page and spangap-core's `bat` CLI command read them. No
 * dedicated task — both callbacks run on the esp_timer task. The switch is
 * closed only for the length of a reading, which is the point of it: a divider
 * left connected is a permanent load on the cell.
 * ========================================================================= */

namespace {

/* 390 k over 100 k, so VBAT × 100/490 arrives at the pin. Trim NUM/DEN if a
 * multimeter disagrees. */
constexpr uint32_t BAT_DIV_NUM   = 490, BAT_DIV_DEN = 100;
constexpr int      BAT_SAMPLES   = 16;             /* averaged per read — kills ADC jitter */
constexpr int      BAT_SETTLE_MS = 10;             /* switch closed → divider settled */
constexpr int64_t  BAT_PERIOD_US = 60LL * 1000000; /* re-sample cadence: every minute */

/* The pin sees a full cell as ~857 mV, so the 2.5 dB step (~0..1.1 V) is the
 * one that spends the ADC's range on the voltage actually present. At 12 dB the
 * whole cell would live in the bottom quarter of the scale. */
constexpr adc_atten_t BAT_ATTEN = ADC_ATTEN_DB_2_5;

/* Open-circuit voltage at 100 %, 90 %, … 0 % for the single Li-ion cell the
 * kits ship. Linear interpolation between the decade points; input jitter is
 * smoothed by the per-read averaging + EMA below. */
const uint16_t s_ocvMv[11] = {
    4160, 4020, 3940, 3870, 3810, 3760, 3740, 3720, 3680, 3620, 2990,
};

adc_oneshot_unit_handle_t s_adc      = nullptr;
adc_cali_handle_t         s_adcCali  = nullptr;
adc_unit_t                s_adcUnit  = ADC_UNIT_1;
adc_channel_t             s_adcChan  = ADC_CHANNEL_0;   /* GPIO 1; confirmed at init */
bool                      s_adcReady = false;
uint32_t                  s_mvEma    = 0;               /* smoothed VBAT, mV (0 = unset) */

uint8_t batteryPercent(uint16_t mv) {
    if (mv >= s_ocvMv[0])  return 100;
    if (mv <= s_ocvMv[10]) return 0;
    for (int i = 1; i <= 10; i++) {
        if (mv >= s_ocvMv[i]) {
            uint16_t hi = s_ocvMv[i - 1], lo = s_ocvMv[i];
            int pctLo = 100 - i * 10;
            return (uint8_t)(pctLo + (uint32_t)(mv - lo) * 10 / (hi - lo));
        }
    }
    return 0;
}

esp_timer_handle_t s_settleTimer = nullptr;

/* Sample, smooth, publish, and open the switch again. Runs on the esp_timer
 * task, BAT_SETTLE_MS after batteryArm closed the switch. */
void batterySample(void*) {
    int acc = 0, ok = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, s_adcChan, &raw) == ESP_OK) { acc += raw; ok++; }
    }
    gpio_set_level((gpio_num_t)BOARD_BAT_EN_PIN, !BOARD_BAT_EN_ACTIVE);
    if (!ok) return;
    int raw = acc / ok;
    int pinMv;
    if (!(s_adcCali && adc_cali_raw_to_voltage(s_adcCali, raw, &pinMv) == ESP_OK))
        pinMv = (int)((int64_t)raw * 1100 / 4095);      /* nominal 12-bit @ 2.5 dB */
    uint32_t mv = (uint32_t)pinMv * BAT_DIV_NUM / BAT_DIV_DEN;
    /* Light EMA across reads (~3-4 min at the 1/min cadence) so the icon and
     * percent don't wobble on noise; first reading seeds it directly (no lag). */
    s_mvEma = s_mvEma ? (s_mvEma * 3 + mv) / 4 : mv;
    uint16_t outMv = (uint16_t)s_mvEma;
    storageBegin();                                     /* one commit -> subscribers see both */
    storageSet("battery.millivolt", (int)outMv);
    storageSet("battery.percent",   (int)batteryPercent(outMv));
    storageEnd();
}

/* Close the divider's switch and come back for the reading once it has settled.
 * Two shots rather than one because the settle is real and the esp_timer task
 * is shared: a sleep inside the callback would hold every other timer in the
 * system for its duration, once a minute, forever. */
void batteryArm(void*) {
    if (!s_adcReady || !s_settleTimer) return;
    gpio_set_level((gpio_num_t)BOARD_BAT_EN_PIN, BOARD_BAT_EN_ACTIVE);
    esp_timer_start_once(s_settleTimer, BAT_SETTLE_MS * 1000);
}

}  // namespace

/* =========================================================================
 * Public API — the board bring-up Services (see w12.h).
 * ========================================================================= */

void W12Board::onStart() {
    w12PowerInit();     /* Vext rail + GNSS lines + LoRa CS park */
}

/* onInit — the board says what it is, once storage exists to say it into. Every
 * surface that names the hardware (the Hardware section of Settings, on both
 * the display and the browser) reads this key, so a board is identified in one
 * place rather than by each surface knowing which board it is running on. */
void W12Board::onInit() {
    storageSet("sys.board", BOARD_NAME);
}

/* onInit — ADC bring-up, an initial reading, then the once-a-minute timer.
 * Runs after spangapInit() so storage is up for the ephemeral writes. */
void W12Battery::onInit() {
    /* The divider's switch, parked open: batteryArm is the only thing that ever
     * closes it, and batterySample the only thing that opens it again. */
    driveOut(BOARD_BAT_EN_PIN, !BOARD_BAT_EN_ACTIVE);

    if (adc_oneshot_io_to_channel(BOARD_BAT_ADC, &s_adcUnit, &s_adcChan) != ESP_OK) {
        warn("battery: GPIO%d is not an ADC pin\n", BOARD_BAT_ADC);
        return;
    }
    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id = s_adcUnit;
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) {
        warn("battery: adc unit init failed\n");
        return;
    }
    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.atten    = BAT_ATTEN;
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(s_adc, s_adcChan, &ccfg) != ESP_OK) {
        warn("battery: adc channel config failed\n");
        return;
    }
    adc_cali_curve_fitting_config_t cal = {};
    cal.unit_id  = s_adcUnit;
    cal.chan     = s_adcChan;
    cal.atten    = BAT_ATTEN;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_adcCali) != ESP_OK) {
        s_adcCali = nullptr;                /* fall back to nominal raw->mV scaling */
        warn("battery: adc calibration unavailable, using nominal scale\n");
    }
    const esp_timer_create_args_t scfg = { .callback = batterySample, .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK, .name = "battery_rd", .skip_unhandled_events = true };
    if (esp_timer_create(&scfg, &s_settleTimer) != ESP_OK) {
        warn("battery: settle timer create failed\n");
        return;
    }
    s_adcReady = true;

    batteryArm(nullptr);                    /* first reading lands a settle later */

    const esp_timer_create_args_t targs = { .callback = batteryArm, .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK, .name = "battery", .skip_unhandled_events = true };
    esp_timer_handle_t th = nullptr;
    if (esp_timer_create(&targs, &th) == ESP_OK)
        esp_timer_start_periodic(th, BAT_PERIOD_US);
    else
        warn("battery: timer create failed\n");
}
