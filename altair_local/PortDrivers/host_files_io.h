#ifndef HOST_FILES_IO_H
#define HOST_FILES_IO_H

#include <stddef.h>
#include <stdint.h>

void host_files_init(const char *apps_root);
void host_files_out(uint8_t port, uint8_t data);
uint8_t host_files_in(uint8_t port);

/*
 * ESP32-compatible shim. The shared io_ports.c dispatch (mirror of the
 * firmware's port_drivers/io_ports.c) calls files_output()/files_input();
 * these forward to the host implementations above. The buffer/length args
 * are unused on the host because FT data is returned via port 60/61 reads,
 * matching the ESP32 driver.
 */
size_t files_output(int port, uint8_t data, char *buffer, size_t buffer_length);
uint8_t files_input(uint8_t port);

#endif
