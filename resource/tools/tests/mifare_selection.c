#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "app_status.h"
#include "nrf.h"
test_dwt_t test_dwt;
test_debug_t test_debug;
uint32_t SystemCoreClock = 64000000;

#define NRF_LOG_INFO(...) ((void)0)
#define DEF_FIFO_LENGTH 64
#define PCD_TRANSCEIVE 12
#define PICC_ANTICOLL1 0x93
#define U8ARR_BIT_LEN(value) (sizeof(value) * 8)

typedef struct {
    uint8_t uid[10], uid_len, sak, atqa[2], cascade, ats[255], ats_len;
} picc_14a_tag_t;
typedef struct { int8_t forcebcc, forcecl2, forcecl3, forcerats; } hf14a_config_t;
static hf14a_config_t hf14aconfig;
static const uint8_t uid[10] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x12, 0x34, 0x56};
static uint16_t anti_bits, sak_bits;
static unsigned cascades, anti_calls, select_calls;
static bool nak, bad_bcc;

static uint8_t atqa_request(uint8_t *out, uint16_t capacity) {
    assert(capacity == 16);
    out[0] = 4; out[1] = 0;
    return STATUS_HF_TAG_OK;
}

static void crc_14a_append(uint8_t *bytes, size_t size) {
    assert(size == 7);
    bytes[size] = 0; bytes[size + 1] = 0;
}

static uint8_t pcd_14a_reader_ats_request(uint8_t *out, uint16_t *size, size_t capacity) {
    (void)out; (void)size; (void)capacity;
    assert(false && "Classic selection must not send RATS");
    return STATUS_HF_ERR_STAT;
}

static uint8_t pcd_14a_reader_bytes_transfer(uint8_t operation, uint8_t *in, size_t size,
        uint8_t *out, uint16_t *bits, size_t capacity) {
    assert(operation == PCD_TRANSCEIVE && capacity == DEF_FIFO_LENGTH * 8);
    unsigned level = (in[0] - PICC_ANTICOLL1) / 2;
    assert(level < 4);
    if (in[1] == 0x20) {
        assert(size == 2);
        anti_calls++;
        if (nak) {
            out[0] = 4;
        } else if (level < cascades - 1) {
            out[0] = 0x88;
            memcpy(out + 1, uid + level * 3, 3);
            out[4] = out[0] ^ out[1] ^ out[2] ^ out[3];
        } else {
            /* A fourth cascade peer is malformed, not a longer UID. */
            memcpy(out, uid + (level < 3 ? level * 3 : 0), 4);
            out[4] = out[0] ^ out[1] ^ out[2] ^ out[3];
        }
        if (bad_bcc) out[4] ^= 0x80;
        *bits = anti_bits;
    } else {
        assert(in[1] == 0x70 && size == 9);
        select_calls++;
        if (hf14aconfig.forcebcc == 1) assert(in[6] == (in[2] ^ in[3] ^ in[4] ^ in[5]));
        out[0] = level < cascades - 1 ? 4 : 8;
        out[1] = 0; out[2] = 0;
        *bits = sak_bits;
    }
    return STATUS_HF_TAG_OK;
}

/* SELECTION FUNCTION */

static void reset(void) {
    hf14aconfig = (hf14a_config_t){0, 0, 0, 2};
    anti_bits = 40; sak_bits = 24; cascades = 1;
    anti_calls = select_calls = 0;
    nak = bad_bcc = false;
}

int main(void) {
    picc_14a_tag_t tag;
    for (unsigned levels = 1; levels <= 3; levels++) {
        reset(); cascades = levels;
        assert(pcd_14a_reader_scan_once(&tag) == STATUS_HF_TAG_OK);
        assert(tag.uid_len == 3 * levels + 1 && tag.cascade == levels && tag.sak == 8);
        assert(!memcmp(tag.uid, uid, tag.uid_len));
        assert(anti_calls == levels && select_calls == levels);
    }
    /* A four-bit NAK is a framing failure, never a bad UID checksum. */
    reset(); nak = true; anti_bits = 4;
    assert(pcd_14a_reader_scan_once(&tag) == STATUS_HF_ERR_STAT);
    assert(select_calls == 0 && tag.uid_len == 0);
    /* Even plausible UID bytes cannot validate a response of the wrong length.
     * An explicit BCC repair override must not bypass frame-length checks. */
    const uint16_t invalid_bits[] = {0, 4, 8, 16, 32, 39, 41, 48, 512};
    for (unsigned force = 0; force <= 2; force++) {
        for (unsigned i = 0; i < sizeof(invalid_bits) / sizeof(invalid_bits[0]); i++) {
            reset(); anti_bits = invalid_bits[i]; hf14aconfig.forcebcc = force;
            assert(pcd_14a_reader_scan_once(&tag) == STATUS_HF_ERR_STAT);
            assert(select_calls == 0 && tag.uid_len == 0);
        }
    }
    reset(); bad_bcc = true;
    assert(pcd_14a_reader_scan_once(&tag) == STATUS_HF_ERR_BCC);
    assert(select_calls == 0);
    reset(); bad_bcc = true; hf14aconfig.forcebcc = 1;
    assert(pcd_14a_reader_scan_once(&tag) == STATUS_HF_TAG_OK);
    reset(); sak_bits = 4;
    assert(pcd_14a_reader_scan_once(&tag) == STATUS_HF_ERR_STAT);
    reset(); cascades = 4;
    assert(pcd_14a_reader_scan_once(&tag) == STATUS_HF_ERR_STAT);
    assert(anti_calls == 3 && select_calls == 3);
    return 0;
}
