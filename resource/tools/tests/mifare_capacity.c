int main(void) {
    uint8_t data[59] = {6, 4, 1, 2, 3, 4};
    data[9] = 126; data[10] = 1;
    memset(data + 11, 255, 6);
    memset(data + 43, 0x42, 16);
    uint8_t output[16];
    uint16_t length = 0;
    reset();
    memcpy(data + 27, memory[126], 16);
    assert(write_user_unit(59, data, output, &length) == STATUS_HF_TAG_NO);
    assert(!writes && capacity_checks == 1);
    capacity_status = STATUS_HF_ERR_PARITY;
    assert(write_user_unit(59, data, output, &length) == STATUS_HF_ERR_PARITY && !writes);
    capacity_status = STATUS_HF_TAG_OK;
    assert(write_user_unit(59, data, output, &length) == STATUS_HF_TAG_OK);
    assert(writes == 1 && length == 16 && !memcmp(output, data + 43, 16));
    assert(!memcmp(memory[126], data + 43, 16));
    assert(write_user_unit(59, data, output, &length) == STATUS_CMD_ERR && writes == 1);

    for (unsigned i = 0; i < 3; i++) {
        reset(); selected_sak = (uint8_t[]){9, 0x18, 0}[i];
        assert(write_user_unit(59, data, output, &length) == STATUS_HF_TAG_NO);
        assert(!writes && !capacity_checks);
    }
    for (unsigned i = 0; i < 3; i++) {
        reset(); data[9] = (uint8_t[]){0, 127, 128}[i];
        assert(write_user_unit(59, data, output, &length) == STATUS_PAR_ERR);
        assert(!writes && !scans && !capacity_checks);
    }
    reset(); selected_sak = 0x10; data[9] = 126;
    assert(write_user_unit(59, data, output, &length) == STATUS_HF_TAG_OK);
    assert(writes == 1 && !capacity_checks); /* Existing SL2 contract unchanged. */

    uint8_t block0[60] = {6, 4, 1, 2, 3, 4};
    block0[10] = 1; block0[59] = 1; memset(block0 + 11, 255, 6);
    reset(); signatures = MF1_MAGIC_GEN2;
    memcpy(block0 + 27, memory[0], 16);
    memcpy(block0 + 43, memory[0], 16); block0[52] = 0x42;
    assert(block0_set(60, block0, output, &length) == STATUS_HF_TAG_NO && !writes);
    capacity_status = STATUS_HF_TAG_OK; block0[59] = 1;
    assert(block0_set(60, block0, output, &length) == STATUS_HF_TAG_OK && !writes);
    assert(block0_set(59, block0, output, &length) == STATUS_HF_TAG_OK && writes == 1);
    assert(!memcmp(memory[0], block0 + 43, 16));

    uint8_t trailer[38] = {4, 1, 2, 3, 4};
    trailer[8] = 6; trailer[9] = 31; trailer[10] = 1;
    memset(trailer + 11, 255, 6);
    memset(trailer + 21, 0x42, 6); memset(trailer + 27, 0x43, 6);
    reset();
    memcpy(trailer + 17, memory[127] + 6, 4);
    memcpy(trailer + 33, memory[127] + 6, 4);
    assert(trailer_set(37, trailer, output, &length) == STATUS_HF_TAG_NO && !writes);
    capacity_status = STATUS_HF_TAG_OK; trailer[37] = 1;
    assert(trailer_set(38, trailer, output, &length) == STATUS_HF_TAG_OK && !writes);
    assert(trailer_set(37, trailer, output, &length) == STATUS_HF_TAG_OK && writes == 1);
    assert(!memcmp(memory[127], trailer + 21, 6));
    assert(!memcmp(memory[127] + 10, trailer + 27, 6));
    reset(); trailer[9] = 32;
    assert(trailer_set(37, trailer, output, &length) == STATUS_PAR_ERR && !writes && !scans);
    return 0;
}
