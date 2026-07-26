# 모델 학습 및 변환

이 폴더는 펌웨어에서 사용하는 `224x224` 2클래스 MobileNetV2 0.35-width 모델의 데이터 분할, 학습, 평가, ONNX 내보내기와 ESP-DL 양자화를 담당합니다.

## 환경 준비

프로젝트 로컬 가상 환경을 사용하세요. ESP-IDF Python 환경과 모델 학습 환경은 분리하는 것이 좋습니다.

```powershell
python -m venv .venv
& .\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
pip install torch torchvision --index-url https://download.pytorch.org/whl/cpu
pip install onnx pillow h5py esp-ppq ddgs
```

## 데이터 준비

원본 파인튜닝 이미지는 다음 두 클래스 폴더에 넣습니다.

```text
model/data/finetune_train/no_human/
model/data/finetune_train/human/
```

그다음 기본 분할 명령을 한 번 실행합니다.

```powershell
python model/prepare_finetune_split.py
```

기본 실행은 정확히 같은 이미지와 유사 이미지 그룹이 서로 다른 split으로 새지 않도록 묶어서 다음 활성 데이터를 생성합니다. 현재 확정된 데이터 수량은 다음과 같습니다.

- `model/data/train`: 539장, 약 70%, 학습용
- `model/data/val`: 115장, 약 15%, early stopping과 체크포인트 선택용
- `model/data/test`: 115장, 약 15%, 최종 평가용
- `model/data/calib`: 128장, train에서 클래스별 64장씩 복사한 ESP-DL 양자화 캘리브레이션용
- `model/data/split_manifest.json`: 분할, 파일 목록, SHA-256과 calib 선택 기록

원본 데이터가 바뀌어 기존 생성물을 다시 만들 때는 다음 명령을 사용합니다.

```powershell
python model/prepare_finetune_split.py --replace-generated
```

`--replace-generated`는 `train`, `val`, `test`, `calib`, `split_manifest.json`만 교체하며 `finetune_train` 원본은 삭제하지 않습니다. 제외할 원본 이미지가 있으면 `model/data/finetune_exclude.txt`에 `human/example.jpg` 같은 원본 기준 상대 경로를 한 줄에 하나씩 적습니다.

## 공식 0.35 사전학습 가중치

MobileNetV2 0.35-width는 공식 Keras ImageNet no-top 가중치를 사용합니다. `.h5`를 `h5py`로 읽어 PyTorch 텐서로 변환하므로 TensorFlow를 설치할 필요는 없습니다.

가중치를 미리 다운로드하고 SHA-256 및 변환을 확인하려면 다음 명령을 실행합니다.

```powershell
python model/download_pretrained.py
```

`train_model.py`도 캐시가 없으면 같은 공식 가중치를 자동으로 다운로드합니다. 오프라인에서는 캐시를 미리 준비하거나 다음처럼 파일을 직접 지정합니다.

```powershell
python model/train_model.py --pretrained-weights <mobilenet_v2_0.35_224_no_top.h5> --offline
```

기존 MobileNetV2 1.0-width 체크포인트는 0.35 구조와 호환되지 않습니다.

## 권장 2단계 학습

학습 split에는 파일을 새로 저장하지 않는 온라인 증강을 적용합니다. 좌우 반전은 50%, 작은 회전(`±6°`)과 확대·축소(`0.90~1.08배`)는 각각 30% 확률로 적용됩니다. 색상 처리는 원본 유지 15%, 밝기·대비·채도 변화 55%, 흑백 12%, 야간 18% 중 정확히 하나만 선택하므로 색채·흑백·야간 증강이 서로 중첩되지 않습니다. val/test/calib에는 이 증강을 적용하지 않습니다.

현재 저장소의 `.espdl`은 이 증강 프로파일을 추가하기 전에 학습된 모델입니다. 새 증강의 성능은 학습부터 ONNX export와 ESP-DL 양자화까지 다시 실행한 뒤 기존 test split으로 비교해야 합니다.

첫 단계에서는 공식 Keras 백본을 고정하고 새 2클래스 분류기부터 학습합니다.

```powershell
python model/train_model.py `
  --epochs 10 `
  --freeze-features `
  --output model/artifacts/checkpoints/stage1.pt
```

두 번째 단계에서는 같은 split의 1단계 체크포인트를 불러와 전체 네트워크를 낮은 학습률로 파인튜닝합니다.

```powershell
python model/train_model.py `
  --epochs 20 `
  --lr 1e-5 `
  --init-checkpoint model/artifacts/checkpoints/stage1.pt `
  --output model/artifacts/checkpoints/best.pt
```

`--init-checkpoint`와 `--resume`은 체크포인트에 기록된 `split_manifest.json` SHA-256이 현재 split과 같을 때만 허용됩니다. 다른 split에서 학습한 가중치로 현재 test가 오염되는 것을 막기 위한 검사입니다.

한 단계로 학습하려면 다음 명령도 사용할 수 있습니다.

```powershell
python model/train_model.py --epochs 20
```

## 변환과 최종 평가

학습 설정과 `best.pt`를 확정한 뒤 ONNX와 ESP-DL 모델을 생성합니다. 기본 양자화는 첫 bottleneck의 Conv/Clip만 INT16으로 두고 나머지 연산은 INT8로 처리하는 검증된 `mixed_int8_int16` 프로파일입니다.

```powershell
python model/export_onnx.py
python model/quantize_espdl.py
```

순수 INT8은 현재 데이터에서 정확도가 크게 붕괴했으므로 최종 펌웨어용 기본값이 아닙니다. 비교 진단이 꼭 필요할 때만 별도 출력 경로와 `--pure-int8`을 사용하세요.

```powershell
python model/quantize_espdl.py `
  --pure-int8 `
  --evaluate-split test `
  --output model/artifacts/espdl/pure_int8_diagnostic.espdl
```

