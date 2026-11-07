#pragma once
#include <stdint.h>
enum { HFCLK_USB = 1, HFCLK_LF = 2, HFCLK_NFC = 4, HFCLK_READER = 8 };
uint32_t app_hfclk_acquire(uint8_t owner);
uint32_t app_hfclk_release(uint8_t owner);
uint8_t app_hfclk_owners(void);
uint32_t app_hfclk_wait(void);
