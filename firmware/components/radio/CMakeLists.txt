idf_component_register(SRCS "radio.cpp" "links.cpp" "field_source.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES bt esp_timer core settings)
