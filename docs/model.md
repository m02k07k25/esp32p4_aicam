# 모델 학습과 변환

`model/`은 펌웨어 빌드와 분리된 Python 작업공간입니다. 입력 데이터는 `model/data/finetune_train/<label>/`에 준비하고, 생성되는 split·체크포인트·ONNX·평가 CSV는 기본적으로 Git에 포함하지 않습니다.

## 환경 예시

```powershell
python -m venv .venv
& .\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

이 저장소는 Python 3.12.9, PyTorch 2.11.0, torchvision 0.26.0,
ONNX 1.17.0 및 ESP-PPQ 1.2.9 조합으로 검증했습니다. GPU 학습이 필요하면
PyTorch 공식 설치 선택기에서 같은 public version의 CUDA wheel을 먼저 설치한
뒤 `requirements.txt`를 적용합니다. 서버의 USB 로그/JPEG 수신기만 사용할
경우에는 전체 ML 환경 대신 `pyserial==3.5`만 설치해도 됩니다.

## 권장 순서

```powershell
python model/prepare_finetune_split.py
python model/download_pretrained.py
python model/train_model.py --epochs 10 --freeze-features --output model/artifacts/checkpoints/stage1.pt
python model/train_model.py --epochs 20 --lr 1e-5 --init-checkpoint model/artifacts/checkpoints/stage1.pt --output model/artifacts/checkpoints/best.pt
python model/export_onnx.py
python model/quantize_espdl.py
python model/evaluate_test.py
```

`prepare_finetune_split.py`는 train/val/test를 만들고 train에서 calibration 샘플을 선택합니다. 기존 생성 split을 의도적으로 교체할 때만 `--replace-generated`를 사용합니다.

최종 펌웨어 입력은 다음 파일 쌍입니다.

```text
model/artifacts/espdl/classifier_224_p4.espdl
model/artifacts/espdl/classifier_224_p4.espdl.json
```

매니페스트는 구조, 입력 크기, 라벨 순서, 전처리, five-crop 결정 규칙, mixed INT8/INT16 설정 및 `.espdl` SHA-256을 기록합니다. `firmware/p4_inference/main/classifier_manifest.cmake`가 빌드 전에 이 정보를 검증합니다.

현재 모델은 MobileNetV2 0.35-width, 224x224 입력, `no_human`/`human` 두 클래스와 mixed INT8/INT16 양자화를 사용합니다. `model/legacy/augment_dataset.py`는 이전 브랜치에만 있던 데이터 증강 도구를 보존한 것이며 기본 학습 흐름에서는 호출하지 않습니다.
