# Glass firmware: the JLang VM twice (the display and the program being
# shown), glass.jlang and program.jlang built in, and the hardware
# built-ins they use.
set(JL ../../../src)
idf_component_register(SRCS "main.c" "../../core/glass.c"
                            "${JL}/builtins.c" "${JL}/chunk.c" "${JL}/compiler.c"
                            "${JL}/debug.c" "${JL}/heap.c" "${JL}/lexer.c"
                            "${JL}/value.c" "${JL}/vm.c"
                       INCLUDE_DIRS "." "${JL}" "../../core"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_adc esp_timer
                       EMBED_TXTFILES "../../glass.jlang" "../../program.jlang")
