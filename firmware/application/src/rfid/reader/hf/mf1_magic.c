#include "mf1_magic.h"
#include "mf1_toolbox.h"
#include "rc522.h"
#include "app_status.h"
#include "bsp_delay.h"
#include <stdbool.h>
#include <string.h>

#define MAGIC_TIMEOUT_MS 5u
#define MAGIC_FIELD_OFF_MS 5u
#define MAGIC_FIELD_ON_MS 8u

typedef struct {
    picc_14a_tag_t expected;
    uint16_t flags;
    uint8_t gen2;
} magic_probe_t;

static void reset_field(void) {
    pcd_14a_reader_antenna_off();
    pcd_14a_reader_mf1_unauth();
    bsp_delay_ms(MAGIC_FIELD_OFF_MS);
    pcd_14a_reader_reset();
    pcd_14a_reader_antenna_on();
    bsp_delay_ms(MAGIC_FIELD_ON_MS);
}

static uint16_t restart(magic_probe_t *probe, bool halted) {
    reset_field();
    picc_14a_tag_t tag;
    uint16_t status = pcd_14a_reader_scan_auto(&tag);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    const picc_14a_tag_t *expected = &probe->expected;
    if (tag.uid_len != expected->uid_len || memcmp(tag.uid, expected->uid, tag.uid_len) ||
        memcmp(tag.atqa, expected->atqa, sizeof(tag.atqa)) || tag.sak != expected->sak) {
        return STATUS_HF_TAG_NO;
    }
    // Hardware authentication derives its four UID bytes from the selection cascade.
    probe->expected.cascade = tag.cascade;
    pcd_14a_reader_timeout_set(MAGIC_TIMEOUT_MS);
    if (halted) {
        pcd_14a_reader_halt_tag();
    }
    return STATUS_HF_TAG_OK;
}

static bool acknowledged(uint16_t status, uint16_t bits, const uint8_t *response) {
    return status == STATUS_HF_TAG_OK && bits == 4 && (response[0] & 0x0f) == 0x0a;
}

static bool wakeup(uint8_t command, bool short_frame) {
    uint8_t response = 0;
    uint16_t bits = 0;
    uint16_t status;
    if (short_frame) {
        status = pcd_14a_reader_bits_transfer(&command, 7, NULL, &response, NULL, &bits, 8);
    } else {
        status = pcd_14a_reader_bytes_transfer(PCD_TRANSCEIVE, &command, 1, &response, &bits, 8);
    }
    return acknowledged(status, bits, &response);
}

static uint16_t exchange(const uint8_t *command, uint8_t length, uint8_t *response, uint16_t *bits) {
    uint8_t frame[9];
    memcpy(frame, command, length);
    crc_14a_append(frame, length);
    *bits = 0;
    return pcd_14a_reader_bytes_transfer(PCD_TRANSCEIVE, frame, length + 2,
                                         response, bits, DEF_FIFO_LENGTH * 8);
}

static bool valid_crc(uint8_t *response, uint16_t bits, uint16_t bytes) {
    if (bits != bytes * 8) {
        return false;
    }
    uint8_t crc[2];
    crc_14a_calculate(response, bytes - 2, crc);
    return memcmp(crc, response + bytes - 2, sizeof(crc)) == 0;
}

static uint16_t gen1_restart(magic_probe_t *probe, bool cold) {
    // Check the target before probing, including the direct power-on wakeup.
    uint16_t status = restart(probe, !cold);
    if (status == STATUS_HF_TAG_OK && cold) {
        // Some Gen1A cards only accept 40/7 in their power-on state. SELECT
        // followed by HALT is not equivalent to a fresh field on these cards.
        reset_field();
        pcd_14a_reader_timeout_set(MAGIC_TIMEOUT_MS);
    }
    return status;
}

