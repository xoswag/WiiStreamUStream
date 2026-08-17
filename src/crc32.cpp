#include "crc32.h"

void crc32_init(crc32_t *crc) {
    for (int i = 0; i < 256; ++i) {
        uint32_t v = (uint32_t) i;
        for (int j = 0; j < 8; ++j) {
            v = (v & 1) ? (CRC32_INITIAL ^ (v >> 1)) : (v >> 1);
        }
        crc->table[i] = v;
    }
    crc->value = 0;
}

static void crc32_update(crc32_t *c, const uint8_t *buf, size_t len) {
    uint32_t value = c->value;
    for (size_t i = 0; i < len; ++i) {
        value = c->table[(value ^ buf[i]) & 0xFF] ^ (value >> 8);
    }
    c->value = value;
}

uint32_t crc32_crc(crc32_t *c, const uint8_t *buf, size_t len) {
    c->value = 0xfffffffful;
    crc32_update(c, buf, len);
    return c->value ^ 0xfffffffful;
}
