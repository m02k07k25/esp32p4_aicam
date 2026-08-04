/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __EXAMPLE_PEER_DATA_TRANSFER_H__
#define __EXAMPLE_PEER_DATA_TRANSFER_H__

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the P4 frame receiver and C6 HTTP validation server. */
esp_err_t example_peer_data_transfer_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __EXAMPLE_PEER_DATA_TRANSFER_H__ */
