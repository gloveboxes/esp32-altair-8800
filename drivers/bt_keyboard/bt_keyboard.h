#pragma once

#include <stdbool.h>
#include <stdint.h>

bool bt_keyboard_init(void);
void bt_keyboard_request_pairing(void);
void bt_keyboard_request_disconnect(void);
void bt_keyboard_request_clear_bonds(void);
bool bt_keyboard_is_ready(void);
bool bt_keyboard_is_connecting(void);
bool bt_keyboard_is_connected(void);
bool bt_keyboard_has_bond(void);

/* Interactive Bluetooth keyboard manager (USB serial JTAG console). Runs a small
 * command shell (P/U/D/S/Q) until the keyboard connects or the user quits. */
void bt_keyboard_run_config_shell(void);

/* Boot-time interactive Bluetooth keyboard manager (USB serial JTAG console).
 * Prints status, waits 5 s for the user to press 'B' (or 'U' to clear bonds),
 * and then runs bt_keyboard_run_config_shell(). */
void bt_keyboard_run_boot_shell(void);

/* Print the current BLE keyboard status (initializing / connected / bonded /
 * etc.) to the USB serial JTAG console. */
void bt_keyboard_print_status(void);