static uint16_t gen1(magic_probe_t *probe) {
    uint16_t status = gen1_restart(probe, true);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    bool cold = true;
    if (!wakeup(0x40, true)) {
        cold = false;
        status = gen1_restart(probe, cold);
        if (status != STATUS_HF_TAG_OK || !wakeup(0x40, true)) {
            return status;
        }
    }
    bool second_ack = wakeup(0x43, false);
    probe->flags |= second_ack ? MF1_MAGIC_GEN1A : MF1_MAGIC_GEN1B;
    if (!second_ack) {
        status = gen1_restart(probe, cold);
        if (status != STATUS_HF_TAG_OK || !wakeup(0x40, true)) {
            return status;
        }
    }
    const uint8_t config[] = {0xe0, 0};
    uint8_t response[DEF_FIFO_LENGTH];
    uint16_t bits;
    status = exchange(config, sizeof(config), response, &bits);
    if (status != STATUS_HF_TAG_OK || !valid_crc(response, bits, 18)) {
        return STATUS_HF_TAG_OK;
    }
    const uint8_t hidden[] = {0x38, 0};
    status = exchange(hidden, sizeof(hidden), response, &bits);
    probe->flags |= status == STATUS_HF_TAG_OK && valid_crc(response, bits, 18)
        ? MF1_MAGIC_GDM_40 : MF1_MAGIC_GDM_ZUID;
    return STATUS_HF_TAG_OK;
}

static uint16_t gen2_authenticated(magic_probe_t *probe, uint8_t credential, uint8_t *key) {
    uint16_t status = restart(probe, false);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    status = pcd_14a_reader_mf1_auth(&probe->expected, credential == 2 ? 0x61 : 0x60, 0, key);
    probe->gen2 = MF1_MAGIC_GEN2_AUTH_DENIED;
    if (status != STATUS_HF_TAG_OK) {
        return STATUS_HF_TAG_OK;
    }
    uint8_t response[DEF_FIFO_LENGTH];
    uint16_t bits;
    const uint8_t write[] = {PICC_WRITE, 0};
    status = exchange(write, sizeof(write), response, &bits);
    /* Cut RF immediately after the WRITE command reply. Never send its data
     * phase, HALT or another command while the tag may be awaiting data. */
    pcd_14a_reader_antenna_off();
    pcd_14a_reader_mf1_unauth();
    probe->gen2 = MF1_MAGIC_GEN2_INCONCLUSIVE;
    if (acknowledged(status, bits, response)) {
        probe->flags |= MF1_MAGIC_GEN2;
        probe->gen2 = MF1_MAGIC_GEN2_WRITE_ACK;
    } else if (status == STATUS_HF_TAG_OK && bits == 4 &&
               (response[0] == 0 || response[0] == 1 || response[0] == 4 || response[0] == 5)) {
        probe->gen2 = MF1_MAGIC_GEN2_WRITE_DENIED;
    }
    return STATUS_HF_TAG_OK;
}

static uint16_t gen2(magic_probe_t *probe, uint8_t credential, uint8_t *key) {
    uint16_t status = restart(probe, false);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    const uint8_t rats[] = {0xe0, 0x20};
    static const uint8_t ats4[] = {9, 0x78, 0, 0x91, 2, 0xda, 0xbc, 0x19, 0x10};
    static const uint8_t ats7[] = {13, 0x78, 0, 0x71, 2, 0x88, 0x49, 0xa1, 0x30, 0x20, 0x15, 6, 8};
    uint8_t response[DEF_FIFO_LENGTH];
    uint16_t bits;
    status = exchange(rats, sizeof(rats), response, &bits);
    if (status == STATUS_HF_TAG_OK &&
        ((valid_crc(response, bits, sizeof(ats4) + 2) && !memcmp(response, ats4, sizeof(ats4))) ||
         (valid_crc(response, bits, sizeof(ats7) + 2) && !memcmp(response, ats7, sizeof(ats7))))) {
        probe->flags |= MF1_MAGIC_GEN2;
        probe->gen2 = MF1_MAGIC_GEN2_ATS;
        return STATUS_HF_TAG_OK;
    }
    if (!credential) {
        return STATUS_HF_TAG_OK;
    }
    return gen2_authenticated(probe, credential, key);
}

