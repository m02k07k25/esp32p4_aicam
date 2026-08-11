# P4 inference

OV5647 프레임을 ESP-DL MobileNetV2 분류기로 추론하는 ESP32-P4 펌웨어입니다. P4 HTTP는 Ethernet으로 제공하고, 별도 감시 task가 30초마다 추론해 `human`일 때만 ESP-Hosted custom SDIO로 `c6_sdio_ble`에 JPEG를 보냅니다.

| 경로 | 기능 |
| --- | --- |
| `/pic` | 요청 이후 촬영된 JPEG |
| `/record` | 요청 이후 촬영된 RGB565 원본 프레임 |
| `/classify.jpg` | 새 프레임을 5-crop 추론하고 JPEG와 결과 헤더 반환 |

`/classify.jpg` 응답에는 `X-Class-Index`, `X-Class-Label`, `X-Class-Score`, `X-Inference-Time-Ms`, `X-Inference-Total-Ms` 헤더가 포함됩니다. 이 endpoint는 조회 전용이므로 SDIO 전송을 시작하지 않습니다.

자동 감시 경로는 30초마다 five-crop 추론을 실행합니다. 800×800 입력에서는 좌상단 `[0,0,400,400]`, 우상단 `[400,0,800,400]`, 좌하단 `[0,400,400,800]`, 우하단 `[400,400,800,800]`, 중앙 `[200,200,600,600]`을 평가합니다. 결과가 `human`이고 C6가 `READY`이면 이 중 **human 점수가 가장 높은 crop**을 모델과 같은 nearest-neighbor 방식으로 정확히 224×224 RGB565로 리사이즈하고 JPEG로 인코딩합니다. 품질은 60부터 5씩 20까지 낮추며, 30,720바이트 이하인 경우에만 SDIO로 전송합니다.

SDIO link task는 C6 연결이 끊기면 1, 2, 4, 8, 최대 10초 backoff로 재연결합니다. P4와 C6는 한 프레임만 in-flight로 유지하며, BLE 송신·NACK 복구 창이 끝난 뒤 돌아오는 다음 30초 감시 주기에 새 이미지를 보냅니다.

```powershell
idf.py set-target esp32p4
idf.py build
idf.py -p COM_P4 flash monitor
```

## Detection timestamp and SDIO v3

After Ethernet connects, the firmware starts SNTP without blocking camera or
inference startup. Configure the clock source in `menuconfig` or
`sdkconfig.defaults`:

```text
CONFIG_P4_INFERENCE_SNTP_ENABLE=y
CONFIG_P4_INFERENCE_SNTP_SERVER="pool.ntp.org"
```

Use a LAN NTP address when Internet NTP is unavailable. The periodic task
records Unix epoch milliseconds immediately after dequeuing the fresh camera
frame and before inference. Until the first successful SNTP update, or when
SNTP is disabled, `detected_at_ms` is zero.

SDIO protocol v3 sends only JPEG transport fields, the capture timestamp, and
the complete JPEG CRC32 in its 44-byte chunk header. The public submission API
is `sdio_frame_tx_submit_event(jpeg, jpeg_len, detected_at_ms)`. A transfer is
terminally successful only when C6 reports `SERVER_ACKED`; this means the Mesh
server positively acknowledged the reconstructed frame.

빌드는 저장소 루트의 `model/artifacts/espdl/classifier_224_p4.espdl`과 짝이 맞는 `.json` 매니페스트를 검사합니다. 파일이 없거나 SHA-256·전처리·라벨·threshold 정보가 다르면 구성 단계에서 중단됩니다.
