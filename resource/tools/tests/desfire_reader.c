#include "app_status.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum { DEVICE_MODE_READER = 1 };
typedef struct {
    uint8_t uid[10], uid_len, atqa[2], sak, ats[32], ats_len;
} picc_14a_tag_t;
typedef struct {
    uint8_t tx[64], rx[64], tx_size, rx_size, status;
} exchange_t;
static exchange_t exchanges[40];
static unsigned exchange_count, exchange_index, opens, closes, finishes, polls, device_mode = 1;
static bool session_ready, expire;
static uint8_t scan_status;
static bool raw_retained;
static bool release_ok = true;
static unsigned resets;
static unsigned scans;
static picc_14a_tag_t scanned = {
    .uid = {1, 2, 3, 4}, .uid_len = 4, .atqa = {0x44, 3}, .sak = 0x20, .ats = {2, 0}, .ats_len = 2};

void desfire_reader_invalidate(void);

static void nfca_session_open(void) {
    opens++;
    session_ready = true;
}

static void nfca_session_close(void) {
    closes++;
    session_ready = false;
    desfire_reader_invalidate();
}

static bool nfca_session_finish(void) {
    finishes++;
    return true;
}

static bool nfca_session_ready(void) {
    return session_ready;
}

static void nfca_session_poll(void) {
    polls++;
    if (expire)
        nfca_session_close();
}

static unsigned get_device_mode(void) {
    return device_mode;
}

bool hf_radio_release(void) {
    raw_retained = false;
    return release_ok;
}

static void pcd_14a_reader_reset(void) {
    assert(!raw_retained);
    resets++;
}

static void pcd_14a_reader_antenna_on(void) {
}

static void bsp_delay_ms(unsigned ms) {
    assert(ms == 8);
}

static uint8_t pcd_14a_reader_scan_auto(picc_14a_tag_t *tag) {
    scans++;
    *tag = scanned;
    return scan_status;
}

static uint8_t pcd_14a_reader_raw_cmd(bool field, bool wait, bool crc, bool select, bool keep, bool check,
                                      uint16_t timeout, uint16_t bits, uint8_t *tx, uint8_t *rx, uint16_t *count,
                                      uint16_t capacity) {
    assert(!field && wait && crc && !select && keep && check && timeout == 200 && capacity == 512);
    assert(exchange_index < exchange_count);
    exchange_t *entry = &exchanges[exchange_index++];
    assert(bits == entry->tx_size * 8 && !memcmp(tx, entry->tx, entry->tx_size));
    memcpy(rx, entry->rx, entry->rx_size);
    *count = entry->rx_size;
    return entry->status;
}

/* IMPLEMENTATION */

static void queue(const uint8_t *tx, unsigned tx_size, const uint8_t *rx, unsigned rx_size, uint8_t status) {
    assert(exchange_count < sizeof(exchanges) / sizeof(exchanges[0]));
    exchange_t *entry = &exchanges[exchange_count++];
    assert(tx_size <= sizeof(entry->tx) && rx_size <= sizeof(entry->rx));
    memcpy(entry->tx, tx, tx_size);
    memcpy(entry->rx, rx, rx_size);
    entry->tx_size = tx_size;
    entry->rx_size = rx_size;
    entry->status = status;
}

static void begin(void) {
    exchange_count = exchange_index = 0;
    opens = closes = finishes = polls = 0;
    expire = false;
    scan_status = 0;
    device_mode = 1;
    scanned.sak = 0x20;
    scanned.ats_len = 2;
    uint8_t command[] = {1}, output[512];
    uint16_t length;
    assert(desfire_reader_command(1, command, output, &length) == STATUS_HF_TAG_OK);
    assert(opens == 1 && finishes == 1 && !closes && length == 11);
    assert(output[0] == 4 && !memcmp(output + 1, scanned.uid, 4));
}

static void assert_failed(const uint8_t *command, unsigned size, uint16_t expected) {
    uint8_t output[512];
    uint16_t length = 0xffff;
    assert(desfire_reader_command(size, command, output, &length) == expected);
    assert(!length && !session_ready && closes);
    assert(exchange_index == exchange_count);
}

