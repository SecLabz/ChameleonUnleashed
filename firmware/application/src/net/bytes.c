#include "bytes.h"

uint16_t read_le16(const uint8_t *p) {
    return p[0] | ((uint16_t)p[1] << 8);
}

uint32_t read_le32(const uint8_t *p) {
    return read_le16(p) | ((uint32_t)read_le16(p + 2) << 16);
}

void write_le16(uint8_t *p, uint16_t v) {
    p[0] = v;
    p[1] = v >> 8;
}

void write_le32(uint8_t *p, uint32_t v) {
    write_le16(p, v);
    write_le16(p + 2, v >> 16);
}
