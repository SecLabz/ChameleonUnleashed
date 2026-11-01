#pragma once
#include "commands.h"

#include <stdbool.h>
#include <stdint.h>

#include "memory.h"

typedef enum {
    NFCV_STANDARD_READY = 0,
    NFCV_STANDARD_SELECTED,
    NFCV_STANDARD_QUIET,
} nfcv_std_state_t;

typedef struct {
    uint8_t command;
    const uint8_t *params;
    uint8_t param_len;
    bool inventory;
    bool addressed;
    bool selected;
    bool option;
} nfcv_request_t;

typedef enum {
    NFCV_STANDARD_UNHANDLED = 0,
    NFCV_STANDARD_NO_RESPONSE,
    NFCV_STANDARD_RESPONSE,
} nfcv_std_result_t;

typedef struct {
    nfcv_std_state_t state;
    uint8_t uid[ISO15693_UID_SIZE];
    uint8_t dsfid;
    uint8_t afi;
    uint8_t ic_ref;
    bool dsfid_locked;
    bool afi_locked;
    nfcv_mem_t *memory;
} nfcv_std_t;

void nfcv_std_init(nfcv_std_t *standard, nfcv_mem_t *memory, const uint8_t uid[ISO15693_UID_SIZE],
                   uint8_t dsfid, uint8_t afi, uint8_t ic_ref, bool dsfid_locked, bool afi_locked);
void nfcv_std_reset_session(nfcv_std_t *standard);
void nfcv_std_quiet(nfcv_std_t *standard);

bool nfcv_std_prepare(const nfcv_std_t *standard, const iso15693_frame_t *frame,
                      nfcv_request_t *request);
nfcv_std_result_t nfcv_std_handle(nfcv_std_t *standard, const nfcv_request_t *request,
                                  nfcv_reply_t *response);
