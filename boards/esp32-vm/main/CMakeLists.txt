# The JLang bytecode VM inside ESP-IDF firmware. Works on every ESP32
# (ESP32, S2, S3, C3, C6, H2), including the Xtensa chips.
set(JL ../../../src)
idf_component_register(SRCS "main.c"
                            "${JL}/builtins.c" "${JL}/chunk.c" "${JL}/compiler.c"
                            "${JL}/debug.c" "${JL}/heap.c" "${JL}/lexer.c"
                            "${JL}/value.c" "${JL}/vm.c"
                       INCLUDE_DIRS "." "${JL}"
                       PRIV_REQUIRES esp_driver_gpio esp_timer
                       EMBED_TXTFILES "app.jlang")
