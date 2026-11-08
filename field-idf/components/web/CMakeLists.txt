idf_component_register(SRCS "web.cpp"
                       INCLUDE_DIRS "include"
                       EMBED_TXTFILES "www/settings.html"
                       REQUIRES esp_http_server esp_timer esp_wifi json lwip settings)
