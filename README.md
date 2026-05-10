| 지원 대상 | ESP32-P4 |
| --------- | -------- |

# Simple Video Server Classifier 예제

## 빠른 실행 순서

모델 파일이 이미 있으면 바로 빌드와 플래시를 진행할 수 있습니다.

```powershell
idf.py -p PORT build flash monitor
```

모델 파일을 새로 만들어야 한다면 먼저 아래 순서로 실행합니다.

```powershell
python model/train_model.py --epochs 20
python model/export_onnx.py
python model/quantize_espdl.py
idf.py -p PORT build flash monitor
```

모델 학습용 Python 환경과 데이터셋 준비 방법은 [model/README.md](model/README.md)를 확인하세요.

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
```

이 파일이 없으면 `main/CMakeLists.txt`에서 빌드가 중단됩니다. 펌웨어를 빌드하기 전에 `model/` 아래 스크립트로 모델을 생성해야 합니다.

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

- 런타임 전처리는 정사각형 카메라 프레임을 `224x224`로 리사이즈합니다.
- 펌웨어 정규화 값은 torchvision ImageNet normalization과 맞춰져 있습니다.
- `model/labels.txt`와 `main/infer_config.h`의 2개 라벨 순서는 항상 같아야 합니다.
