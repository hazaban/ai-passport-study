# libspeex component for the Voice Keychain (ESP32-C3, no FPU).
# Fixed-point narrowband voice codec, used for the study recorder.
# Built with FIXED_POINT; -ffunction-sections/-fgc-sections drops the
# wideband/floating code that is never called for 8kHz recorder use.
idf_component_register(
    SRCS
        "libspeex/bits.c"
        "libspeex/cb_search.c"
        "libspeex/exc_10_16_table.c"
        "libspeex/exc_10_32_table.c"
        "libspeex/exc_20_32_table.c"
        "libspeex/exc_5_256_table.c"
        "libspeex/exc_5_64_table.c"
        "libspeex/exc_8_128_table.c"
        "libspeex/filters.c"
        "libspeex/gain_table.c"
        "libspeex/gain_table_lbr.c"
        "libspeex/hexc_10_32_table.c"
        "libspeex/hexc_table.c"
        "libspeex/high_lsp_tables.c"
        "libspeex/lpc.c"
        "libspeex/lsp.c"
        "libspeex/lsp_tables_nb.c"
        "libspeex/ltp.c"
        "libspeex/modes.c"
        "libspeex/nb_celp.c"
        "libspeex/quant_lsp.c"
        "libspeex/speex.c"
        "libspeex/speex_callbacks.c"
        "libspeex/stereo.c"
        "libspeex/vbr.c"
        "libspeex/vq.c"
        "libspeex/window.c"
    INCLUDE_DIRS "include"
    REQUIRES
)

# Fixed-point narrowband build. OUTSIDE_SPEEX supplies our own os_support
# glue; USE_SMALLFT uses the included DCT instead of libm fft. RANDOM_PREFIX
# renames fft/rand symbols so they don't clash with ESP-IDF's libc rand.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FIXED_POINT
    OUTSIDE_SPEEX
    USE_SMALLFT
    DISABLE_FLOAT_API
    EXPORT=
    RANDOM_PREFIX=xiph_spx
)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-parameter
    -Wno-sign-compare
    -ffunction-sections
    -fdata-sections
)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "include"
    "libspeex"
)