#ifndef NFC_15_H
#define NFC_15_H
#include "tag_emulation.h"
#include "iso15693/session.h"

void nfc_tag_15_prepare_load(void);
int nfc_tag_15_load(tag_specific_type_t type, tag_data_buffer_t *buffer);
int nfc_tag_15_save(tag_specific_type_t type, tag_data_buffer_t *buffer);
bool nfc_tag_15_factory(uint8_t slot, tag_specific_type_t type);
void nfc_tag_15_sense_switch(bool enable);
void nfc_tag_15_poll(void);
bool nfc_tag_15_active(void);
uint16_t nfc_tag_15_image_command(bool write, uint16_t length, const uint8_t *data,
                                  uint8_t *output, uint16_t *output_length);
#endif
