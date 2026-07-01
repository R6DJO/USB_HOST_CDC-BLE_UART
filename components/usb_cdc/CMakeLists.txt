idf_component_register(SRCS "src/usb_cdc.c"
                    INCLUDE_DIRS "include"
                    REQUIRES msg_router usb_host_cdc_acm freertos log)
