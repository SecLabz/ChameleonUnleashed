#ifndef NFCA_COMMANDS_H
#define NFCA_COMMANDS_H
#include <stdint.h>

/* Shared NFC-A operations. Output has at least NFCA_COMMAND_OUTPUT_BYTES bytes.
 * No transport framing; callers own prepare/finish and response delivery. */
#define NFCA_COMMAND_OUTPUT_BYTES 272u
uint16_t nfca_command(uint16_t command, uint16_t length, uint8_t *data,
                     uint8_t *output, uint16_t *output_length);
uint16_t nfca_check_keys_on_block(uint16_t length, uint8_t *data, uint8_t *output,
                                  uint16_t *output_length, const uint8_t *expected_uid,
                                  uint8_t uid_length);
uint16_t nfca_prepare(uint16_t command, uint16_t length, const uint8_t *data);
void nfca_finish(uint16_t command);
#endif
