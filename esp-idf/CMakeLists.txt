# hw-meshnology-w12 board HAL — an ordinary spangap component (non-buildable).
#
# Compiles the Meshnology W12 board code: the Vext rail, the sub-GHz front end's
# supply gate, the GNSS enable/reset lines, the LoRa CS park and the battery
# monitor (w12.cpp). No on-device UI — the board's OLED is spangap/tinylcd's.
#
# app_main is NOT here — it's generated into the reticulous/reticulous buildable
# that adds this board with `--with`. This component exports the board's Service
# classes (W12Board, W12Battery) for the generated dispatcher to construct.
include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
idf_component_register(
    SRCS
        "src/w12.cpp"
        # This board's one self-assertion (detect_hw), called by
        # spangapInit() before any bus is claimed.
        "src/detect.cpp"
        ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    REQUIRES
        ${SPANGAP_REQUIRES}
        driver
        esp_adc          # battery voltage ADC on GPIO 1
        esp_timer
        esp_event
        esp_netif
        esp_wifi
        lwip
)
