#ifndef SERVER_SERIAL_ADAPTER_H
#define SERVER_SERIAL_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERVER_SERIAL_FRAME_MAGIC       "BMJPEG01"
#define SERVER_SERIAL_FRAME_MAGIC_SIZE  8U
#define SERVER_SERIAL_FRAME_VERSION     1U
#define SERVER_SERIAL_FRAME_HEADER_SIZE 40U

/*
 * Console-stream image record header. The wire format is packed little-endian:
 *
 *   <8sHHHBBQIIII
 *
 * header_crc32 is CRC-32/IEEE (esp_crc32_le seed 0) over the first 36
 * header bytes. jpeg_crc32 uses the same convention over the immediately
 * following jpeg_len bytes. reserved is always zero.
 */
typedef struct __attribute__((packed)) {
    uint8_t magic[SERVER_SERIAL_FRAME_MAGIC_SIZE];
    uint16_t version;
    uint16_t header_size;
    uint16_t source_addr;
    uint8_t time_source;
    uint8_t reserved;
    uint64_t event_time_ms;
    uint32_t jpeg_len;
    uint32_t jpeg_crc32;
    uint32_t sequence;
    uint32_t header_crc32;
} server_serial_frame_header_t;

#if defined(__cplusplus)
static_assert(sizeof(server_serial_frame_header_t) ==
                  SERVER_SERIAL_FRAME_HEADER_SIZE,
              "serial image header layout changed");
#else
_Static_assert(sizeof(server_serial_frame_header_t) ==
                   SERVER_SERIAL_FRAME_HEADER_SIZE,
               "serial image header layout changed");
#endif

/* Initialize the fixed JPEG slot, stdout writer, and Mesh callback. */
esp_err_t server_serial_adapter_init(void);

/* True from callback enqueue until the complete JPEG record leaves stdout. */
bool server_serial_adapter_is_transmitting(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_SERIAL_ADAPTER_H */
