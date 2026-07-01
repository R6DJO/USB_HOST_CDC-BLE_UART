idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES msg_router uart_bridge usb_cdc ble_bridge)
