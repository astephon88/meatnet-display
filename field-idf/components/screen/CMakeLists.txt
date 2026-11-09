# The fonts are the MicroPython unit's (fonts/, made by tools/make_font.py).
idf_component_register(SRCS "screen.cpp"
                       INCLUDE_DIRS "include"
                       EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../../../fonts/med.bin"
                                   "${CMAKE_CURRENT_LIST_DIR}/../../../fonts/small.bin"
                       REQUIRES esp_lcd esp_driver_spi esp_driver_gpio esp_driver_ledc)
