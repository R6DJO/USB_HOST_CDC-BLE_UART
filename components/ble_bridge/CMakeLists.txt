idf_component_register(SRCS "src/ble_bridge.c"
                    INCLUDE_DIRS "include"
                    REQUIRES nordic_uart_multi msg_router freertos log)
