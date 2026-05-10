idf_component_register(SRCS
                    "main.cpp"
                    "console.cpp"
                    "weather.c"
                    INCLUDE_DIRS "."
                    REQUIRES
                        console
                        nvs_flash
                        esp_event
                        esp_http_client
                        json)