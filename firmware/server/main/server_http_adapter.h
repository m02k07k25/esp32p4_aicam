#ifndef SERVER_HTTP_ADAPTER_H
#define SERVER_HTTP_ADAPTER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional Wi-Fi/SNTP + /latest.jpg + /latest.json adapter. */
esp_err_t server_http_adapter_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_HTTP_ADAPTER_H */
