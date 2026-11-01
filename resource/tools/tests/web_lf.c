#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "lf_commands.h"
#include "data_cmd.h"
#include "app_status.h"

static int mode = 1;
static bool found = true;
static bool electra;
static unsigned calls;
static unsigned acquisition_ms;

int get_device_mode(void) {
    return mode;
}

static bool scan(uint8_t *data, uint32_t timeout_ms, unsigned size) {
    assert(timeout_ms > 0 && timeout_ms <= 500);
    calls++;
    memset(data, 42, size);
    return found && timeout_ms >= acquisition_ms;
}

bool em410x_read(uint8_t *data, uint32_t timeout_ms) {
    bool result = scan(data, timeout_ms, electra ? 15 : 7);
    data[0] = 0;
    data[1] = electra ? 104 : 103;
    return result;
}

bool hidprox_read(uint8_t *data, uint8_t hint, uint32_t timeout_ms) {
    assert(hint == 0);
    return scan(data, timeout_ms, 13);
}

bool ioprox_read(uint8_t *data, uint8_t hint, uint32_t timeout_ms) {
    assert(hint == 0);
    return scan(data, timeout_ms, 16);
}

bool pac_read(uint8_t *data, uint32_t timeout_ms) {
    return scan(data, timeout_ms, 8);
}

bool viking_read(uint8_t *data, uint32_t timeout_ms) {
    return scan(data, timeout_ms, 4);
}

bool jablotron_read(uint8_t *data, uint32_t timeout_ms) {
    return scan(data, timeout_ms, 5);
}

uint16_t lf_t55xx_command(const uint8_t *request, size_t size, uint8_t *output, uint16_t *length) {
    assert(size == 7 && request[0] == 1);
    calls++;
    output[0] = 1;
    *length = 1;
    return STATUS_LF_TAG_OK;
}

int main(void) {
    uint8_t output[16];
    uint8_t hint = 0;
    uint16_t length = 999;
    const unsigned commands[] = {3000, 3002, 3010, 3014, 3004, 3019};
    const unsigned sizes[] = {7, 16, 16, 8, 4, 5};
    for (unsigned i = 0; i < 6; i++) {
        unsigned input = i == 1 || i == 2;
        assert(web_lf_command(commands[i], input, &hint, output, &length) == STATUS_LF_TAG_OK);
        assert(length == sizes[i]);
        if (i == 1) {
            assert(output[12] == 42 && output[13] == 0 && output[15] == 0);
        }
        unsigned before = calls;
        assert(web_lf_command(commands[i], input + 1, &hint, output, &length) == STATUS_PAR_ERR);
        assert(length == 0 && calls == before);
        found = false;
        assert(web_lf_command(commands[i], input, &hint, output, &length) == STATUS_LF_TAG_NO_FOUND);
        assert(length == 0);
        found = true;
    }

    electra = true;
    assert(web_lf_command(3000, 0, NULL, output, &length) == STATUS_LF_TAG_OK);
    assert(length == 15 && output[1] == 104);

    unsigned before = calls;
    mode = 2;
    assert(web_lf_command(3000, 0, NULL, output, &length) == STATUS_DEVICE_MODE_ERROR);
    assert(length == 0 && calls == before);
    mode = 1;
    assert(web_lf_command(3001, 0, NULL, output, &length) == STATUS_PAR_ERR);
    assert(length == 0 && calls == before);
    const uint8_t request[] = {1, 0, 0, 0, 0, 0, 0};
    assert(web_lf_command(3033, sizeof(request), request, output, &length) == STATUS_LF_TAG_OK);
    assert(length == 1 && output[0] == 1 && calls == before + 1);

    acquisition_ms = 350;
    assert(web_lf_command(DATA_CMD_VIKING_SCAN, 0, NULL, output, &length) == STATUS_LF_TAG_OK);
    assert(length == 4 && output[0] == 42 && output[3] == 42);
    acquisition_ms = 700;
    assert(web_lf_command(DATA_CMD_VIKING_SCAN, 0, NULL, output, &length) == STATUS_LF_TAG_NO_FOUND);
    assert(length == 0);
}
