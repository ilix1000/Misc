# PrBoom (GPLv2) as ported to ESP32 by Retro-Go ("prboom-go"): the Doom engine.
idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "."
                       REQUIRES rg_compat esp_common
                       LDFRAGMENTS linker.lf)
target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_CONFIG_H RETRO_GO=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w -Wno-error)
