#pragma once
#include "tag_emulation.h"
#include "desfire_model.h"
int nfc_desfire_load(tag_specific_type_t type, tag_data_buffer_t *buffer);
int nfc_desfire_save(tag_specific_type_t type, tag_data_buffer_t *buffer);
bool nfc_desfire_factory(uint8_t slot, tag_specific_type_t type);
uint16_t nfc_desfire_image_command(bool write, uint16_t size, const uint8_t *data,
    uint8_t *output, uint16_t *length);
