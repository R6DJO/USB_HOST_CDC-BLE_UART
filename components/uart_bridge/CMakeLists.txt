idf_component_register(SRCS "src/uart_bridge.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_uart msg_router freertos log)
