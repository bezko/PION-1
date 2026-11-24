# Host test harness for the PION/1 DSP core.
#
# The DSP sources are compiled for the host so that the algorithms can be
# verified without hardware.  This is a separate CMake project so that the
# host build never picks up the Pico SDK.

cmake_minimum_required(VERSION 3.13)
project(PION1_TESTS C)

set(CMAKE_C_STANDARD 11)

set(PION1_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

add_executable(test_dsp
    test_dsp.c
    ${PION1_ROOT}/src/dsp/bitops.c
    ${PION1_ROOT}/src/dsp/mixer.c
    ${PION1_ROOT}/src/dsp/resample.c
    ${PION1_ROOT}/src/dsp/sine_table.c
    ${PION1_ROOT}/src/dsp/voices.c
)

target_include_directories(test_dsp PRIVATE
    ${PION1_ROOT}/src
    ${PION1_ROOT}/src/dsp
)

target_compile_options(test_dsp PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wshadow
)

target_link_libraries(test_dsp PRIVATE m)

enable_testing()
add_test(NAME dsp COMMAND test_dsp)

# ---------------------------------------------------------------------------
# Host renderer: renders the bit stream to a WAV file so the result can be
# listened to and inspected without the hardware.
# ---------------------------------------------------------------------------
add_executable(render_host
    ${PION1_ROOT}/tools/render_host.c
    ${PION1_ROOT}/src/dsp/bitops.c
    ${PION1_ROOT}/src/dsp/mixer.c
    ${PION1_ROOT}/src/dsp/resample.c
    ${PION1_ROOT}/src/dsp/sine_table.c
    ${PION1_ROOT}/src/dsp/voices.c
)

target_include_directories(render_host PRIVATE
    ${PION1_ROOT}/src
    ${PION1_ROOT}/src/dsp
)

target_compile_options(render_host PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wshadow
)

target_link_libraries(render_host PRIVATE m)
