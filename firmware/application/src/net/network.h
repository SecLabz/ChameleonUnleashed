#pragma once

#include <stdbool.h>

bool network_init(void);
void network_poll(void);

void network_disconnect(void);
