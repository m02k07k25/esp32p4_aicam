#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdio_frame_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sdio_frame_tx_init(void);

/* True only after the C6 explicitly reports SDIO_FRAME_CONTROL_READY. */
bool sdio_frame_tx_remote_ready(void);

/* Server-authoritative capture time reconstructed from the latest C6/Mesh
 * TIME exchange. Returns zero until a valid sample arrives or after expiry. */
uint64_t sdio_frame_tx_capture_time_ms(void);

/*
 * Atomically claim the current READY window and queue one bounded JPEG.  The
 * C6 must report READY again after ACCEPTED/SERVER_ACKED/FAILED before a
 * later frame can be queued.
 */
esp_err_t sdio_frame_tx_submit_event(const uint8_t *jpeg,
                                     size_t jpeg_len,
                                     uint64_t detected_at_ms);

#ifdef __cplusplus
}
#endif