static void test_native_and_iso_chaining(void) {
    begin();
    const uint8_t version[] = {2, 0x60}, first[] = {2, 0xaf, 1, 2};
    const uint8_t more[] = {3, 0xaf}, last[] = {3, 0, 3};
    queue(version, sizeof(version), first, sizeof(first), 0);
    queue(more, sizeof(more), last, sizeof(last), 0);
    uint8_t command[] = {2, 0x60}, output[512];
    uint16_t length;
    assert(desfire_reader_command(2, command, output, &length) == STATUS_HF_TAG_OK);
    const uint8_t expected[] = {0, 1, 2, 3};
    assert(length == sizeof(expected) && !memcmp(output, expected, sizeof(expected)));
    assert(exchange_index == 2 && finishes == 2 && !closes);
    begin();
    const uint8_t chained[] = {0x12, 0, 11}, ack[] = {0xa3}, tail[] = {3, 22};
    queue(version, sizeof(version), chained, sizeof(chained), 0);
    queue(ack, sizeof(ack), tail, sizeof(tail), 0);
    assert(desfire_reader_command(2, command, output, &length) == STATUS_HF_TAG_OK);
    const uint8_t joined[] = {0, 11, 22};
    assert(length == 3 && !memcmp(output, joined, 3) && exchange_index == 2);
    begin();
    const uint8_t chained_af[] = {0x12, 0xaf, 1};
    const uint8_t chained_tail[] = {3, 2}, native_more[] = {2, 0xaf}, native_tail[] = {2, 0, 3};
    queue(version, sizeof(version), chained_af, sizeof(chained_af), 0);
    queue(ack, sizeof(ack), chained_tail, sizeof(chained_tail), 0);
    queue(native_more, sizeof(native_more), native_tail, sizeof(native_tail), 0);
    assert(desfire_reader_command(2, command, output, &length) == STATUS_HF_TAG_OK);
    assert(length == sizeof(expected) && !memcmp(output, expected, sizeof(expected)) && exchange_index == 3);
    begin();
    const uint8_t wtx[] = {0xf2, 59}, normal[] = {2, 0, 7};
    queue(version, sizeof(version), wtx, sizeof(wtx), 0);
    queue(wtx, sizeof(wtx), normal, sizeof(normal), 0);
    assert(desfire_reader_command(2, command, output, &length) == STATUS_HF_TAG_OK);
    assert(length == 2 && output[0] == 0 && output[1] == 7 && exchange_index == 2);
}

static void test_failures_and_limits(void) {
    const uint8_t command[] = {2, 0x60}, tx[] = {2, 0x60};
    const uint8_t malformed[][3] = {{3, 0}, {0xa2, 0}, {2}, {0xf2, 60}, {0xf2, 0}, {0x0a, 0}, {6, 0}};
    const unsigned sizes[] = {2, 2, 1, 2, 2, 2, 2};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        begin();
        queue(tx, sizeof(tx), malformed[i], sizes[i], 0);
        assert_failed(command, sizeof(command), STATUS_HF_ERR_STAT);
    }
    begin();
    const uint8_t empty[] = {0};
    queue(tx, sizeof(tx), empty, 0, STATUS_HF_ERR_CRC);
    assert_failed(command, sizeof(command), STATUS_HF_ERR_CRC);
    begin();
    const uint8_t wtx[] = {0xf2, 5};
    for (unsigned i = 0; i < 32; i++)
        queue(i ? wtx : tx, 2, wtx, 2, 0);
    assert_failed(command, sizeof(command), STATUS_HF_ERR_STAT);
    begin();
    const uint8_t af0[] = {2, 0xaf}, af1[] = {3, 0xaf};
    for (unsigned i = 0; i < 32; i++) {
        const uint8_t next[] = {(uint8_t)(2 | (i & 1)), 0xaf};
        queue(i ? next : tx, 2, i & 1 ? af1 : af0, 2, 0);
    }
    assert_failed(command, sizeof(command), STATUS_HF_ERR_STAT);
    begin();
    uint8_t block[64] = {0x12, 0};
    for (unsigned i = 0; i < 9; i++) {
        const uint8_t ack[] = {(uint8_t)(0xa2 | (i & 1))};
        block[0] = 0x12 | (i & 1);
        queue(i ? ack : tx, i ? 1 : 2, block, sizeof(block), 0);
    }
    assert_failed(command, sizeof(command), STATUS_HF_ERR_STAT);
    begin();
    for (unsigned i = 0; i < 9; i++) {
        const uint8_t next[] = {(uint8_t)(2 | (i & 1)), 0xaf};
        block[0] = 2 | (i & 1);
        block[1] = 0xaf;
        queue(i ? next : tx, 2, block, sizeof(block), 0);
    }
    assert_failed(command, sizeof(command), STATUS_HF_ERR_STAT);
    begin();
    expire = true;
    assert_failed(command, sizeof(command), STATUS_PAR_ERR);
    begin();
    const uint8_t forbidden[] = {2, 0x71};
    assert_failed(forbidden, sizeof(forbidden), STATUS_PAR_ERR);
    begin();
    const uint8_t close[] = {0};
    assert_failed(close, sizeof(close), STATUS_SUCCESS);
}

