#ifndef SERVER_SERIAL_TIME_ADAPTER_H
#define SERVER_SERIAL_TIME_ADAPTER_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERVER_SERIAL_TIME_MAGIC       "BMTIME01"
#define SERVER_SERIAL_TIME_MAGIC_SIZE  8U
#define SERVER_SERIAL_TIME_VERSION     1U
#define SERVER_SERIAL_TIME_PACKET_SIZE 28U
#define SERVER_SERIAL_TIME_CRC_BYTES   24U

/*
 * Laptop-to-server console UART packet, packed little-endian:
 *
 *   <8sHHQII
 *   magic, version, packet_size, unix_ms, sequence, crc32
 *
 * crc32 covers the first 24 bytes using CRC-32/IEEE with seed zero. The
 * sequence is nonzero and diagnostic only; restarting the PC receiver may
 * restart it because unix_ms is the authoritative freshness value.
 */
typedef struct __attribute__((packed)) {
    uint8_t magic[SERVER_SERIAL_TIME_MAGIC_SIZE];
    uint16_t version;
    uint16_t packet_size;
    uint64_t unix_ms;
    uint32_t sequence;
    uint32_t crc32;
} server_serial_time_packet_t;

#if defined(__cplusplus)
static_assert(sizeof(server_serial_time_packet_t) ==
                  SERVER_SERIAL_TIME_PACKET_SIZE,
              "serial time packet layout changed");
#else
_Static_assert(sizeof(server_serial_time_packet_t) ==
                   SERVER_SERIAL_TIME_PACKET_SIZE,
               "serial time packet layout changed");
#endif

/* Install console UART RX and register the laptop-backed clock provider. */
esp_err_t server_serial_time_adapter_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_SERIAL_TIME_ADAPTER_H */
