#pragma once
#include "tag_base_type.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SLOT_IMAGE_MAX_BYTES 4500u

typedef struct {
    tag_specific_type_t type;
    tag_sense_type_t sense;
    uint16_t memory_bytes, native_bytes;
    uint8_t unit_bytes, option_bytes;
} slot_image_shape_t;

bool slot_image_shape(tag_specific_type_t type, slot_image_shape_t *shape);
bool slot_image_encode(tag_specific_type_t type, const uint8_t *native, size_t native_size,
    uint8_t *wire, size_t capacity, uint16_t *size);
bool slot_image_decode(const uint8_t *wire, size_t size, uint8_t *native,
    size_t capacity, slot_image_shape_t *shape);