static uint16_t signatures(magic_probe_t *probe) {
    static const uint8_t commands[][7] = {
        {0xcf, 0, 0, 0, 0, 0xc6},
        {PICC_READ, 0},
        {0x80, 0},
        {0x0a, 0, 0, 0xa6, 0xb0, 0, 0x10},
    };
    static const uint8_t lengths[] = {6, 2, 2, 7};
    static const uint16_t flags[] = {MF1_MAGIC_GTU, MF1_MAGIC_GEN3, MF1_MAGIC_GDM_AUTH, MF1_MAGIC_SUPER_GEN1};
    for (unsigned i = 0; i < sizeof(lengths); i++) {
        uint16_t status = restart(probe, false);
        if (status != STATUS_HF_TAG_OK) {
            return status;
        }
        uint8_t response[DEF_FIFO_LENGTH];
        uint16_t bits;
        status = exchange(commands[i], lengths[i], response, &bits);
        if (status != STATUS_HF_TAG_OK) {
            continue;
        }
        bool valid;
        switch (i) {
            case 0:
                valid = valid_crc(response, bits, 32) || valid_crc(response, bits, 34);
                break;
            case 1:
                valid = valid_crc(response, bits, 18);
                break;
            case 2:
                valid = bits == 32;
                break;
            default:
                valid = valid_crc(response, bits, 22);
                break;
        }
        if (valid) {
            probe->flags |= flags[i];
        }
    }
    if ((probe->flags & (MF1_MAGIC_SUPER_GEN1 | MF1_MAGIC_GEN3)) == (MF1_MAGIC_SUPER_GEN1 | MF1_MAGIC_GEN3)) {
        probe->flags &= ~MF1_MAGIC_SUPER_GEN1;
        probe->flags |= MF1_MAGIC_SUPER_GEN2;
    }
    uint16_t status = restart(probe, false);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    uint8_t ql88_key[] = {0x70, 0x7b, 0x11, 0xfc, 0x14, 0x81};
    if (pcd_14a_reader_mf1_auth(&probe->expected, 0x61, 68, ql88_key) == STATUS_HF_TAG_OK) {
        probe->flags |= MF1_MAGIC_QL88;
    }
    status = restart(probe, true);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (wakeup(0x20, true) && wakeup(0x23, false)) {
        const uint8_t config[] = {0xe0, 0};
        uint8_t response[DEF_FIFO_LENGTH];
        uint16_t bits;
        status = exchange(config, sizeof(config), response, &bits);
        if (status == STATUS_HF_TAG_OK && valid_crc(response, bits, 18)) {
            probe->flags |= MF1_MAGIC_GDM_20;
        }
    }
    return restart(probe, false);
}

uint16_t mf1_magic_probe(uint16_t length, const uint8_t *data,
                         uint8_t *output, uint16_t *output_length) {
    *output_length = 0;
    if ((length != 15 && length != 21) || (data[0] != 4 && data[0] != 7 && data[0] != 10) ||
        data[14] > 2 || length != (data[14] ? 21 : 15)) {
        return STATUS_PAR_ERR;
    }
    for (unsigned i = data[0]; i < 10; i++) {
        if (data[1 + i]) {
            return STATUS_PAR_ERR;
        }
    }
    uint8_t sak = data[13] & ~0x20;
    if (sak != 8 && sak != 9 && sak != 0x10 && sak != 0x18) {
        return STATUS_PAR_ERR;
    }
    magic_probe_t probe = {0};
    probe.expected.uid_len = data[0];
    memcpy(probe.expected.uid, data + 1, data[0]);
    memcpy(probe.expected.atqa, data + 11, 2);
    probe.expected.sak = data[13];
    const uint8_t unfused[] = {0xaa, 0x55, 0xc3, 0x96};
    if (!memcmp(probe.expected.uid + data[0] - 4, unfused, sizeof(unfused))) {
        probe.flags |= MF1_MAGIC_UNFUSED;
    }
    uint8_t key[6];
    if (data[14]) {
        memcpy(key, data + 15, sizeof(key));
    }
    uint16_t timeout_ms = pcd_14a_reader_timeout_get();
    hf14a_config_t config = *get_hf14a_config();
    hf14a_config_t selection = config;
    // Probes need the selected Classic state even when SAK advertises ISO-DEP.
    selection.forcerats = 2;
    set_hf14a_config(&selection);
    uint16_t status = gen1(&probe);
    if (status == STATUS_HF_TAG_OK) {
        status = gen2(&probe, data[14], key);
    }
    if (status == STATUS_HF_TAG_OK) {
        status = signatures(&probe);
    }
    pcd_14a_reader_antenna_off();
    pcd_14a_reader_mf1_unauth();
    pcd_14a_reader_timeout_set(timeout_ms);
    set_hf14a_config(&config);
    if (status == STATUS_HF_TAG_OK) {
        output[0] = probe.flags >> 8;
        output[1] = probe.flags;
        output[2] = probe.gen2;
        *output_length = 3;
    }
    return status;
}

