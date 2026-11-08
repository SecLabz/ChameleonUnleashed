#pragma once

#include <stdbool.h>

bool network_init(void);
void network_poll(void);
/* Read-only pending-work check; safe from an RF callback. */
bool network_work_pending(void);

void network_disconnect(void);
