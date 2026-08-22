#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdio_time_clock.h"

#define EXPECT(expression)                                                     \
    do {                                                                       \
        if (!(expression)) {                                                   \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression);   \
            return false;                                                      \
        }                                                                      \
    } while (0)

#define TEST_EPOCH_MS UINT64_C(1780000000000)

static sdio_time_message_t sample_message(uint32_t request_id,
                                          uint64_t client_tx_us)
{
    return (sdio_time_message_t) {
        .magic = SDIO_TIME_MAGIC,
        .version = SDIO_FRAME_VERSION,
        .size = sizeof(sdio_time_message_t),
        .kind = SDIO_TIME_KIND_SAMPLE,
        .status = SDIO_TIME_STATUS_OK,
        .request_id = request_id,
        .client_tx_monotonic_us = client_tx_us,
        .server_rx_unix_ms = TEST_EPOCH_MS,
        .server_tx_unix_ms = TEST_EPOCH_MS + 20U,
    };
}

static bool test_ntp_midpoint_and_expiry(void)
{
    sdio_time_clock_t clock = {0};
    sdio_time_message_t sample = sample_message(7U, UINT64_C(1000000));

    EXPECT(sdio_time_clock_apply_sample(&clock, &sample, 7U,
                                        UINT64_C(1000000), 1100000) ==
           SDIO_TIME_APPLY_ACCEPTED);
    EXPECT(clock.valid);
    EXPECT(clock.anchor_monotonic_us == 1050000);
    EXPECT(clock.anchor_unix_us ==
           TEST_EPOCH_MS * UINT64_C(1000) + UINT64_C(10000));
    EXPECT(clock.measured_delay_us == UINT64_C(80000));
    EXPECT(sdio_time_clock_now_ms(&clock, 1100000) ==
           TEST_EPOCH_MS + UINT64_C(60));
    EXPECT(sdio_time_clock_now_ms(
               &clock, 1100000 + SDIO_TIME_ANCHOR_MAX_AGE_US) != 0U);
    EXPECT(sdio_time_clock_now_ms(
               &clock, 1100001 + SDIO_TIME_ANCHOR_MAX_AGE_US) == 0U);
    return true;
}

static bool test_stale_request_is_ignored(void)
{
    sdio_time_clock_t clock = {0};
    sdio_time_message_t sample = sample_message(8U, UINT64_C(2000000));

    EXPECT(sdio_time_clock_apply_sample(&clock, &sample, 9U,
                                        UINT64_C(2000000), 2100000) ==
           SDIO_TIME_APPLY_STALE);
    EXPECT(!clock.valid);
    EXPECT(sdio_time_clock_apply_sample(&clock, &sample, 8U,
                                        UINT64_C(2000001), 2100000) ==
           SDIO_TIME_APPLY_STALE);
    EXPECT(!clock.valid);
    return true;
}

static bool test_remote_unavailable_is_not_a_sample(void)
{
    sdio_time_clock_t clock = {0};
    sdio_time_message_t sample = sample_message(10U, UINT64_C(3000000));
    sample.status = SDIO_TIME_STATUS_UNAVAILABLE;

    EXPECT(sdio_time_clock_apply_sample(&clock, &sample, 10U,
                                        UINT64_C(3000000), 3100000) ==
           SDIO_TIME_APPLY_REMOTE_STATUS);
    EXPECT(!clock.valid);
    return true;
}

static bool test_invalid_server_timestamps_are_rejected(void)
{
    sdio_time_clock_t clock = {0};
    sdio_time_message_t sample = sample_message(11U, UINT64_C(4000000));

    sample.server_rx_unix_ms = UINT64_C(1000);
    EXPECT(sdio_time_clock_apply_sample(&clock, &sample, 11U,
                                        UINT64_C(4000000), 4100000) ==
           SDIO_TIME_APPLY_INVALID);

    sample = sample_message(11U, UINT64_C(4000000));
    sample.server_tx_unix_ms = sample.server_rx_unix_ms - 1U;
    EXPECT(sdio_time_clock_apply_sample(&clock, &sample, 11U,
                                        UINT64_C(4000000), 4100000) ==
           SDIO_TIME_APPLY_INVALID);

    sample = sample_message(11U, UINT64_C(4000000));
    sample.server_tx_unix_ms = sample.server_rx_unix_ms + 200U;
    EXPECT(sdio_time_clock_apply_sample(&clock, &sample, 11U,
                                        UINT64_C(4000000), 4100000) ==
           SDIO_TIME_APPLY_INVALID);
    EXPECT(!clock.valid);
    return true;
}

static bool test_old_round_trip_is_rejected(void)
{
    sdio_time_clock_t clock = {0};
    sdio_time_message_t sample = sample_message(12U, UINT64_C(5000000));
    const int64_t too_late =
        INT64_C(5000000) + SDIO_TIME_QUERY_TIMEOUT_US + 1;

    EXPECT(sdio_time_clock_apply_sample(&clock, &sample, 12U,
                                        UINT64_C(5000000), too_late) ==
           SDIO_TIME_APPLY_STALE);
    EXPECT(!clock.valid);
    return true;
}

static bool test_invalidate_clears_mapping(void)
{
    sdio_time_clock_t clock = {0};
    sdio_time_message_t sample = sample_message(13U, UINT64_C(6000000));
    EXPECT(sdio_time_clock_apply_sample(&clock, &sample, 13U,
                                        UINT64_C(6000000), 6100000) ==
           SDIO_TIME_APPLY_ACCEPTED);
    sdio_time_clock_invalidate(&clock);
    EXPECT(!clock.valid);
    EXPECT(sdio_time_clock_now_ms(&clock, 6100000) == 0U);
    return true;
}

int main(void)
{
    const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"NTP midpoint and expiry", test_ntp_midpoint_and_expiry},
        {"stale request", test_stale_request_is_ignored},
        {"remote unavailable", test_remote_unavailable_is_not_a_sample},
        {"invalid server timestamps", test_invalid_server_timestamps_are_rejected},
        {"old round trip", test_old_round_trip_is_rejected},
        {"invalidate mapping", test_invalidate_clears_mapping},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (!tests[i].run()) {
            fprintf(stderr, "test failed: %s\n", tests[i].name);
            return 1;
        }
        printf("PASS: %s\n", tests[i].name);
    }
    return 0;
}
