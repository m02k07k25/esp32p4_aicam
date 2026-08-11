#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "server_serial_adapter.h"

int main(void)
{
    assert(sizeof(server_serial_frame_header_t) == 40U);
    assert(offsetof(server_serial_frame_header_t, version) == 8U);
    assert(offsetof(server_serial_frame_header_t, header_size) == 10U);
    assert(offsetof(server_serial_frame_header_t, source_addr) == 12U);
    assert(offsetof(server_serial_frame_header_t, time_source) == 14U);
    assert(offsetof(server_serial_frame_header_t, event_time_ms) == 16U);
    assert(offsetof(server_serial_frame_header_t, jpeg_len) == 24U);
    assert(offsetof(server_serial_frame_header_t, jpeg_crc32) == 28U);
    assert(offsetof(server_serial_frame_header_t, sequence) == 32U);
    assert(offsetof(server_serial_frame_header_t, header_crc32) == 36U);
    assert(strlen(SERVER_SERIAL_FRAME_MAGIC) == 8U);
    puts("PASS: serial image header is exact packed 40-byte layout");
    return 0;
}
