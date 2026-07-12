# 모델 학습 및 변환

아래 순서대로 실행하면 데이터 준비, 학습, ONNX 내보내기, ESP-DL 양자화까지 진행됩니다.

```powershell
python -m venv .venv
& .\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
pip install torch torchvision --index-url https://download.pytorch.org/whl/cpu
pip install onnx pillow esp-ppq ddgs

python model/prepare_coco_human_dataset.py
python model/train_model.py --epochs 20
python model/export_onnx.py
python model/quantize_espdl.py
```

산악 환경 이미지를 추가로 수집하려면 다음 명령을 사용합니다. 수집 결과는 바로 학습 데이터에
섞지 않고 `download/mountain_person/<label>`에 저장되므로, 라벨과 이미지 사용 권한을 검토한
뒤 `model/data/train`과 `model/data/val`로 나누어 복사하세요.

```powershell
python model/collect_mountain_dataset.py --per-class 700
```

이미 `.venv`와 데이터셋이 준비되어 있다면 아래 3개만 다시 실행하면 됩니다.

```powershell
python model/train_model.py --epochs 20
python model/export_onnx.py
python model/quantize_espdl.py
```

## 개요

이 폴더는 펌웨어에서 사용하는 `224x224` 2클래스 MobileNetV2 0.35-width 분류 모델을 학습하고 변환하는 작업 공간입니다.

최종 펌웨어가 사용하는 파일은 다음 경로에 생성됩니다.

```text
model/artifacts/espdl/classifier_224_p4.espdl
```

## 라벨

`labels.txt`가 클래스 순서의 기준입니다. 기본 라벨은 다음과 같습니다.

```text
no_human
human
```

이 순서는 펌웨어 쪽 라벨 정의와 맞아야 합니다.

## 폴더 구조

```text
model/
  labels.txt
  train_model.py
  export_onnx.py
  quantize_espdl.py
  data/
    train/<label>/*.jpg
    val/<label>/*.jpg
    calib/**/*.jpg
  artifacts/
    checkpoints/best.pt
    onnx/classifier_224.onnx
    espdl/classifier_224_p4.espdl
```

`data/train`과 `data/val`에는 `labels.txt`에 있는 라벨명과 같은 폴더가 있어야 합니다.

## 스크립트 역할

- `prepare_coco_human_dataset.py`: COCO에서 `human / no_human` 학습용 샘플을 내려받아 `model/data`에 배치합니다.
- `collect_mountain_dataset.py`: 산악 환경 후보 이미지를 수집하고 카메라 입력과 비슷한 RGB565 색상으로 변환합니다.
- `train_model.py`: MobileNetV2 0.35-width 모델을 학습하고 `artifacts/checkpoints/best.pt`를 저장합니다.
- `export_onnx.py`: 학습된 체크포인트를 `artifacts/onnx/classifier_224.onnx`로 내보냅니다.
- `quantize_espdl.py`: ONNX 모델을 ESP-DL용 int8 모델인 `artifacts/espdl/classifier_224_p4.espdl`로 변환합니다.

## 주의사항

모델 도구는 프로젝트 로컬 `.venv`에서 실행하세요. ESP-IDF가 사용하는 Python 환경과 섞지 않는 것이 좋습니다.

MobileNetV2 0.35-width는 torchvision의 기본 ImageNet pretrained weight와 구조가 달라서 랜덤 초기화로 학습됩니다. 기존 1.0-width 체크포인트는 이 설정에서 그대로 사용할 수 없으므로 모델 변경 후에는 학습부터 다시 실행해야 합니다.

COCO 데이터 준비는 기본적으로 이미지에서 가장 큰 사람 bbox가 전체 면적의 30% 이하인 샘플만
`human`으로 사용합니다. 다른 비율이 필요하면 `--max-person-area-ratio`로 조정할 수 있습니다.
