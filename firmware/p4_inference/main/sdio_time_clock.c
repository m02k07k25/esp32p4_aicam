#include "sdio_time_clock.h"

#include <limits.h>
#include <stddef.h>

#define MIN_VALID_UNIX_MS UINT64_C(1704067200000) /* 2024-01-01 UTC */

void sdio_time_clock_invalidate(sdio_time_clock_t *clock)
{
    if (clock == NULL) {
        return;
    }
    clock->valid = false;
    clock->anchor_monotonic_us = 0;
    clock->refreshed_monotonic_us = 0;
    clock->anchor_unix_us = 0;
    clock->measured_delay_us = 0;
}

sdio_time_apply_result_t sdio_time_clock_apply_sample(
    sdio_time_clock_t *clock,
    const sdio_time_message_t *sample,
    uint32_t expected_request_id,
    uint64_t expected_client_tx_monotonic_us,
    int64_t client_rx_monotonic_us)
{
    if (clock == NULL || sample == NULL) {
        return SDIO_TIME_APPLY_INVALID;
    }
    if (sample->request_id != expected_request_id ||
        sample->client_tx_monotonic_us != expected_client_tx_monotonic_us) {
        return SDIO_TIME_APPLY_STALE;
    }
    if (sample->status != SDIO_TIME_STATUS_OK) {
        return SDIO_TIME_APPLY_REMOTE_STATUS;
    }
    if (expected_client_tx_monotonic_us > (uint64_t)INT64_MAX ||
        client_rx_monotonic_us < 0 ||
        client_rx_monotonic_us < (int64_t)expected_client_tx_monotonic_us) {
        return SDIO_TIME_APPLY_INVALID;
    }
    if (sample->server_rx_unix_ms < MIN_VALID_UNIX_MS ||
        sample->server_tx_unix_ms < sample->server_rx_unix_ms ||
        sample->server_tx_unix_ms > UINT64_MAX / UINT64_C(1000)) {
        return SDIO_TIME_APPLY_INVALID;
    }

    const uint64_t client_elapsed_us =
        (uint64_t)(client_rx_monotonic_us -
                   (int64_t)expected_client_tx_monotonic_us);
    if (client_elapsed_us > (uint64_t)SDIO_TIME_QUERY_TIMEOUT_US) {
        return SDIO_TIME_APPLY_STALE;
    }

    const uint64_t server_elapsed_ms =
        sample->server_tx_unix_ms - sample->server_rx_unix_ms;
    if (server_elapsed_ms > UINT64_MAX / UINT64_C(1000)) {
        return SDIO_TIME_APPLY_INVALID;
    }
    const uint64_t server_elapsed_us = server_elapsed_ms * UINT64_C(1000);
    if (server_elapsed_us > client_elapsed_us +
                                SDIO_TIME_QUANTIZATION_SLOP_US) {
        /* Server timestamps have millisecond resolution. A small negative
         * apparent path delay is therefore possible, but a larger one means
         * this response cannot belong to the measured round trip. */
        return SDIO_TIME_APPLY_INVALID;
    }

    const uint64_t server_rx_us = sample->server_rx_unix_ms * UINT64_C(1000);
    const uint64_t server_midpoint_us =
        server_rx_us + server_elapsed_us / UINT64_C(2);
    const int64_t client_midpoint_us =
        (int64_t)expected_client_tx_monotonic_us +
        (int64_t)(client_elapsed_us / UINT64_C(2));

    clock->valid = true;
    clock->anchor_monotonic_us = client_midpoint_us;
    clock->refreshed_monotonic_us = client_rx_monotonic_us;
    clock->anchor_unix_us = server_midpoint_us;
    clock->measured_delay_us = client_elapsed_us > server_elapsed_us ?
                               client_elapsed_us - server_elapsed_us : 0;
    return SDIO_TIME_APPLY_ACCEPTED;
}

uint64_t sdio_time_clock_now_ms(const sdio_time_clock_t *clock,
                                int64_t now_monotonic_us)
{
    if (clock == NULL || !clock->valid || now_monotonic_us < 0 ||
        now_monotonic_us < clock->anchor_monotonic_us ||
        now_monotonic_us < clock->refreshed_monotonic_us ||
        now_monotonic_us - clock->refreshed_monotonic_us >
            SDIO_TIME_ANCHOR_MAX_AGE_US) {
        return 0;
    }

    const uint64_t elapsed_us =
        (uint64_t)(now_monotonic_us - clock->anchor_monotonic_us);
    if (clock->anchor_unix_us > UINT64_MAX - elapsed_us) {
        return 0;
    }
    return (clock->anchor_unix_us + elapsed_us) / UINT64_C(1000);
}
