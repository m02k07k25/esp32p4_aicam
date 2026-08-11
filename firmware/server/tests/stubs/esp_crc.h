#ifndef TEST_ESP_CRC_H
#define TEST_ESP_CRC_H

#include <stdint.h>

static inline uint32_t esp_crc32_le(uint32_t crc,
                                    const uint8_t *data,
                                    uint32_t len)
{
    crc = ~crc;
    while (len-- != 0U) {
        crc ^= *data++;
        for (unsigned int bit = 0; bit < 8U; ++bit) {
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) &
                                (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

#endif
