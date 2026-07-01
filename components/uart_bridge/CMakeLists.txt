idf_component_register(SRCS "src/uart_bridge.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_uart msg_router freertos log)

# The Radio and Lora links must use distinct UART peripherals, otherwise the
# second uart_driver_install() returns ESP_ERR_INVALID_STATE -> abort -> reboot.
# Kconfig only documents the constraint; enforce it here, at configure time.
# (DEFINED guard because the value may legitimately be 0, which is falsy in a
# plain if().)
if(DEFINED CONFIG_UART_RADIO_NUM AND DEFINED CONFIG_UART_LORA_NUM)
    if(CONFIG_UART_RADIO_NUM EQUAL CONFIG_UART_LORA_NUM)
        message(FATAL_ERROR
            "CONFIG_UART_RADIO_NUM and CONFIG_UART_LORA_NUM must differ, but "
            "both are set to ${CONFIG_UART_RADIO_NUM}. Pick two distinct UART "
            "peripherals (UART0 is usually the log console; prefer UART1 and "
            "UART2) via menuconfig -> Component config -> UART Bridge.")
    endif()
endif()
