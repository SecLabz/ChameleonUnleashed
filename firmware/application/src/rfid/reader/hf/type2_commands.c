#include "type2_commands.h"
#include "rc522.h"
#include "app_status.h"
#include <string.h>

uint16_t type2_read_command(uint16_t size, const uint8_t *data,
    uint8_t *output, uint16_t *length) {
    *length = 0;
    if (size < 3 || (data[2] != 4 && data[2] != 7 && data[2] != 10) ||
        size != 3u + data[2] + (data[0] == 5)) return STATUS_PAR_ERR;
    uint8_t frame[5] = {0}; // Three FAST_READ bytes plus the appended CRC.
    uint16_t sent = 2, expected;
    switch (data[0]) {
    case 0:
        if (data[1]) return STATUS_PAR_ERR;
        frame[0] = 0x60; sent = 1; expected = 8; break;
    case 1: frame[0] = 0x30; expected = 16; break;
    case 2:
        if (data[1]) return STATUS_PAR_ERR;
        frame[0] = 0x3c; expected = 32; break;
    case 3: case 4:
        if (data[1] > 2) return STATUS_PAR_ERR;
        frame[0] = data[0] == 3 ? 0x39 : 0x3e;
        expected = data[0] == 3 ? 3 : 1; break;
    case 5: {
        uint8_t end = data[3 + data[2]];
        if (end < data[1] || end - data[1] >= 8) return STATUS_PAR_ERR;
        frame[0] = 0x3a; frame[2] = end; sent = 3;
        expected = (end - data[1] + 1) * 4; break;
    }
    default: return STATUS_PAR_ERR;
    }
    frame[1] = data[1];
    // Return an already selected tag to HALT before WUPA without a power cycle.
    pcd_14a_reader_fast_halt_tag();
    picc_14a_tag_t tag;
    uint16_t status = pcd_14a_reader_scan_auto(&tag);
    if (status != STATUS_HF_TAG_OK) return status;
    if (tag.sak != 0 || tag.uid_len != data[2] || memcmp(tag.uid, data + 3, tag.uid_len))
        return STATUS_HF_TAG_NO;
    // CRC is checked and removed by the RF owner. Reserve space for it before copying.
    uint8_t received[34];
    uint16_t count = 0;
    status = pcd_14a_reader_raw_cmd(false, true, true, false, true, true, 25,
        sent * 8, frame, received, &count, sizeof(received) * 8);
    if (status != STATUS_HF_TAG_OK) return status;
    if (count != expected) return STATUS_HF_ERR_STAT;
    memcpy(output, received, count); *length = count;
    return STATUS_HF_TAG_OK;
}
