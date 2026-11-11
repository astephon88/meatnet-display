idf_component_register(SRCS "web.cpp"
                       INCLUDE_DIRS "include"
                       EMBED_TXTFILES "../../../www/index.html"
                       REQUIRES core esp_http_server esp_timer esp_wifi json lwip settings)
