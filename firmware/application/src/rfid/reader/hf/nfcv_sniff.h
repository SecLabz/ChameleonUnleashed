#pragma once
#include <stdbool.h>
#include <stdint.h>

bool nfcv_sniff_start(uint8_t gain);
void nfcv_sniff_poll(void);
void nfcv_sniff_stop(void);
