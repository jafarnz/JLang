# Compiles app.jlang to RISC-V assembly with `jlang build` on every build.
# Set JLANG to the compiler's path if it isn't in build/ or on your PATH.
set(JLANG_APP "${CMAKE_CURRENT_LIST_DIR}/app.jlang")
set(JLANG_ASM "${CMAKE_CURRENT_BINARY_DIR}/program.s")
if(DEFINED ENV{JLANG})
    set(JLANG_EXE "$ENV{JLANG}")
else()
    find_program(JLANG_EXE jlang HINTS "${CMAKE_CURRENT_LIST_DIR}/../../../build")
endif()
if(NOT JLANG_EXE)
    message(FATAL_ERROR "jlang not found. Run `make` in the JLang folder first, or set JLANG.")
endif()

idf_component_register(SRCS "main.c" "${JLANG_ASM}"
                       INCLUDE_DIRS "../.."
                       PRIV_REQUIRES esp_driver_gpio esp_timer)

add_custom_command(OUTPUT "${JLANG_ASM}"
                   COMMAND "${JLANG_EXE}" build "${JLANG_APP}" -o "${JLANG_ASM}"
                   DEPENDS "${JLANG_APP}" "${JLANG_EXE}"
                   COMMENT "Compiling app.jlang to RISC-V"
                   VERBATIM)