uint16_t mf1_classic_inspect(uint16_t length, const uint8_t *data,
                             uint8_t *output, uint16_t *output_length) {
    *output_length = 0;
    if (length < 21 || data[14] == 0 || data[14] > 64 || length != 15 + 6 * data[14]) {
        return STATUS_PAR_ERR;
    }
    uint8_t request[21];
    memcpy(request, data, 14);
    request[14] = 1;
    memcpy(request + 15, data + 15, 6);
    uint8_t magic[3];
    uint16_t magic_length;
    uint16_t status = mf1_magic_probe(sizeof(request), request, magic, &magic_length);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }

    magic_probe_t probe = {0};
    probe.expected.uid_len = data[0];
    memcpy(probe.expected.uid, data + 1, data[0]);
    memcpy(probe.expected.atqa, data + 11, 2);
    probe.expected.sak = data[13];
    probe.flags = ((uint16_t)magic[0] << 8) | magic[1];
    probe.gen2 = magic[2];
    uint8_t credential = 0;
    uint8_t key[6] = {0};
    if (probe.gen2 == MF1_MAGIC_GEN2_WRITE_ACK || probe.gen2 == MF1_MAGIC_GEN2_WRITE_DENIED ||
        probe.gen2 == MF1_MAGIC_GEN2_INCONCLUSIVE) {
        credential = 1;
        memcpy(key, data + 15, sizeof(key));
    }

    uint16_t timeout_ms = pcd_14a_reader_timeout_get();
    hf14a_config_t config = *get_hf14a_config();
    hf14a_config_t selection = config;
    selection.forcerats = 2;
    set_hf14a_config(&selection);
    // Signatures run once; key searches reuse the existing sector-0 verifier.
    for (uint8_t kind = 1; kind <= 2 && status == STATUS_HF_TAG_OK; kind++) {
        if ((probe.flags & ~MF1_MAGIC_UNFUSED) || probe.gen2 == MF1_MAGIC_GEN2_INCONCLUSIVE) {
            break;
        }
        uint8_t first = kind == 1 ? 1 : 0;
        if ((kind == 1 && credential == 1) || first == data[14]) {
            continue;
        }
        status = restart(&probe, false);
        if (status != STATUS_HF_TAG_OK) {
            break;
        }
        mf1_toolbox_check_keys_on_block_in_t in = {
            .block = 0,
            .key_type = kind == 2 ? 0x61 : 0x60,
            .keys_len = data[14] - first,
            .keys = (mf1_key_t *)(data + 15 + 6 * first),
        };
        mf1_toolbox_check_keys_on_block_out_t found;
        status = mf1_toolbox_check_keys_on_block(&in, &found);
        if (status == STATUS_MF_ERR_AUTH) {
            status = STATUS_HF_TAG_OK;
            continue;
        }
        if (status != STATUS_HF_TAG_OK) {
            break;
        }
        uint8_t evidence = probe.gen2;
        status = gen2_authenticated(&probe, kind, found.key.key);
        if (probe.gen2 == MF1_MAGIC_GEN2_AUTH_DENIED) {
            probe.gen2 = evidence;
        } else {
            credential = kind;
            memcpy(key, found.key.key, sizeof(key));
        }
    }

    mf1_prng_type_t prng;
    if (status == STATUS_HF_TAG_OK) {
        status = restart(&probe, false);
    }
    if (status == STATUS_HF_TAG_OK) {
        // Keep the probe timeout for expected HALT silence; PRNG retains its
        // existing RF reset delay. Historical command 2002 is unchanged.
        status = check_prng_type(&prng);
    }
    // PRNG uses its own selection path; bind its observation to the same target.
    if (status == STATUS_HF_TAG_OK) {
        status = restart(&probe, false);
    }
    pcd_14a_reader_antenna_off();
    pcd_14a_reader_mf1_unauth();
    pcd_14a_reader_timeout_set(timeout_ms);
    set_hf14a_config(&config);
    if (status == STATUS_HF_TAG_OK) {
        output[0] = probe.flags >> 8;
        output[1] = probe.flags;
        output[2] = probe.gen2;
        output[3] = prng;
        output[4] = credential;
        memset(output + 5, 0, 6);
        if (credential) {
            memcpy(output + 5, key, sizeof(key));
        }
        *output_length = 11;
    }
    return status;
}