static void test_identity_and_recovery(void) {
    begin();
    const uint8_t wrong_uid[] = {1, 4, 9, 9, 9, 9};
    assert_failed(wrong_uid, sizeof(wrong_uid), STATUS_HF_ERR_ATS);
    begin();
    scanned.ats_len = 1;
    const uint8_t start[] = {1};
    assert_failed(start, sizeof(start), STATUS_HF_ERR_ATS);
    begin();
    scanned.sak = 0;
    assert_failed(start, sizeof(start), STATUS_HF_ERR_ATS);
    begin();
    scan_status = STATUS_HF_TAG_NO;
    assert_failed(start, sizeof(start), STATUS_HF_TAG_NO);
    begin(); /* Fresh activation still works after every rejected session. */
    const uint8_t same_uid[] = {1, 4, 1, 2, 3, 4};
    uint8_t output[512];
    uint16_t length;
    assert(desfire_reader_command(sizeof(same_uid), same_uid, output, &length) == STATUS_HF_TAG_OK);
    device_mode = 0;
    const uint8_t command[] = {2, 0x60};
    assert(desfire_reader_command(sizeof(command), command, output, &length) == STATUS_DEVICE_MODE_ERROR);
    assert(!length && !exchange_index);
}

static void test_retained_frontend(void) {
    begin();
    const uint8_t stop[] = {0};
    uint8_t output[512];
    uint16_t length;
    assert(desfire_reader_command(sizeof(stop), stop, output, &length) == STATUS_SUCCESS);
    raw_retained = true;
    const uint8_t start[] = {1};
    assert(desfire_reader_command(sizeof(start), start, output, &length) == STATUS_HF_TAG_OK);
    assert(!raw_retained && length == 11);
    assert(output[0] == scanned.uid_len && !memcmp(output + 1, scanned.uid, scanned.uid_len));

    assert(desfire_reader_command(sizeof(stop), stop, output, &length) == STATUS_SUCCESS);
    raw_retained = true;
    release_ok = false;
    unsigned previous_opens = opens;
    unsigned previous_resets = resets;
    unsigned previous_scans = scans;
    length = 0xffff;
    assert(desfire_reader_command(sizeof(start), start, output, &length) == STATUS_HF_ERR_STAT);
    assert(!length && !session_ready);
    assert(opens == previous_opens && resets == previous_resets && scans == previous_scans);

    release_ok = true;
    assert(desfire_reader_command(sizeof(start), start, output, &length) == STATUS_HF_TAG_OK);
    assert(length == 11);
}

int main(void) {
    test_native_and_iso_chaining();
    test_failures_and_limits();
    test_identity_and_recovery();
    test_retained_frontend();
    return 0;
}
