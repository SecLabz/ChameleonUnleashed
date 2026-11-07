static void setup(void) {
    memset(memory, 0, sizeof(memory));
    uint8_t block0[] = {1, 2, 3, 4, 4, 8, 4, 0};
    memcpy(memory[0], block0, sizeof(block0));
    for (unsigned block = 3; block < 128; block += 4) {
        memset(memory[block], 255, 16);
        memory[block][7] = 7;
        memory[block][8] = 128;
        memory[block][9] = 105;
    }
    writes = 0;
    scans = 0;
    corrupt_readback = false;
    selected_sak = 8;
    selected_sak_from_block0 = false;
    capacity_status = STATUS_HF_TAG_NO;
    capacity_checks = 0;
    config = (hf14a_config_t){0, 0, 0, 1};
}

int main(void) {
    uint8_t request[52] = {0, 3, 1, 4, 1, 2, 3, 4};
    request[12] = 1;
    memset(request + 13, 255, 6);
    uint8_t output[27];
    uint16_t length = 0;
    setup();
    memset(request + 4, 0, 7);
    assert(maintenance(19, request, output, &length) == STATUS_HF_TAG_NO && writes == 0);
    request[0] = 3;
    assert(maintenance(19, request, output, &length) == STATUS_HF_TAG_OK && writes == 0);
    request[0] = 0;
    memcpy(request + 4, memory[0], 4);
    memory[0][4] ^= 1;
    assert(maintenance(19, request, output, &length) == STATUS_HF_TAG_OK);
    assert(length == 27 && output[15] == 5 && writes == 0);
    assert(config.forcebcc == 0 && config.forcerats == 1);
    memcpy(request + 19, memory[0], 16);
    memcpy(request + 35, memory[0], 16);
    request[39] = 4;
    request[0] = 1;
    assert(maintenance(51, request, output, &length) == STATUS_HF_TAG_OK && writes == 0);
    request[0] = 2;
    assert(maintenance(51, request, output, &length) == STATUS_HF_TAG_OK && writes == 1);
    assert(memory[0][4] == 4 && config.forcebcc == 0 && config.forcerats == 1);

    setup();
    request[4] = 42;
    assert(maintenance(51, request, output, &length) == STATUS_HF_TAG_NO && writes == 0);
    assert(config.forcebcc == 0 && config.forcerats == 1);
    request[4] = 1;
    request[13] = 0;
    assert(maintenance(51, request, output, &length) == STATUS_MF_ERR_AUTH && writes == 0);
    assert(config.forcebcc == 0 && config.forcerats == 1);
    request[13] = 255;

    setup();
    request[11] = 7;
    memory[7][6] = 0x7f;
    memory[7][8] = 0x88;
    memcpy(request + 19, memory[7], 16);
    memcpy(request + 35, memory[7], 16);
    request[41] = 255;
    request[43] = 128;
    request[0] = 1;
    assert(maintenance(51, request, output, &length) == STATUS_CMD_ERR && writes == 0);
    request[12] = 2;
    assert(maintenance(51, request, output, &length) == STATUS_HF_TAG_OK && writes == 0);
    request[0] = 2;
    assert(maintenance(51, request, output, &length) == STATUS_HF_TAG_OK && writes == 1);

    setup();
    uint8_t original_identity[16];
    memcpy(original_identity, memory[0], sizeof(original_identity));
    request[1] = 3;
    request[11] = 1;
    request[12] = 1;
    memcpy(request + 19, memory[1], 16);
    memset(request + 35, 42, 16);
    request[0] = 1;
    assert(maintenance(51, request, output, &length) == STATUS_HF_TAG_OK && writes == 0);
    assert(memcmp(memory[0], original_identity, 16) == 0);
    request[0] = 2;
    assert(maintenance(51, request, output, &length) == STATUS_HF_TAG_OK && writes == 1);
    assert(memcmp(memory[1], request + 35, 16) == 0);
    assert(memcmp(memory[0], original_identity, 16) == 0);
    assert(config.forcebcc == 0 && config.forcerats == 1);
    request[11] = 3;

    for (uint8_t method = 1; method <= 3; method++) {
        setup();
        request[1] = method;
        request[11] = 3;
        request[12] = method == 3 ? 1 : 0;
        memcpy(request + 19, memory[3], 16);
        memcpy(request + 35, memory[3], 16);
        memset(request + 35, 42, 6);
        memory[3][6] = 0; /* Invalid rights: backdoor may still repair them. */
        request[25] = 0;
        uint16_t status = maintenance(51, request, output, &length);
        if (method == 3) {
            assert(status != STATUS_HF_TAG_OK && writes == 0);
        } else {
            assert(status == STATUS_HF_TAG_OK && writes == 1);
            assert(memory[3][6] == 255 && memory[3][0] == 42);
        }
        assert(config.forcebcc == 0 && config.forcerats == 1);
    }
    setup();
    request[1] = 1;
    request[12] = 0;
    memcpy(request + 19, memory[3], 16);
    memcpy(request + 35, memory[3], 16);
    request[35] = 42;
    corrupt_readback = true;
    assert(maintenance(51, request, output, &length) == STATUS_CMD_ERR && writes == 1);
    assert(config.forcebcc == 0 && config.forcerats == 1);
    /* Explicit known-model recovery must still read and repair malformed SAK,
     * including when that prevents any meaningful ATS capacity check. */
    for (unsigned level = 0; level < 2; level++) {
        uint8_t desired = level ? 0x10 : 8;
        setup(); selected_sak_from_block0 = true; memory[0][5] = 0;
        request[0] = 3; request[1] = 3; request[2] = 6;
        request[11] = 0; request[12] = 1;
        assert(maintenance(19, request, output, &length) == STATUS_HF_TAG_OK && output[10] == 0);
        memcpy(request + 19, memory[0], 16);
        memcpy(request + 35, memory[0], 16); request[40] = desired;
        request[0] = 1;
        assert(maintenance(51, request, output, &length) == STATUS_HF_TAG_OK && !writes);
        request[0] = 2;
        assert(maintenance(51, request, output, &length) == STATUS_HF_TAG_OK && writes == 1);
        assert(memory[0][5] == desired && !capacity_checks);
        assert(config.forcebcc == 0 && config.forcerats == 1);

    }

    setup();
    request[0] = 1; request[1] = 3; request[2] = 6; request[51] = 1;
    request[11] = 126; request[12] = 1;
    memcpy(request + 19, memory[126], 16);
    memset(request + 35, 42, 16);
    assert(maintenance(52, request, output, &length) == STATUS_HF_TAG_NO && !writes);
    assert(config.forcebcc == 0 && config.forcerats == 1);
    capacity_status = STATUS_HF_TAG_OK;
    assert(maintenance(52, request, output, &length) == STATUS_HF_TAG_OK && !writes);
    request[0] = 2;
    assert(maintenance(52, request, output, &length) == STATUS_HF_TAG_OK && writes == 1);
    assert(!memcmp(memory[126], request + 35, 16));
    assert(config.forcebcc == 0 && config.forcerats == 1);
    setup(); capacity_status = STATUS_HF_TAG_OK;
    request[0] = 1; request[11] = 0;
    memcpy(request + 19, memory[0], 16);
    memcpy(request + 35, memory[0], 16);
    request[39] = 4;
    assert(maintenance(52, request, output, &length) == STATUS_HF_TAG_OK && !writes);
    request[40] = 0x10; /* Never repair an observed SL1 tag into SL2 metadata. */
    assert(maintenance(52, request, output, &length) == STATUS_PAR_ERR && !writes);
    request[51] = 2;
    unsigned previous_scans = scans;
    assert(maintenance(52, request, output, &length) == STATUS_PAR_ERR && scans == previous_scans);
    request[51] = 1; request[2] = 1;
    assert(maintenance(52, request, output, &length) == STATUS_PAR_ERR && scans == previous_scans);
    return 0;
}
