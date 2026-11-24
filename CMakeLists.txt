cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(PION1 C CXX ASM)

# Initialise the SDK.  This must come after project().
pico_sdk_init()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(PION1_VERSION "0.1.0")

# ---------------------------------------------------------------------------
# Platform-independent DSP core.
#
# These sources are compiled both into the firmware and into the host test
# harness, so they must not include any Pico SDK header.
# ---------------------------------------------------------------------------
set(PION1_DSP_SOURCES
    src/dsp/bitops.c
    src/dsp/mixer.c
    src/dsp/resample.c
    src/dsp/sine_table.c
    src/dsp/voices.c
)

set(PION1_APP_SOURCES
    src/main.c
    src/params.c
    src/pio/bitstream.c
)

# The PIO programs are assembled by the SDK's pioasm wrapper.  The header is
# generated after the executable target exists, because the SDK's helper
# attaches the generation step as a dependency of the target.
set(PION1_PIO_HEADER ${CMAKE_CURRENT_BINARY_DIR}/pion1.pio.h)

add_executable(${PROJECT_NAME}
    ${PION1_APP_SOURCES}
    ${PION1_DSP_SOURCES}
)

pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/src/pio/pion1.pio
                         OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/dsp
)

# The SDK's libraries carry the include paths (pico_stdlib_headers,
# pico_base_headers, hardware_pio, ...) and the linker scripts.
target_link_libraries(${PROJECT_NAME} PRIVATE
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_pio
    hardware_dma
    hardware_sync
    hardware_timer
    hardware_watchdog
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PION1_VERSION="${PION1_VERSION}"
)

# ---------------------------------------------------------------------------
# Warnings: the project is built with -Wall -Wextra -Werror so that a
# regression never reaches the hardware.
#
# The strict flags are applied to this project's own sources only.  The SDK
# contributes INTERFACE sources (gpio.c, panic.c, ...) to the target, and
# those are compiled with the SDK's own settings -- they are not ours to
# police.
# ---------------------------------------------------------------------------
set(PION1_WARNINGS
    -Wall
    -Wextra
    -Werror
    -Wshadow
)

set(PION1_OWN_SOURCES
    ${PION1_APP_SOURCES}
    ${PION1_DSP_SOURCES}
)
set_source_files_properties(${PION1_OWN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "${PION1_WARNINGS}"
)

# ---------------------------------------------------------------------------
# Linker settings.
#
# The DMA rings and the parameter block live in RAM.  The scratch banks are
# used so that the audio core's data does not share a bank with the stack,
# which keeps the DMA ring accesses from stalling the CPU.
# ---------------------------------------------------------------------------
target_link_options(${PROJECT_NAME} PRIVATE
    -Wl,--print-memory-usage
)

pico_add_extra_outputs(${PROJECT_NAME})
