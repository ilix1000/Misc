# u8g2 graphics library (copied from the Flipper ESP32 port, display drivers
# not needed: we render into the buffer and push it to the SSD1306 ourselves)
file(GLOB U8G2_SRCS "*.c")
idf_component_register(SRCS ${U8G2_SRCS} INCLUDE_DIRS ".")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused -Wno-error)
