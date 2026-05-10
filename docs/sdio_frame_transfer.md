# SDIO 분류 프레임 전송

P4는 `/classify.jpg` 요청을 처리한 뒤, JPEG와 추론 결과를 ESP-Hosted custom data로 C6에 전송합니다.

## P4 송신

송신은 `main/sdio_frame_tx.c`에서 처리합니다.

- message ID: `SDIO_FRAME_MSG_ID`
- chunk 헤더: `sdio_frame_chunk_header_t`
- chunk 데이터 최대 크기: `SDIO_FRAME_CHUNK_DATA_MAX`
- payload 구성: `sdio_frame_chunk_header_t` + JPEG chunk bytes

JPEG가 custom RPC payload 한도를 넘을 수 있으므로 여러 chunk로 나눠 보냅니다. C6는 `frame_id`, `chunk_index`, `chunk_count`, `chunk_offset`, `jpeg_size`를 사용해 JPEG를 다시 조립하면 됩니다.

## C6 수신 조건

C6의 ESP-Hosted slave 펌웨어에도 아래 설정이 필요합니다.

```text
CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y
```

## C6 수신 예시

`idf.py create-project-from-example "espressif/esp_hosted:slave"`로 만든 C6 slave 프로젝트에 같은 프로토콜 상수와 헤더 구조를 추가한 뒤 callback을 등록합니다.

```c
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "slave_control.h"

#define SDIO_FRAME_MSG_ID           0x504A5047u
#define SDIO_FRAME_MAGIC            0x46544A50u
#define SDIO_FRAME_VERSION          1
#define SDIO_FRAME_CLASS_NAME_MAX   32

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t frame_id;
    uint16_t chunk_index;
    uint16_t chunk_count;
    uint32_t flags;
    uint32_t chunk_offset;
    uint32_t chunk_size;
    uint32_t jpeg_size;
    int32_t class_id;
    float score;
    float inference_ms;
    float total_ms;
    char class_name[SDIO_FRAME_CLASS_NAME_MAX];
} sdio_frame_chunk_header_t;

static const char *TAG = "sdio_frame_rx";
static uint32_t s_frame_id;
static uint8_t *s_jpeg_buf;
static uint32_t s_jpeg_size;
static uint16_t s_chunks_received;
static uint16_t s_chunk_count;

static void frame_callback(uint32_t msg_id, const uint8_t *data, size_t data_len)
{
    if (data_len < sizeof(sdio_frame_chunk_header_t)) {
        return;
    }

    const sdio_frame_chunk_header_t *header = (const sdio_frame_chunk_header_t *)data;
    const uint8_t *chunk = data + header->header_size;

    if (header->magic != SDIO_FRAME_MAGIC || header->version != SDIO_FRAME_VERSION) {
        return;
    }
    if (header->header_size != sizeof(sdio_frame_chunk_header_t) ||
        data_len < header->header_size + header->chunk_size) {
        return;
    }

    if (s_jpeg_buf == NULL || s_frame_id != header->frame_id) {
        free(s_jpeg_buf);
        s_jpeg_buf = malloc(header->jpeg_size);
        if (s_jpeg_buf == NULL) {
            ESP_LOGE(TAG, "failed to allocate jpeg buffer");
            return;
        }
        s_frame_id = header->frame_id;
        s_jpeg_size = header->jpeg_size;
        s_chunks_received = 0;
        s_chunk_count = header->chunk_count;
    }

    if (header->chunk_offset + header->chunk_size > s_jpeg_size) {
        return;
    }

    memcpy(s_jpeg_buf + header->chunk_offset, chunk, header->chunk_size);
    s_chunks_received++;

    if (s_chunks_received == s_chunk_count) {
        ESP_LOGI(TAG,
                 "frame=%lu jpeg=%lu B class=%ld label=%s score=%.4f infer=%.2f ms total=%.2f ms",
                 (unsigned long)header->frame_id,
                 (unsigned long)s_jpeg_size,
                 (long)header->class_id,
                 header->class_name,
                 header->score,
                 header->inference_ms,
                 header->total_ms);

        /* 여기서 s_jpeg_buf를 Wi-Fi HTTP 응답, WebSocket, BLE 전송 등에 사용합니다. */
    }
}

esp_err_t app_frame_receiver_init(void)
{
    return esp_hosted_register_custom_callback(SDIO_FRAME_MSG_ID, frame_callback);
}
```

`app_frame_receiver_init()`는 C6 slave 초기화가 끝난 뒤 호출해야 합니다.
