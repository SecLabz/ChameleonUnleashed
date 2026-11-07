#pragma once
#include <stdbool.h>
#include <stdint.h>
#define RECOVERY_VECTOR_COUNT 128u
#define RECOVERY_VECTOR_ALIGNMENT (RECOVERY_VECTOR_COUNT * sizeof(uint32_t))
uint32_t recovery_millis(void);
void recovery_reboot_later(void);
void recovery_power_poll(bool restarting);
