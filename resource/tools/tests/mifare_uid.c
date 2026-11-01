#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "app_status.h"
#define RC522_H
#define __DELAY_H__
#define DEF_FIFO_LENGTH 64
#define PCD_TRANSCEIVE 12
typedef struct {
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t sak;
    uint8_t atqa[2];
} picc_14a_tag_t;
typedef struct {
    int8_t forcebcc;
    int8_t forcecl2;
    int8_t forcecl3;
    int8_t forcerats;
} hf14a_config_t;
static picc_14a_tag_t selected;
static hf14a_config_t config;
static uint8_t block0[16];
static uint8_t replacement[7];
static uint8_t method;
static unsigned writes;
static unsigned scans;
static unsigned fail_at;
static uint16_t timeout_ms;
static bool field;
static void pcd_14a_reader_antenna_off(void) {
    field = false;
}

static void pcd_14a_reader_antenna_on(void) {
    field = true;
}

static void pcd_14a_reader_mf1_unauth(void) {}
static void pcd_14a_reader_reset(void) {
    assert(!field);
    timeout_ms = 25;
}

static void bsp_delay_ms(uint16_t ms) {
    assert(ms == 5 || ms == 8);
}

static uint16_t pcd_14a_reader_timeout_get(void) {
    return timeout_ms;
}

static void pcd_14a_reader_timeout_set(uint16_t ms) {
    timeout_ms = ms;
}

static hf14a_config_t *get_hf14a_config(void) {
    return &config;
}

static void set_hf14a_config(const hf14a_config_t *value) {
    config = *value;
}

static uint16_t pcd_14a_reader_scan_auto(picc_14a_tag_t *tag) {
    assert(field && config.forcerats == 2);
    *tag = selected;
    if (++scans == fail_at) {
        if (fail_at == 1) {
            tag->uid[0] ^= 1;
        } else {
            tag->atqa[0] ^= 1;
        }
    }
    return 0;
}
static void pcd_14a_reader_halt_tag(void) {
    assert(field);
}

static uint16_t pcd_14a_reader_bits_transfer(uint8_t *tx, uint16_t bits, uint8_t *parity,
        uint8_t *rx, uint8_t *rx_parity, uint16_t *received, uint16_t capacity) {
    assert(field && bits == 7 && !parity && !rx_parity && capacity == 8);
    assert(*tx == (method == 6 ? 0x20 : 0x40));
    *rx = 10;
    *received = 4;
    return 0;
}
static uint16_t pcd_14a_reader_bytes_transfer(uint8_t command, uint8_t *tx, uint8_t length,
        uint8_t *rx, uint16_t *received, uint16_t capacity) {
    assert(field && command == PCD_TRANSCEIVE && length == 1 && capacity == 8);
    assert(*tx == (method == 6 ? 0x23 : 0x43));
    *rx = 10;
    *received = 4;
    return 0;
}
static uint16_t pcd_14a_reader_mf1_read(uint8_t block, uint8_t *out) {
    assert(field && block == 0);
    memcpy(out, block0, 16);
    return 0;
}
static uint16_t pcd_14a_reader_mf1_write(uint8_t block, const uint8_t *data) {
    assert(field && block == 0 && !memcmp(data, replacement, 4));
    assert(data[4] == (data[0] ^ data[1] ^ data[2] ^ data[3]));
    assert(!memcmp(data + 5, block0 + 5, 11));
    memcpy(block0, data, 16);
    memcpy(selected.uid, replacement, selected.uid_len);
    writes++;
    return 0;
}
static uint16_t pcd_14a_reader_raw_cmd(bool open, bool wait, bool crc, bool select,
        bool keep, bool check, uint16_t ms, uint16_t bits, uint8_t *tx,
        uint8_t *rx, uint16_t *received, uint16_t capacity) {
    assert(field && !open && wait && crc && !select && keep && check && ms == 100 && capacity == 512);
    unsigned length = bits / 8;
    if (method == 3 && tx[0] == 0x30) {
        assert(length == 2 && tx[1] == 0);
        memcpy(rx, block0, 16);
        *received = 16;
        return 0;
    }
    if (method == 4 && tx[5] == 0xce) {
        assert(length == 7 && tx[0] == 0xcf && tx[6] == 0);
        assert(!memcmp(tx + 1, (const uint8_t[]){1, 2, 3, 4}, 4));
        memcpy(rx, block0, 16);
        *received = 16;
        return 0;
    }
    if (method == 9 && tx[4] == 0xb0) {
        assert(length == 7 && !memcmp(tx, (const uint8_t[]){10, 0, 0, 0xa6, 0xb0, 0, 16}, 7));
        memcpy(rx, (const uint8_t[]){10, 0, 0x90, 0}, 4);
        *received = 4;
        return 0;
    }
    if (method == 3) {
        assert(length == 5u + selected.uid_len && !memcmp(tx, (const uint8_t[]){0x90, 0xfb, 0xcc, 0xcc}, 4));
        assert(tx[4] == selected.uid_len && !memcmp(tx + 5, replacement, selected.uid_len));
    } else if (method == 4) {
        assert(length == 23 && tx[5] == 0xcd && !memcmp(tx + 7, replacement, 4));
        assert(!memcmp(tx + 12, block0 + 5, 11));
    } else {
        assert(method == 9 && length == 12 && tx[4] == 0xa0 && !memcmp(tx + 7, replacement, 4) && tx[11] == 0);
    }
    memcpy(selected.uid, replacement, selected.uid_len);
    writes++;
    memcpy(rx, (const uint8_t[]){0x90, 0}, 2);
    *received = 2;
    if (method == 9) {
        memcpy(rx, (const uint8_t[]){10, 0, 0x90, 0}, 4);
        *received = 4;
    }
    return 0;
}
#include "mf1_uid.c"
int main(void) {
    for (unsigned choice = 1; choice <= 9; choice++) {
        if (choice == 2 || choice == 5) {
            continue;
        }
        for (unsigned failure = 0; failure <= 2; failure++) {
            method = choice;
            selected = (picc_14a_tag_t){{1, 2, 3, 4}, 4, 8, {4, 0}};
            memset(block0, 0xa5, 16);
            memcpy(block0, selected.uid, 4);
            memcpy(replacement, (const uint8_t[]){5, 6, 7, 8}, 4);
            uint8_t request[21] = {choice, 4};
            memcpy(request + 2, selected.uid, 4);
            memcpy(request + 9, replacement, 4);
            if (choice == 4) {
                memcpy(request + 16, (const uint8_t[]){1, 2, 3, 4}, 4);
            }
            config.forcerats = 1;
            timeout_ms = 77;
            scans = 0;
            writes = 0;
            fail_at = 0;
            uint8_t output[8];
            uint16_t size;
            request[20] = 1;
            assert(mf1_uid_special(21, request, output, &size) == 0);
            assert(size == 5 && !memcmp(output + 1, request + 2, 4) && writes == 0);
            assert(!field && config.forcerats == 1 && timeout_ms == 77);
            request[20] = 0;
            scans = 0;
            fail_at = failure;
            uint16_t status = mf1_uid_special(21, request, output, &size);
            assert((status == 0) == (failure == 0));
            assert(writes == (failure == 1 ? 0u : 1u) && !field && config.forcerats == 1 && timeout_ms == 77);
            if (status == 0) {
                assert(size == 5 && !memcmp(output + 1, replacement, 4));
            } else {
                assert(size == 0);
            }
        }
    }
    puts("Magic UID: preflight, preserved block tail, wire frames, verification and RF cleanup passed");
}
