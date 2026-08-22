#ifndef SERVER_WIFI_TIME_ADAPTER_H
#define SERVER_WIFI_TIME_ADAPTER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the independent Wi-Fi STA and persistent SNTP clock provider. */
esp_err_t server_wifi_time_adapter_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_WIFI_TIME_ADAPTER_H */
