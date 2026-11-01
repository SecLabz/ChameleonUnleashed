#pragma once
#include <stdint.h>

/* Explicit unaligned little-endian serialization. */
uint16_t read_le16(const uint8_t *bytes);
uint32_t read_le32(const uint8_t *bytes);
void write_le16(uint8_t *bytes, uint16_t value);
void write_le32(uint8_t *bytes, uint32_t value);
