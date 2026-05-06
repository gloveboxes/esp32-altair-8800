#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define CHAT_PORT_TRIGGER 120
#define CHAT_PORT_REQUEST 121
#define CHAT_PORT_RESET_RESPONSE 122
#define CHAT_PORT_STATUS 123
#define CHAT_PORT_DATA 124

#define CHAT_STATUS_EOF 0
#define CHAT_STATUS_WAITING 1
#define CHAT_STATUS_DATA_READY 2

void chat_io_init(void);
void chat_io_prompt_api_key(void);
void chat_io_run_config_shell(void);
void chat_io_run_boot_shell(void);
void chat_io_set_network_available(bool available);
size_t chat_output(int port, uint8_t data, char *buffer, size_t buffer_length);
uint8_t chat_input(uint8_t port);
void chat_client_poll(void);

#ifdef __cplusplus
}
#endif