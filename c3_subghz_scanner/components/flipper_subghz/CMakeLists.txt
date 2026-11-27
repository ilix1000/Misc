# Flipper Zero Sub-GHz protocol library (lib/subghz from the Flipper firmware,
# unmodified decoders) + a small compatibility layer for plain ESP-IDF.
file(GLOB PROTO_SRCS "subghz/protocols/*.c" "subghz/blocks/*.c")
idf_component_register(
    SRCS ${PROTO_SRCS}
         "subghz/receiver.c" "subghz/environment.c" "subghz/registry.c"
         "toolbox/manchester_decoder.c" "toolbox/manchester_encoder.c" "toolbox/float_tools.c"
         "shim/furi_shim.c" "shim/flipper_format_stub.c"
    INCLUDE_DIRS "shim" "shim/lib/toolbox" "subghz" "."
    REQUIRES esp_timer freertos log)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable
    -Wno-sign-compare -Wno-missing-field-initializers -Wno-format -Wno-type-limits
    -Wno-implicit-fallthrough -Wno-maybe-uninitialized -Wno-error)
