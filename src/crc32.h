#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct crc32 {
    uint32_t table[256];
    uint32_t value;
} crc32_t;

// Reflected CRC-32 polynomial. With an init of 0xFFFFFFFF and a final XOR of
// 0xFFFFFFFF this is bit-identical to java.util.zip.CRC32, which is what the
// client validates frames with.
#define CRC32_INITIAL 0xedb88320

#ifdef __cplusplus
extern "C" {
#endif

void crc32_init(crc32_t *crc);

uint32_t crc32_crc(crc32_t *c, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
