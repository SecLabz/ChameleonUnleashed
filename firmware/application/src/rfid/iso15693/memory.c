#include "memory.h"

#include <string.h>


void nfcv_mem_init(nfcv_mem_t *store, uint8_t count, const uint8_t *data, const uint8_t *locks) {
    store->block_count = count;
    memcpy(store->memory, data, count * NFCV_BLOCK_SIZE);
    memcpy(store->locks, locks, count);
}

void nfcv_mem_read(const nfcv_mem_t *store, nfcv_reply_t *response, bool option,
                   uint8_t first_block, uint8_t block_count) {
    if (first_block >= store->block_count || block_count == 0u ||
            first_block + block_count > store->block_count) {
        nfcv_reply_error(response, ISO15693_ERR_BLOCK_UNAVAILABLE);
        return;
    }

    /* Leave room for the PHY's two CRC bytes before copying any block data. */
    unsigned size = 1u + block_count * (NFCV_BLOCK_SIZE + (option ? 1u : 0u));
    if (size > sizeof(response->bytes) - ISO15693_CRC_SIZE) {
        nfcv_reply_error(response, ISO15693_ERR_GENERIC);
        return;
    }

    uint16_t out = 1u;
    response->bytes[0] = 0x00u;
    for (uint8_t block = 0u; block < block_count; block++) {
        const uint8_t block_index = (uint8_t)(first_block + block);
        if (option) {
            response->bytes[out++] = store->locks[block_index];
        }
        const uint16_t memory_index = (uint16_t)block_index * NFCV_BLOCK_SIZE;
        memcpy(&response->bytes[out], &store->memory[memory_index], NFCV_BLOCK_SIZE);
        out = (uint16_t)(out + NFCV_BLOCK_SIZE);
    }
    response->len = out;
}

void nfcv_mem_write(nfcv_mem_t *store, nfcv_reply_t *response, const uint8_t *params,
                    uint8_t param_len) {
    if (param_len != (uint8_t)(1u + NFCV_BLOCK_SIZE)) {
        nfcv_reply_silent(response);
        return;
    }
    const uint8_t block = params[0];
    if (block >= store->block_count) {
        nfcv_reply_error(response, ISO15693_ERR_BLOCK_UNAVAILABLE);
        return;
    }
    if (store->locks[block] != 0u) {
        nfcv_reply_error(response, ISO15693_ERR_BLOCK_LOCKED);
        return;
    }
    memcpy(&store->memory[(uint16_t)block * NFCV_BLOCK_SIZE], &params[1], NFCV_BLOCK_SIZE);
    nfcv_reply_ok(response);
}

void nfcv_mem_lock(nfcv_mem_t *store, nfcv_reply_t *response, const uint8_t *params,
                   uint8_t param_len) {
    if (param_len != 1u) {
        nfcv_reply_silent(response);
        return;
    }
    const uint8_t block = params[0];
    if (block >= store->block_count) {
        nfcv_reply_error(response, ISO15693_ERR_BLOCK_UNAVAILABLE);
        return;
    }
    if (store->locks[block] != 0u) {
        nfcv_reply_error(response, ISO15693_ERR_BLOCK_ALREADY_LOCKED);
        return;
    }
    store->locks[block] = 0x01u;
    nfcv_reply_ok(response);
}

void nfcv_mem_security(const nfcv_mem_t *store, nfcv_reply_t *response, const uint8_t *params,
                       uint8_t param_len) {
    if (param_len != 2u) {
        nfcv_reply_silent(response);
        return;
    }
    const uint8_t first_block = params[0];
    const uint8_t count = (uint8_t)(params[1] + 1u);
    if (!count || first_block >= store->block_count || first_block + count > store->block_count) {
        nfcv_reply_error(response, ISO15693_ERR_BLOCK_UNAVAILABLE);
        return;
    }
    response->bytes[0] = 0x00u;
    memcpy(&response->bytes[1], &store->locks[first_block], count);
    response->len = (uint16_t)(1u + count);
}
