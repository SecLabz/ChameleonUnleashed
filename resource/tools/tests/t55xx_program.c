#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "lf_t55xx.h"
#include "t55xx_radio.h"
#include "app_status.h"
#include "hex_utils.h"
#include "protocols/wiegand.h"

static uint32_t memory[7];
static unsigned writes;
static unsigned reads;
static uint8_t order[7];
static bool corrupt_write;
static bool broadcast_only;
static uint8_t unreadable_mask;

bool t55xx_radio_detect(t55xx_demod_t *demod) {
    reads++;
    demod->configuration = memory[0];
    demod->modulation = (memory[0] >> 12) & 31;
    return true;
}

bool t55xx_radio_read(uint8_t block,
    const t55xx_demod_t *demod, uint32_t *word) {
    reads++;
    assert(block < 7);
    if (unreadable_mask & (1u << block)) {
        return false;
    }
    *word = broadcast_only ? memory[0] : memory[block];
    return true;
}

void t55xx_radio_write(uint8_t block, uint32_t word) {
    assert(block < 7 && writes < 7);
    order[writes++] = block;
    memory[block] = corrupt_write ? word ^ 1 : word;
}

static void reset(void) {
    memset(memory, 0, sizeof(memory));
    memory[0] = 0x00148041;
    memory[1] = 0xff800000;
    memory[2] = 0x0034a800;
    writes = 0;
    reads = 0;
    corrupt_write = false;
    broadcast_only = false;
    unreadable_mask = 0;
}

int main(void) {
    uint8_t input[64] = {1, 1, 0, 0, 0, 0, 0, 0, 170, 0, 4, 3, 0, 255, 79};
    uint8_t output[LF_T55XX_PLAN_BYTES];
    uint16_t length;
    reset();
    assert(lf_t55xx_command(input, 14, output, &length) == STATUS_PAR_ERR);
    assert(reads == 0 && writes == 0);
    input[2] = 1;
    assert(lf_t55xx_command(input, 15, output, &length) == STATUS_PAR_ERR);
    assert(reads == 0 && writes == 0);
    input[2] = 0;
    input[1] = 3;
    assert(lf_t55xx_command(input, 15, output, &length) == STATUS_PAR_ERR);
    assert(reads == 0 && writes == 0);
    input[1] = 1;
    assert(lf_t55xx_command(input, 15, output, &length) == STATUS_LF_TAG_OK);
    assert(length == LF_T55XX_PLAN_BYTES && writes == 0);
    assert(bytes_to_num(output + 37, 4) == 0x00088041);
    memcpy(input + 15, output + 65, 4);
    input[1] = 2;
    memory[2] ^= 1;
    assert(lf_t55xx_command(input, 19, output, &length) == STATUS_BUSY);
    assert(writes == 0);
    memory[2] ^= 1;
    assert(lf_t55xx_command(input, 19, output, &length) == STATUS_LF_TAG_OK);
    assert(output[3] == 2 && writes == 3);
    assert(order[0] == 1 && order[1] == 2 && order[2] == 0);
    assert(memory[0] == 0x00088041);
    assert(memory[1] == 0xf2000003 && memory[2] == 0x00ff4fe9);

    input[1] = 1;
    assert(lf_t55xx_command(input, 15, output, &length) == STATUS_LF_TAG_OK);
    memcpy(input + 15, output + 65, 4);
    input[1] = 2;
    writes = 0;
    assert(lf_t55xx_command(input, 19, output, &length) == STATUS_LF_TAG_OK);
    assert(writes == 0 && output[3] == 1);

    reset();
    input[1] = 1;
    assert(lf_t55xx_command(input, 15, output, &length) == STATUS_LF_TAG_OK);
    memcpy(input + 15, output + 65, 4);
    input[1] = 2;
    unreadable_mask = 1u << 6;
    assert(lf_t55xx_command(input, 19, output, &length) == STATUS_LF_TAG_OK);
    assert(writes == 3 && output[3] == 2 && !(output[2] & unreadable_mask));

    reset();
    input[1] = 1;
    assert(lf_t55xx_command(input, 15, output, &length) == STATUS_LF_TAG_OK);
    memcpy(input + 15, output + 65, 4);
    input[1] = 2;
    corrupt_write = true;
    assert(lf_t55xx_command(input, 19, output, &length) == STATUS_CMD_ERR);
    assert(writes == 1 && memory[0] == 0x00148041);

    reset();
    input[1] = 1;
    broadcast_only = true;
    assert(lf_t55xx_command(input, 15, output, &length) == STATUS_LF_TAG_NO_FOUND);
    assert(writes == 0);

    reset();
    memory[0] |= 0x10;
    assert(lf_t55xx_command(input, 15, output, &length) == STATUS_LF_TAG_LOGIN_REQUIRED);
    assert(writes == 0);

    reset();
    memory[0] |= 0x60000000;
    assert(lf_t55xx_command(input, 15, output, &length) == STATUS_LF_TAG_OK);
    assert(bytes_to_num(output + 37, 4) == 0x60088041);

    /* A constant desired word cannot be verified under an NRZ source. This
     * must be rejected while preparing, before any block has changed. */
    reset();
    memory[0] = 0x00080081;
    uint8_t zero_id[19] = {1, 1, 0, 0, 0, 0, 0, 1, 54, 0, 8, 'I', 'D', 'T', 'K', 0, 0, 0, 0};
    assert(lf_t55xx_command(zero_id, sizeof(zero_id), output, &length) == STATUS_NOT_IMPLEMENTED);
    assert(writes == 0);
    wiegand_card_t checkpoint = {.format = HCP32, .card_number = 0xFFFFFF};
    assert(wiegand_card_valid(&checkpoint));
    checkpoint.card_number++;
    assert(!wiegand_card_valid(&checkpoint));
    wiegand_card_t optus = {.format = OPTUS34, .facility_code = 0x7FF, .card_number = 1};
    assert(wiegand_card_valid(&optus));
    optus.facility_code++;
    assert(!wiegand_card_valid(&optus));
    wiegand_card_t smartpass = {.format = SMP34, .facility_code = 0x1FFF, .card_number = 1};
    assert(wiegand_card_valid(&smartpass));
    smartpass.facility_code++;
    assert(!wiegand_card_valid(&smartpass));
    wiegand_card_t actprox = {.format = ACTPHID, .oem = 900, .card_number = 1};
    assert(wiegand_card_valid(&actprox));
    actprox.issue_level = 1;
    assert(!wiegand_card_valid(&actprox));
    reset();
    assert(lf_t55xx_command(zero_id, sizeof(zero_id), output, &length) == STATUS_NOT_IMPLEMENTED);
    assert(writes == 0);
    return 0;
}
