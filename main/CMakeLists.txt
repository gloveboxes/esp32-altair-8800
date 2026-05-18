set(app_sources
    "i8080_disasm.c"
    "cpu_state.c"
    "main.c"
    "ansi_input.c"
    "config.c"
    "network_time.c"
    "time_setup.c"
    "wifi.c"
    "wifi_setup.c"
    "status_led.c"
    "websocket_console.c"
    "websocket_server.c"
    "../captive_portal/captive_portal.c"
    "../port_drivers/io_ports.c"
    "../port_drivers/json_scan.c"
    "../port_drivers/chat_io.c"
    "../port_drivers/environment_io.c"
    "../port_drivers/files_io.c"
    "../port_drivers/time_io.c"
    "../port_drivers/utility_io.c"
    "../port_drivers/weather_io.c"
    "../altair8800/intel8080.c"
    "../altair8800/memory.c"
    "../altair8800/pico_88dcdd_flash.c"
    "../altair8800/esp32_88dcdd_sd_card.c"
    "../drivers/sdcard_esp32/sdcard_esp32.c"
    "../drivers/axs15231b/esp_lcd_axs15231b.c"
    "../drivers/bt_keyboard/bt_keyboard.c"
    "../front_panel/panel_display.c"
    "../front_panel/ili9341.c"
    "../front_panel/axs15231b_lcd.c"
    "../front_panel/altair_panel.c"
    "../front_panel/virtual_monitor.c"
    "../front_panel/vt100_terminal.c"
    "../front_panel/altair_splash.c"
)

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "." 
                                    ".."
                                    "../front_panel"
                                    "../altair8800"
                                    "../disks"
                                    "../drivers/sdcard_esp32"
                                    "../drivers/bt_keyboard"
                                    "../drivers/axs15231b/include"
                                    "../drivers/axs15231b/priv_include"
                                    "../captive_portal"
                                    "../terminal"
                       REQUIRES fatfs 
                                driver 
                                esp_lcd 
                                spi_flash 
                                sdmmc
                                esp_driver_sdmmc
                                esp_driver_usb_serial_jtag
                                esp_driver_ledc
                                esp_driver_rmt
                                esp_timer
                                bt
                                esp_hid
                                nvs_flash
                                esp_wifi
                                esp_http_server
                                esp_http_client
                                esp_netif
                                esp_event
                                esp-tls
                                mbedtls
                                lwip)

# Enable stricter warnings for app code only
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wunused-function
    -Wunused-variable
    -Wunused-but-set-variable
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP_LCD_AXS15231B_VER_MAJOR=2
    ESP_LCD_AXS15231B_VER_MINOR=1
    ESP_LCD_AXS15231B_VER_PATCH=0
)
