idf_component_register(SRCS "src/msg_router.c"
                    INCLUDE_DIRS "include"
                    REQUIRES freertos esp_ringbuf log)
