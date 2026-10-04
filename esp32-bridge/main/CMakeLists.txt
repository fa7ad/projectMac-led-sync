idf_component_register(SRCS "main.c" "lamp.c"
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "index.html"
                    REQUIRES bt nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer lwip)
