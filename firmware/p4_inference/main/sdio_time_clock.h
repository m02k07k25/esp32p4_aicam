#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdio_frame_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A TIME request may cross a multi-hop Mesh path behind C6. Keep this longer
 * than C6's five-second Gateway response timeout, but short enough that a late
 * response cannot silently become the clock source. */
#define SDIO_TIME_QUERY_TIMEOUT_US       (10LL * 1000LL * 1000LL)
/* Keep clock work out of phase with the fixed 10-second inference cadence.
 * Prime-ish second intervals prevent a TIME request from repeatedly owning
 * every matching human-detection window after a stable boot. */
#define SDIO_TIME_QUERY_INTERVAL_US      (307LL * 1000LL * 1000LL)
#define SDIO_TIME_BUSY_RETRY_US          (1LL * 1000LL * 1000LL)
#define SDIO_TIME_ERROR_RETRY_US         (37LL * 1000LL * 1000LL)
#define SDIO_TIME_ANCHOR_MAX_AGE_US      (900LL * 1000LL * 1000LL)
#define SDIO_TIME_QUANTIZATION_SLOP_US   2000ULL

typedef struct {
    bool valid;
    int64_t anchor_monotonic_us;
    int64_t refreshed_monotonic_us;
    uint64_t anchor_unix_us;
    uint64_t measured_delay_us;
} sdio_time_clock_t;

typedef enum {
    SDIO_TIME_APPLY_ACCEPTED = 0,
    SDIO_TIME_APPLY_STALE,
    SDIO_TIME_APPLY_REMOTE_STATUS,
    SDIO_TIME_APPLY_INVALID,
} sdio_time_apply_result_t;

void sdio_time_clock_invalidate(sdio_time_clock_t *clock);

sdio_time_apply_result_t sdio_time_clock_apply_sample(
    sdio_time_clock_t *clock,
    const sdio_time_message_t *sample,
    uint32_t expected_request_id,
    uint64_t expected_client_tx_monotonic_us,
    int64_t client_rx_monotonic_us);

/* Return server-authoritative Unix milliseconds, or zero if the mapping has
 * never been established, is inconsistent, or has aged out. */
uint64_t sdio_time_clock_now_ms(const sdio_time_clock_t *clock,
                                int64_t now_monotonic_us);

#ifdef __cplusplus
}
#endif
