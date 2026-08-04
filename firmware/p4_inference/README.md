# P4 inference

OV5647 프레임을 ESP-DL MobileNetV2 분류기로 추론하는 ESP32-P4 펌웨어입니다. P4 HTTP는 Ethernet으로 제공하며, 성공한 `/classify.jpg` 결과는 선택적으로 ESP-Hosted custom SDIO를 통해 C6에도 복사합니다.

| 경로 | 기능 |
| --- | --- |
| `/pic` | 요청 이후 촬영된 JPEG |
| `/record` | 요청 이후 촬영된 RGB565 원본 프레임 |
| `/classify.jpg` | 새 프레임을 5-crop 추론하고 JPEG와 결과 헤더 반환 |

`/classify.jpg` 응답에는 `X-Class-Index`, `X-Class-Label`, `X-Class-Score`, `X-Inference-Time-Ms`, `X-Inference-Total-Ms` 헤더가 포함됩니다. 현재 추론은 HTTP 요청 시에만 실행되며 백그라운드 주기 타이머는 없습니다.

```powershell
idf.py set-target esp32p4
idf.py build
idf.py -p COM_P4 flash monitor
```

빌드는 저장소 루트의 `model/artifacts/espdl/classifier_224_p4.espdl`과 짝이 맞는 `.json` 매니페스트를 검사합니다. 파일이 없거나 SHA-256·전처리·라벨·threshold 정보가 다르면 구성 단계에서 중단됩니다.
