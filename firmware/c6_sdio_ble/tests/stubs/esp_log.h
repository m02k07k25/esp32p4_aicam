#pragma once

void host_test_log(const char *tag, const char *format, ...);

#define ESP_LOGE(tag, format, ...) host_test_log((tag), (format), ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) host_test_log((tag), (format), ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) host_test_log((tag), (format), ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) host_test_log((tag), (format), ##__VA_ARGS__)
