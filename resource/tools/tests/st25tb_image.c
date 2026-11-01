#include <assert.h>
#include <stddef.h>
#include <string.h>
#include "st25tb/model.h"

enum { STATUS_SUCCESS = 0x68, STATUS_PAR_ERR = 0x60 };
typedef struct {
    uint16_t command, status, length;
    uint8_t data[ST25TB_IMAGE_SIZE];
} data_frame_tx_t;
static data_frame_tx_t response;
static bool loaded = true;
static unsigned image_reads;

static bool nfc_st25tb_get_image(uint8_t image[ST25TB_IMAGE_SIZE]) {
    image_reads++;
    for (unsigned i = 0; i < ST25TB_IMAGE_SIZE; i++) image[i] = i;
    return loaded;
}

static data_frame_tx_t *data_frame_make(uint16_t command, uint16_t status,
                                       uint16_t length, const uint8_t *data) {
    response = (data_frame_tx_t){.command = command, .status = status, .length = length};
    if (length) memcpy(response.data, data, length);
    else assert(data == NULL);
    return &response;
}

/* IMAGE COMMAND */

int main(void) {
    data_frame_tx_t *result = cmd_processor_st25tb_get_image(4310, 0, 0, NULL);
    assert(result->command == 4310 && result->status == STATUS_SUCCESS);
    assert(result->length == ST25TB_IMAGE_SIZE && image_reads == 1);
    for (unsigned i = 0; i < result->length; i++) assert(result->data[i] == i);
    uint8_t selectors[] = {0x00, 0xd0, 0xd1, 0xd2, 0xdf};
    for (unsigned i = 0; i < sizeof(selectors); i++) {
        result = cmd_processor_st25tb_get_image(4310, 0, 1, &selectors[i]);
        assert(result->status == STATUS_PAR_ERR && result->length == 0);
        assert(image_reads == 1);
    }
    loaded = false;
    result = cmd_processor_st25tb_get_image(4310, 0, 0, NULL);
    assert(result->status == STATUS_PAR_ERR && result->length == 0 && image_reads == 2);
}
