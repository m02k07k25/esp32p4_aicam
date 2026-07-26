| 지원 대상 | ESP32-P4 |
| --------- | -------- |

# Simple Video Server Classifier 예제

## 빠른 실행 순서

모델 파일이 이미 있으면 바로 빌드와 플래시를 진행할 수 있습니다.

```powershell
idf.py -p PORT build flash monitor
```

모델 파일을 새로 만들어야 한다면 `model/data/finetune_train/<label>/`에 원본 이미지를 넣고 아래 순서로 실행합니다. 첫 학습 단계는 공식 Keras MobileNetV2 0.35 백본을 고정하고, 두 번째 단계는 전체 네트워크를 파인튜닝합니다.

```powershell
python model/prepare_finetune_split.py
python model/download_pretrained.py
python model/train_model.py --epochs 10 --freeze-features --output model/artifacts/checkpoints/stage1.pt
python model/train_model.py --epochs 20 --lr 1e-5 --init-checkpoint model/artifacts/checkpoints/stage1.pt --output model/artifacts/checkpoints/best.pt
python model/export_onnx.py
python model/quantize_espdl.py
python model/evaluate_test.py
idf.py -p PORT build flash monitor
```

기본 데이터 생성 경로는 `model/data/train`, `val`, `test`, `calib`이며 현재 수량은 각각 539/115/115/128장입니다. 매니페스트는 `model/data/split_manifest.json`이고, `calib` 128장은 train에서 클래스별 64장씩 복사됩니다. 원본을 변경한 뒤 재생성할 때는 `python model/prepare_finetune_split.py --replace-generated`를 사용합니다.

공식 Keras MobileNetV2 0.35 H5 가중치는 TensorFlow 없이 PyTorch로 변환됩니다. `evaluate_test.py`는 학습 설정을 모두 확정한 뒤 마지막에 실행하세요. 모델 학습용 Python 환경, 자동 가중치 다운로드와 데이터 검증에 대한 자세한 설명은 [model.md](model.md)를 확인하세요.

## 개요

이 프로젝트는 ESP32-P4에서 카메라 프레임을 HTTP로 제공하고, 요청이 들어올 때 로컬 `224x224` 이미지 분류 모델을 실행합니다.

현재 기본 전제는 다음과 같습니다.

- 카메라 센서: `OV5647`
- 캡처 포맷: `800x800` 센서 프레임을 `RGB565`로 변환
- 분류기 입력: `224x224`
- 모델 타입: `classifier_224_p4.espdl`로 변환된 2클래스 MobileNetV2 0.35-width 분류기

## HTTP 엔드포인트

| URL | Method | 설명 |
| --- | ------ | ---- |
| `/pic` | `GET` | 최신 프레임을 `capture.jpg`로 반환합니다. |
| `/record` | `GET` | 현재 카메라 버퍼의 원본 프레임 바이트를 반환합니다. |
| `/classify.jpg` | `GET` | 현재 프레임을 분류하고, 원본 프레임 JPEG와 분류 결과 헤더를 반환합니다. |

`/classify.jpg` 응답에는 다음 헤더가 포함됩니다.

- `X-Class-Index`
- `X-Class-Label`
- `X-Class-Score`
- `X-Inference-Time-Ms`
- `X-Inference-Total-Ms`

분류 결과는 이미지 위에 오버레이로 그리지 않습니다. 응답 JPEG는 원본 프레임입니다.

## 모델 파일

펌웨어 빌드는 아래 ESP-DL 모델 파일을 직접 임베드합니다.

```text
model/artifacts/espdl/classifier_224_p4.espdl
model/artifacts/espdl/classifier_224_p4.espdl.json
```

기본 `quantize_espdl.py`는 첫 bottleneck만 INT16, 나머지는 INT8인 mixed 프로파일을 생성합니다. 두 파일이 없거나 매니페스트의 0.35 모델·mixed INT8/INT16 프로파일·전처리·라벨·SHA-256 정보가 실제 모델과 다르면 `main/CMakeLists.txt`에서 빌드가 중단됩니다. 펌웨어를 빌드하기 전에 현재 `model/` 아래 스크립트로 모델을 다시 생성해야 합니다.

## 현재 모델 검증 결과

동일한 115장 test split에서 측정한 값입니다.

| 실행 경로 | 판정 방식 | 정확도 |
| --- | --- | ---: |
| FP32 / ONNX | 단일 이미지 | 113/115 = 98.26% |
| FP32 | 5개 half-crop, threshold 0.5 | 101/115 = 87.83% |
| FP32 / ONNX | 5개 half-crop, val threshold `0.72482646` | 111/115 = 96.52% |
| PPQ 순수 ESP-DL INT8 | 단일 이미지 | 48/115 = 41.74% |
| PPQ mixed INT8/INT16 | 단일 이미지 | 111/115 = 96.52% |
| PPQ mixed INT8/INT16 | 5개 half-crop, threshold 0.5 | 104/115 = 90.43% |
| PPQ mixed INT8/INT16 | 5개 half-crop, val threshold `0.72482646` | 110/115 = 95.65% |

양자화 결과는 ESP-PPQ fake-quant 시뮬레이션입니다. 아직 실제 ESP32-P4에서 `800x800 RGB565` 카메라 입력으로 측정한 값이 아니므로 보드 정확도와 지연시간은 별도로 검증해야 합니다. val에서 정한 threshold는 표시상 `0.72482646`이고, 펌웨어와 정확히 같은 float32 값은 `0.72482645511627197`입니다. 이 값은 `.espdl.json`에 기록되고 CMake가 컴파일 정의로 전달하므로 현재 펌웨어에도 같은 `>=` 판정이 적용됩니다.

## 프로젝트 설정

설정 메뉴는 다음 명령으로 엽니다.

```powershell
idf.py menuconfig
```

중요한 기본 설정은 다음과 같습니다.

- 카메라 센서는 `OV5647` 경로를 사용합니다.
- 기본 센서 포맷은 `RAW8 800x800`입니다.
- 앱은 JPEG 인코딩과 분류 전에 비디오 장치 출력 포맷을 `RGB565`로 요청합니다.
- Wi-Fi Remote를 사용할 경우 ESP32-P4는 ESP32-C6와 ESP-Hosted SDIO로 통신합니다.
- `/classify.jpg` 처리 후 JPEG와 추론 결과를 C6로 보내는 SDIO custom data 프로토콜은 [sdio_frame_transfer.md](sdio_frame_transfer.md)를 확인하세요.

## 요청 예시

일반 JPEG를 가져옵니다.

```bash
curl http://esp-web.local/pic > capture.jpg
```

분류 결과 헤더를 확인합니다.

```bash
curl -i http://esp-web.local/classify.jpg
```

응답 헤더 예시는 다음과 같습니다.

```text
X-Class-Index: 1
X-Class-Label: human
X-Class-Score: 0.9821
X-Inference-Time-Ms: 24.73
X-Inference-Total-Ms: 38.46
```

## 참고

- 런타임 전처리는 `800x800` 프레임의 사분면 4개와 중앙 영역을 각각 `224x224`로 리사이즈해 5회 추론합니다.
- 펌웨어 정규화 값은 Keras MobileNetV2의 RGB `x / 127.5 - 1` 전처리와 맞춰져 있습니다.
- `model/labels.txt`와 `main/infer_config.h`의 2개 라벨 순서는 항상 같아야 합니다.