mixed 프로파일의 PPQ fake-quant 결과와 five-crop threshold를 확인하려면 다음 명령을 사용합니다. `both`는 먼저 val만으로 threshold를 정한 다음 그 값을 test에 적용합니다.

```powershell
python model/quantize_espdl.py --evaluate-split both
```

최종 test 평가는 모든 설정을 확정한 뒤 마지막에 한 번만 실행합니다.

```powershell
python model/evaluate_test.py
```

평가 결과에는 loss, accuracy, balanced accuracy, confusion matrix, 클래스별 precision/recall/F1이 포함되며 이미지별 결과는 `model/artifacts/evaluation/test_predictions.csv`에 저장됩니다. test 결과를 보고 설정을 다시 고르면 test가 검증 데이터 역할을 하게 되므로, 그런 경우에는 새로운 독립 test 데이터가 필요합니다.

FP32에서 펌웨어의 five-crop 방식과 val에서 정한 기본 threshold를 모사하려면 다음 옵션을 사용합니다. 현재 test 원본이 `224x224`이므로 각 half-crop은 원본의 절반 영역을 잘라 다시 `224x224`로 리사이즈합니다. 예전 top-1 동작과 비교할 때만 `--human-threshold 0.5`를 추가합니다.

```powershell
python model/evaluate_test.py --five-crop
```

ONNX 단일 이미지 결과를 확인할 때는 고정 배치 크기 때문에 `--batch-size 1`이 필요합니다.

```powershell
python model/evaluate_test.py `
  --onnx model/artifacts/onnx/classifier_224.onnx `
  --batch-size 1
```

## 현재 기준 성능

동일한 115장 test split에서 확인한 결과입니다.

| 실행 경로 | 입력/판정 | 정확도 |
| --- | --- | ---: |
| PyTorch FP32 / ONNX FP32 | 단일 이미지 | 113/115 = 98.26% |
| FP32 | 5개 half-crop, human threshold 0.5 | 101/115 = 87.83% |
| PyTorch FP32 / ONNX FP32 | 5개 half-crop, val threshold `0.72482646` | 111/115 = 96.52% |
| PPQ 순수 ESP-DL INT8 | 단일 이미지 | 48/115 = 41.74% |
| PPQ mixed INT8/INT16 | 단일 이미지 | 111/115 = 96.52% |
| PPQ mixed INT8/INT16 | 5개 half-crop, human threshold 0.5 | 104/115 = 90.43% |
| PPQ mixed INT8/INT16 | 5개 half-crop, val에서 선택한 threshold `0.72482646` | 110/115 = 95.65% |

PPQ 수치는 내보낸 `.espdl`을 실제 보드에서 실행한 값이 아니라 ESP-PPQ의 fake-quant 그래프를 PC에서 실행한 시뮬레이션 결과입니다. 특히 현재 test는 RGB 이미지이고 five-crop도 `224x224` 원본에서 모사했으므로, 실제 카메라의 `800x800 RGB565` 입력, ESP-DL 전처리와 보드 런타임 정확도·지연시간은 별도로 측정해야 합니다.

표시상 `0.72482646`은 val에서만 고른 human threshold이며, 펌웨어와 정확히 같은 float32 값은 `0.72482645511627197`입니다. `quantize_espdl.py`가 이 값과 `>=` 규칙을 `.espdl.json`에 기록하고, CMake가 검증한 값을 컴파일 정의로 전달합니다. 펌웨어는 5개 crop 중 최대 human score가 이 값 이상일 때만 `human`을 반환합니다. 단, 실제 RGB565 보드 입력에 대한 실측 검증은 여전히 필요합니다.

## 폴더 구조

```text
model/
  labels.txt
  data/
    finetune_train/<label>/*
    finetune_exclude.txt
    train/<label>/*
    val/<label>/*
    test/<label>/*
    calib/<label>/*
    split_manifest.json
  artifacts/
    checkpoints/stage1.pt
    checkpoints/best.pt
    evaluation/test_predictions.csv
    onnx/classifier_224.onnx
    espdl/classifier_224_p4.espdl
    espdl/classifier_224_p4.espdl.json
```

`model/labels.txt`가 클래스 순서의 기준이며 기본 순서는 다음과 같습니다.

```text
no_human
human
```

이 순서는 체크포인트, ONNX, ESP-DL 매니페스트와 `main/infer_config.h`에서 같아야 합니다.

## 전처리와 산출물 호환성

Keras 가중치와 맞추기 위해 RGB 입력을 `x / 127.5 - 1`로 정규화하고 stride-2 계층에는 TensorFlow SAME 비대칭 패딩을 사용합니다. 리사이즈도 ESP-DL과 같은 `floor(output_index * source_size / output_size)` 좌표식을 사용합니다.

최종 펌웨어 산출물은 다음 두 파일입니다.

```text
model/artifacts/espdl/classifier_224_p4.espdl
model/artifacts/espdl/classifier_224_p4.espdl.json
```

펌웨어 빌드는 `.espdl.json`의 0.35 구조, 입력 크기, 전처리, 라벨 순서, five-crop 집계·human threshold, 기본 mixed INT8/INT16 프로파일과 실제 `.espdl` SHA-256을 검사합니다. 순수 INT8, 구형 또는 서로 다른 실행에서 생성된 산출물이 섞이면 빌드를 중단합니다.

학습과 평가도 `split_manifest.json` 및 train/val/test 전체 파일의 SHA-256을 검사합니다. 학습 후 split 또는 test 파일이 달라지면 평가를 거부합니다.
