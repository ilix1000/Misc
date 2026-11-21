# SMS Plus (GPLv2, Charles MacDonald), as adapted for ESP32 by Retro-Go.
# Master System, Game Gear, SG-1000.
idf_component_register(SRC_DIRS "." "cpu" "sound"
                       INCLUDE_DIRS "." "cpu" "sound"
                       REQUIRES rg_compat esp_common)
target_compile_definitions(${COMPONENT_LIB} PRIVATE RETRO_GO=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -include esp_attr.h -Wno-error -w)
