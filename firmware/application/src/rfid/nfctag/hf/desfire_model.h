#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Keep the existing 4500-byte HF buffer and HTTP limit. Public DESFire
 * captures are usually small; card capacity is not image size. Larger public
 * captures remain exportable on the host, but must fail before slot mutation.
 * Do not truncate files or enlarge every slot to the card's advertised size. */
#define DESFIRE_SLOT_MAX 4492u /* native word padding plus the 5-byte HTTP envelope */
#define DESFIRE_APPS_MAX 28u
#define DESFIRE_FILES_MAX 64u

typedef struct {
    const uint8_t *aid, *keys, *versions;
    uint8_t state, key_len, version_len, files_state, first_file, files;
} desfire_app_t;
typedef struct {
    const uint8_t *settings, *data;
    uint16_t settings_len, data_len;
    uint8_t id, settings_state, data_state;
} desfire_file_t;
typedef struct {
    const uint8_t *uid, *atqa, *ats, *version, *keys, *versions;
    uint8_t uid_len, sak, ats_len, key_state, key_len, version_len, apps_state;
    bool public_capture, free_present;
    uint32_t free_memory;
    desfire_app_t apps[DESFIRE_APPS_MAX];
    desfire_file_t files[DESFIRE_FILES_MAX];
    uint8_t app_count, file_count;
    int8_t selected;
    const uint8_t *pending;
    uint16_t pending_len;
    uint8_t pending_status, version_part;
    uint8_t scratch[84];
} desfire_model_t;

/* Native image: UID length, UID, BE16 metadata length, BE16 data length,
 * DFA1/DFD1 observations (version 1), or DFU1 identity with no file data.
 * Returns the actual word-aligned persistent size, never fixed-size padding. */
unsigned desfire_image_size(const uint8_t *data, size_t capacity);
bool desfire_model_load(desfire_model_t *model, const uint8_t *data, size_t size);
void desfire_model_reset(desfire_model_t *model);
/* Native request/response without ISO-DEP PCB/CRC; wrapped APDUs are adapted
 * by the transport. Results contain one native status byte then <=24 data bytes. */
unsigned desfire_model_command(desfire_model_t *model, const uint8_t *data,
    unsigned size, uint8_t *output);
