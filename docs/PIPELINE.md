# 분류 파이프라인

## 런타임 구조

런타임 코드는 크게 두 부분으로 나뉩니다.

- `main/simple_video_server_example.c`
  - 카메라 초기화
  - V4L2 버퍼 dequeue/requeue
  - HTTP 엔드포인트 등록
  - JPEG 응답 처리
- `main/infer_bridge.cpp`
  - 분류 모델 로드
  - `224x224` 전처리
  - 모델 추론
  - top-1 후처리

## 엔드포인트

### `/pic`

최신 프레임을 `capture.jpg`로 반환합니다.

### `/record`

현재 카메라 버퍼의 원본 바이트를 반환합니다.

### `/classify.jpg`

현재 `RGB565` 프레임을 분류하고, 원본 프레임을 JPEG로 반환합니다.

응답 헤더는 다음과 같습니다.

- `X-Class-Index`
- `X-Class-Label`
- `X-Class-Score`
- `X-Inference-Time-Ms`
- `X-Inference-Total-Ms`

## 추론 경로

현재 추론 흐름은 다음과 같습니다.

```text
OV5647 sensor
  -> 800x800 frame
  -> app requests RGB565 output
  -> /classify.jpg dequeues one frame
  -> infer_bridge preprocesses 800x800 RGB565 -> 224x224 tensor
  -> MobileNetV2 0.35-width classifier runs with esp-dl
  -> top-1 class is returned through HTTP headers
  -> JPEG and classification metadata are chunked and sent to C6 over ESP-Hosted custom data
  -> the original frame is JPEG-encoded and returned as classify.jpg
  -> V4L2 buffer is queued back to the driver
```

## 모델 설정

공유 런타임 설정은 `main/infer_config.h`에 있습니다.

- 입력 너비: `224`
- 입력 높이: `224`
- 라벨 수: `2`
- 전처리 방식: `resize`
- 분류 응답 JPEG 품질: `60`

기본 라벨은 다음과 같습니다.

```text
no_human
human
```

이 순서는 `model/labels.txt`와 맞아야 합니다.

## 빌드 연동

분류 모델은 아래 파일에서 임베드됩니다.

```text
model/artifacts/espdl/classifier_224_p4.espdl
```

`main/CMakeLists.txt`는 이 파일이 없으면 빌드를 중단합니다. 링크 단계에서 늦게 실패하지 않고, 모델 파일 누락을 바로 확인하기 위한 처리입니다.

## 모델 작업 공간

`model/` 폴더는 ESP-IDF Python 환경과 분리된 별도 모델 작업 공간입니다.

작업 흐름은 다음과 같습니다.

```text
train_model.py
  -> artifacts/checkpoints/best.pt
export_onnx.py
  -> artifacts/onnx/classifier_224.onnx
quantize_espdl.py
  -> artifacts/espdl/classifier_224_p4.espdl
```

학습, ONNX 내보내기, 양자화, 펌웨어 전처리 경로는 모두 같은 `224x224` 리사이즈 정책과 ImageNet normalization 값을 사용합니다.

## SDIO 전송

`/classify.jpg` 처리 후 P4는 `main/sdio_frame_tx.c`를 통해 JPEG와 추론 결과를 C6로 전송합니다. JPEG는 ESP-Hosted custom data 한도를 넘을 수 있어서 `sdio_frame_chunk_header_t` 헤더가 붙은 여러 chunk로 나뉩니다.

C6 쪽 수신 callback 예시는 [sdio_frame_transfer.md](sdio_frame_transfer.md)에 있습니다.
