#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "reply.h"

#define NFCV_MEMORY_MAX_BLOCKS 32u
#define NFCV_BLOCK_SIZE 4u
#define NFCV_MEMORY_MAX_BYTES (NFCV_MEMORY_MAX_BLOCKS * NFCV_BLOCK_SIZE)

typedef struct {
    uint8_t block_count;
    uint8_t memory[NFCV_MEMORY_MAX_BYTES];
    uint8_t locks[NFCV_MEMORY_MAX_BLOCKS];
} nfcv_mem_t;

/* Called only with a validated, complete four-byte-block image. */
void nfcv_mem_init(nfcv_mem_t *store, uint8_t count, const uint8_t *data, const uint8_t *locks);

void nfcv_mem_read(const nfcv_mem_t *store, nfcv_reply_t *response, bool option,
                   uint8_t first_block, uint8_t block_count);
void nfcv_mem_write(nfcv_mem_t *store, nfcv_reply_t *response, const uint8_t *params,
                    uint8_t param_len);
void nfcv_mem_lock(nfcv_mem_t *store, nfcv_reply_t *response, const uint8_t *params,
                   uint8_t param_len);
void nfcv_mem_security(const nfcv_mem_t *store, nfcv_reply_t *response, const uint8_t *params,
                       uint8_t param_len);
