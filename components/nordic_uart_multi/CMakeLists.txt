idf_component_register(SRCS "src/nordic_uart_multi.c"
                    INCLUDE_DIRS "include"
                    REQUIRES bt esp_ringbuf nvs_flash log freertos)
