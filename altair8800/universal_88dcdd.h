#ifndef UNIVERSAL_88DCDD_H
#define UNIVERSAL_88DCDD_H

#include "intel8080.h"
#include <stdbool.h>

bool host_disk_init(const char *drive_a, const char *drive_b, const char *drive_c, const char *drive_d);
void host_disk_close(void);
disk_controller_t host_disk_controller(void);

#endif
