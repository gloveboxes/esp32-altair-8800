idf_component_register(SRCS "main.c"
                             "config.c"
                             "wifi.c"
                             "websocket_console.c"
                             "websocket_server.c"
                             "../captive_portal/captive_portal.c"
                             "../PortDrivers/io_ports.c"
                             "../PortDrivers/time_io.c"
                             "../PortDrivers/utility_io.c"
                             "../Altair8800/intel8080.c"
                             "../Altair8800/memory.c"
                             "../Altair8800/pico_88dcdd_flash.c"
                             "../Altair8800/esp32_88dcdd_sd_card.c"
                             "../drivers/sdcard_esp32/sdcard_esp32.c"
                             "../FrontPanel/ili9341.c"
                             "../FrontPanel/altair_panel.c"
                       INCLUDE_DIRS "." 
                                    ".."
                                    "../FrontPanel"
                                    "../Altair8800"
                                    "../Disks"
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
