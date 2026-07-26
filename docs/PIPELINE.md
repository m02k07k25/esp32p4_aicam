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
  -> infer_bridge defines four 400x400 quadrants plus one centered 400x400 crop
  -> each crop is resized directly to a 224x224 tensor
  -> MobileNetV2 0.35-width classifier runs five times with esp-dl
  -> the result from the crop with the highest human score is returned through HTTP headers
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

양자화 시 같은 경로에 `classifier_224_p4.espdl.json`도 생성됩니다. 기본 모델은 첫 bottleneck의 Conv/Clip만 INT16이고 나머지 연산은 INT8인 ESP32-P4 mixed 프로파일입니다. 빌드는 매니페스트에서 이 프로파일, 0.35 폭, 224 입력, Keras 전처리, 라벨 순서와 `.espdl` SHA-256을 검증합니다. 순수 INT8, 구형 모델 또는 서로 다른 실행에서 생성된 파일이 섞이면 빌드가 중단됩니다.

## 모델 작업 공간

`model/` 폴더는 ESP-IDF Python 환경과 분리된 별도 모델 작업 공간입니다.

원본 데이터는 `model/data/finetune_train/<label>/`에 둡니다. 기본 준비 명령은 중복·유사 이미지 그룹을 유지하며 활성 데이터를 70/15/15로 나누고, train에서 클래스별 64장을 calib로 복사합니다. 현재 수량은 train 539장, val 115장, test 115장, calib 128장입니다.

```powershell
python model/prepare_finetune_split.py
```

생성 경로는 다음과 같습니다.

```text
model/data/train/<label>/
model/data/val/<label>/
model/data/test/<label>/
model/data/calib/<label>/
model/data/split_manifest.json
```

원본이 바뀌어 생성 데이터를 교체할 때만 `python model/prepare_finetune_split.py --replace-generated`를 사용합니다. 이 옵션은 `finetune_train` 원본을 보존합니다.

공식 Keras MobileNetV2 0.35 ImageNet H5 가중치는 다음 명령으로 미리 내려받아 검증할 수 있으며, 캐시가 없으면 첫 학습 때도 자동으로 내려받습니다. H5를 직접 PyTorch 형식으로 변환하므로 TensorFlow는 필요하지 않습니다.

```powershell
python model/download_pretrained.py
```

권장 2단계 학습과 변환 흐름은 다음과 같습니다.

학습 이미지는 디스크에 증강본을 저장하지 않고 매 epoch 온라인으로 변환합니다. 좌우 반전·작은 회전·확대축소를 적용한 뒤 원본 색상, 색채 변화, 흑백, 야간 중 하나만 선택하며 val/test/calib는 변환하지 않습니다.

```powershell
python model/train_model.py --epochs 10 --freeze-features --output model/artifacts/checkpoints/stage1.pt
python model/train_model.py --epochs 20 --lr 1e-5 --init-checkpoint model/artifacts/checkpoints/stage1.pt --output model/artifacts/checkpoints/best.pt
python model/export_onnx.py
python model/quantize_espdl.py
python model/evaluate_test.py
```

`evaluate_test.py`는 학습과 변환 설정을 모두 확정한 뒤 마지막에 한 번만 실행합니다. test는 학습, early stopping, 하이퍼파라미터 선택이나 calib에 사용하지 않습니다.

기본 `quantize_espdl.py`는 mixed INT8/INT16 모델을 만듭니다. PPQ fake-quant 그래프에서 val threshold 선택과 test 적용까지 확인할 때는 다음 명령을 사용합니다.

```powershell
python model/quantize_espdl.py --evaluate-split both
```

`--pure-int8`은 정확도 붕괴를 비교하기 위한 진단 옵션일 뿐 최종 펌웨어 프로파일이 아닙니다.

산출물 흐름은 다음과 같습니다.

```text
prepare_finetune_split.py
  -> data/train, data/val, data/test, data/calib
  -> data/split_manifest.json
train_model.py
  -> artifacts/checkpoints/best.pt
export_onnx.py
  -> artifacts/onnx/classifier_224.onnx
quantize_espdl.py
  -> artifacts/espdl/classifier_224_p4.espdl
  -> artifacts/espdl/classifier_224_p4.espdl.json
evaluate_test.py
  -> artifacts/evaluation/test_predictions.csv
```

학습, ONNX 내보내기, 양자화, 펌웨어 전처리 경로는 모두 같은 `224x224` ESP-DL floor-index nearest-neighbor 리사이즈 정책과 Keras MobileNetV2의 RGB `x / 127.5 - 1` 정규화를 사용합니다.

## 현재 정확도 기준

현재 115장 test split 결과는 다음과 같습니다.

| 실행 경로 | 입력/판정 | 정확도 |
| --- | --- | ---: |
| FP32 / ONNX | 단일 이미지 | 113/115 = 98.26% |
| FP32 | `224x224` 원본의 5개 half-crop, threshold 0.5 | 101/115 = 87.83% |
| FP32 / ONNX | 5개 half-crop, val threshold `0.72482646` | 111/115 = 96.52% |
| PPQ 순수 ESP-DL INT8 | 단일 이미지 | 48/115 = 41.74% |
| PPQ mixed INT8/INT16 | 단일 이미지 | 111/115 = 96.52% |
| PPQ mixed INT8/INT16 | 5개 half-crop, threshold 0.5 | 104/115 = 90.43% |
| PPQ mixed INT8/INT16 | 5개 half-crop, val에서 선택한 threshold `0.72482646` | 110/115 = 95.65% |

PPQ 행은 실제 `.espdl`을 보드에서 실행한 결과가 아니라 PC의 ESP-PPQ fake-quant 시뮬레이션입니다. 또한 half-crop 평가는 `224x224` test 원본에서 펌웨어의 5-crop 공간 배치를 모사한 것입니다. 실제 런타임은 `800x800 RGB565` 카메라 프레임을 사용하므로 보드 실측이 별도로 필요합니다.

현재 펌웨어는 가장 높은 human score의 crop을 고르고, 그 점수가 val에서 정한 표시상 threshold `0.72482646` 이상일 때만 `human`을 반환합니다. 정확한 float32 값 `0.72482645511627197`과 `>=` 규칙은 `.espdl.json`에 저장되며 CMake가 검증 후 컴파일 정의로 전달합니다.

## SDIO 전송

`/classify.jpg` 처리 후 P4는 `main/sdio_frame_tx.c`를 통해 JPEG와 추론 결과를 C6로 전송합니다. JPEG는 ESP-Hosted custom data 한도를 넘을 수 있어서 `sdio_frame_chunk_header_t` 헤더가 붙은 여러 chunk로 나뉩니다.

C6 쪽 수신 callback 예시는 [sdio_frame_transfer.md](sdio_frame_transfer.md)에 있습니다.
