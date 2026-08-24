#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "server_serial_time_adapter.h"

int main(void)
{
    assert(sizeof(server_serial_time_packet_t) == 28U);
    assert(offsetof(server_serial_time_packet_t, version) == 8U);
    assert(offsetof(server_serial_time_packet_t, packet_size) == 10U);
    assert(offsetof(server_serial_time_packet_t, unix_ms) == 12U);
    assert(offsetof(server_serial_time_packet_t, sequence) == 20U);
    assert(offsetof(server_serial_time_packet_t, crc32) == 24U);
    assert(strlen(SERVER_SERIAL_TIME_MAGIC) == 8U);
    puts("PASS: laptop time header is exact packed 28-byte layout");
    return 0;
}
