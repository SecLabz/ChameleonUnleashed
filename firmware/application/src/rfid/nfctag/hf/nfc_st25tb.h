#ifndef NFC_ST25TB_H
#define NFC_ST25TB_H

#include "tag_emulation.h"

int nfc_st25tb_data_loadcb(tag_specific_type_t type, tag_data_buffer_t *buffer);
int nfc_st25tb_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer);
bool nfc_st25tb_data_factory(uint8_t slot, tag_specific_type_t type);
void nfc_st25tb_sense_switch(bool enable);
bool nfc_st25tb_service(void);
bool nfc_st25tb_set_image(const uint8_t image[77]);
bool nfc_st25tb_get_image(uint8_t image[77]);
bool nfc_st25tb_radio_poll(bool *field_present, bool active);
void nfc_st25tb_radio_stop(void);

#endif
