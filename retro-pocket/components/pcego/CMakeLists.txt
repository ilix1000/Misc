# PCE-GO (GPLv2), Retro-Go's PC Engine / TurboGrafx-16 emulator.
idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "."
                       REQUIRES rg_compat esp_common)
target_compile_definitions(${COMPONENT_LIB} PRIVATE RETRO_GO=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -include esp_attr.h -Wno-error -w)
