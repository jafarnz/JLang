# The JLang bytecode VM inside ESP-IDF firmware. Works on every ESP32
# (ESP32, S2, S3, C3, C6, H2), including the Xtensa chips.
set(JL ../../../src)
idf_component_register(SRCS "main.c" "hardware.c" "${JL}/hw.c" "${JL}/oled.c"
                            "${JL}/builtins.c" "${JL}/chunk.c" "${JL}/compiler.c"
                            "${JL}/debug.c" "${JL}/heap.c" "${JL}/lexer.c"
                            "${JL}/value.c" "${JL}/vm.c"
                       INCLUDE_DIRS "." "${JL}"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi esp_driver_ledc esp_driver_uart esp_driver_usb_serial_jtag esp_adc esp_timer nvs_flash
                       EMBED_TXTFILES "app.jlang")
