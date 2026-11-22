# JLang native code on a Raspberry Pi Pico 2, using its RISC-V cores.
#   jlang build app.jlang -o program.s
#   mkdir build && cd build && cmake .. && make
# then copy jlang_app.uf2 to the board in BOOTSEL mode.
cmake_minimum_required(VERSION 3.13)
set(PICO_PLATFORM rp2350-riscv)
set(PICO_BOARD pico2)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(jlang_app C CXX ASM)
pico_sdk_init()
add_executable(jlang_app main.c program.s)
target_include_directories(jlang_app PRIVATE ..)
target_link_libraries(jlang_app pico_stdlib)
pico_enable_stdio_usb(jlang_app 1)
pico_enable_stdio_uart(jlang_app 0)
pico_add_extra_outputs(jlang_app)
