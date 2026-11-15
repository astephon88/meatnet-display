idf_component_register(SRCS "radio.cpp" "links.cpp" "esp_radio.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES bt esp_timer core settings)
