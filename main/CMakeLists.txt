idf_component_register(SRCS "main.c"
                             "config.c"
                             "wifi.c"
                             "websocket_console.c"
                             "websocket_server.c"
                             "../captive_portal/captive_portal.c"
                             "../port_drivers/io_ports.c"
                             "../port_drivers/time_io.c"
                             "../port_drivers/utility_io.c"
                             "../altair8800/intel8080.c"
                             "../altair8800/memory.c"
                             "../altair8800/pico_88dcdd_flash.c"
                             "../altair8800/esp32_88dcdd_sd_card.c"
                             "../drivers/sdcard_esp32/sdcard_esp32.c"
                             "../front_panel/ili9341.c"
                             "../front_panel/altair_panel.c"
                       INCLUDE_DIRS "." 
                                    ".."
                                    "../front_panel"
                                    "../altair8800"
                                    "../disks"
                                    "../drivers/sdcard_esp32"
                                    "../captive_portal"
                                    "../terminal"
                       REQUIRES fatfs 
                                driver 
                                esp_lcd 
                                spi_flash 
                                sdmmc
                                esp_driver_sdmmc
                                esp_driver_usb_serial_jtag
                                esp_timer
                                nvs_flash
                                esp_wifi
                                esp_http_server
                                esp_netif
                                esp_event
                                lwip)
