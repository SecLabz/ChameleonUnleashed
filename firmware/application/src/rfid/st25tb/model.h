#pragma once
#include <stdbool.h>
#include <stdint.h>

#define ST25TB_IMAGE_SIZE 77u
#define ST25TB_REQUEST_MAX 8u
#define ST25TB_RESPONSE_MAX 10u

/* Wire UID, sixteen 4-byte blocks, system block, fixed inventory Chip_ID. */
bool st25tb_load(const uint8_t image[ST25TB_IMAGE_SIZE]);
bool st25tb_save(uint8_t image[ST25TB_IMAGE_SIZE]);
bool st25tb_loaded(void);
void st25tb_unload(void);
void st25tb_field_lost(void);
unsigned st25tb_reply(const uint8_t *frame, unsigned length,
                      uint8_t reply[ST25TB_RESPONSE_MAX]);
