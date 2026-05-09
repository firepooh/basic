idf_component_register(SRCS
                    "main.cpp"
                    "console.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES
                        console
                        nvs_flash
                        esp_event)