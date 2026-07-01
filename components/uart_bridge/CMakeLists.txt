idf_component_register(SRCS "src/uart_radio.c" "src/uart_lora.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_uart msg_router freertos log)
