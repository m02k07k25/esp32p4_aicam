# ESP32-P4 AI Camera + BLE Mesh

ESP32-P4에서 사람을 감지하고, 감지된 `224x224` JPEG를 ESP32-C6과 BLE
Mesh를 거쳐 ESP32 서버로 보내는 프로젝트입니다. 서버에 연결된 PC는 USB
COM 하나로 로그와 사진을 받고, 로컬 웹 화면에서 최근 이벤트를 확인합니다.

이 저장소에는 서로 목적이 다른 두 경로가 있습니다.

1. **모델 데이터 수집용 경로**: `p4_data + c6_hosted`
2. **실제 추론·전송 경로**: `p4_inference + c6_sdio_ble + server`

데이터 수집용 펌웨어는 학습 사진을 모으거나 카메라를 개발할 때만 사용합니다.
실제 추론 시스템을 설치할 때는 `p4_data`와 `c6_hosted`를 플래시하지 않습니다.

## 전체 구성

### 1. 모델 데이터 수집용 펌웨어

```text
Browser/SD card <-- Wi-Fi via C6 <-- ESP32-P4 camera
                       c6_hosted       p4_data
```

| 폴더 | 대상 | 역할 |
| --- | --- | --- |
| [`firmware/p4_data`](firmware/p4_data/) | ESP32-P4 | OV5647 프레임 확인, JPEG 촬영, SD 카드 데이터 수집 |
| [`firmware/c6_hosted`](firmware/c6_hosted/) | ESP32-C6 | ESP-Hosted Wi-Fi 보조 칩 및 기존 SDIO/HTTP 확인 기능 |

`p4_data`의 기본 촬영 화면은 `http://192.168.4.1/`입니다. 이 조합은 모델
데이터 수집 및 카메라 개발을 위한 독립 경로이며 아래 BLE Mesh 프로토콜과
호환되지 않습니다.

### 2. 실제 추론·BLE Mesh 파이프라인

```text
ESP32-P4              ESP32-C6                 ESP32 server          Laptop
p4_inference --SDIO--> c6_sdio_ble --BLE Mesh--> server --USB COM--> Python
  human 감지           JPEG 송신/relay          재조립/NACK           로그/사진/시간
  224x224 JPEG                                  COMPLETE              웹 뷰어

Laptop time --USB--> server --BLE Mesh--> C6 --SDIO--> P4 monotonic clock
```

| 폴더 | 대상 | 역할 |
| --- | --- | --- |
| [`firmware/p4_inference`](firmware/p4_inference/) | ESP32-P4 | 10초 주기 five-crop 추론, 사람 감지, `224x224` JPEG 생성, SDIO host |
| [`firmware/c6_sdio_ble`](firmware/c6_sdio_ble/) | ESP32-C6 | SDIO JPEG 검증, BLE Mesh source, 누락 chunk 재전송, Mesh relay |
| [`firmware/server`](firmware/server/) | ESP32-CAM 등 BLE Mesh 지원 ESP | Provisioner/Gateway, JPEG 재조립, bitmap NACK, USB 로그·사진·시간 |
| [`firmware/esp32_ble`](firmware/esp32_ble/) | 일반 ESP32 | 선택형 순수 BLE Mesh relay; JPEG를 해석하거나 재조립하지 않음 |

공식 실행 조합은 다음 세 프로젝트입니다.

```text
firmware/p4_inference
firmware/c6_sdio_ble
firmware/server
```

통신 거리를 늘려야 할 때만 `firmware/esp32_ble` 노드를 추가합니다.

## 저장소 폴더

| 폴더 | 설명 |
| --- | --- |
| [`firmware/`](firmware/) | 보드별 독립 ESP-IDF 프로젝트 |
| [`firmware/components`](firmware/components/) | C6와 서버가 함께 사용하는 BLE Mesh wire protocol codec; 단독으로 플래시하지 않음 |
| [`model/`](model/) | 데이터 분할, MobileNetV2 학습, 평가, ONNX 변환, ESP-DL 양자화 도구 |
| [`docs/`](docs/) | HTTP, 카메라, SDIO, BLE Mesh 및 모델 상세 문서 |

`model/data`, 체크포인트, ONNX, 평가 결과, `received_images`와 `recovery`의
촬영 이미지는 로컬 작업 데이터이므로 기본적으로 Git에 포함하지 않습니다.
펌웨어 빌드에 필요한 최종 ESP-DL 모델과 매니페스트만 추적합니다.

## 개발 환경

현재 검증된 환경은 다음과 같습니다.

| 항목 | 검증 버전 |
| --- | --- |
| ESP-IDF | `v5.5.x` |
| Python | `3.12.9` |
| PyTorch | `2.11.0` |
| torchvision | `0.26.0` |
| ESP-PPQ | `1.2.9` |
| pyserial | `3.5` |

모든 펌웨어는 ESP-IDF 프로젝트가 서로 독립되어 있으므로 반드시 해당 폴더에서
빌드합니다. ESP-IDF 5.5 환경을 먼저 활성화한 뒤 `idf.py --version`으로 버전을
확인하세요.

### Python 설치

서버에서 로그와 이미지만 받을 경우 `pyserial`만 필요합니다.

```powershell
python -m pip install pyserial==3.5
```

모델 학습·평가·양자화까지 수행하려면 저장소 루트에서 전체 의존성을 설치합니다.

```powershell
python -m venv .venv
& .\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

`requirements.txt`의 PyTorch 버전은 CPU/CUDA 공통 공개 버전으로 고정되어
있습니다. CUDA 전용 wheel이 필요한 PC에서는 [PyTorch 설치 선택기](https://pytorch.org/get-started/locally/)의
명령으로 같은 `torch 2.11.0` / `torchvision 0.26.0` 조합을 먼저 설치한 뒤
나머지 요구사항을 설치하세요.

## A. 모델 데이터 수집

실제 추론 펌웨어를 이미 사용할 수 있다면 이 단계는 건너뛰어도 됩니다.

### A-1. C6 Hosted

```powershell
cd firmware/c6_hosted
idf.py set-target esp32c6
idf.py menuconfig
idf.py build
idf.py -p COM_C6 flash monitor
```

필요하면 `Example Configuration > Wi-Fi default config`에서 Wi-Fi 자격 증명을
설정합니다. 기존 C6 HTTP 확인 기능은 `http://<C6_IP>:8081/`을 사용합니다.

### A-2. P4 데이터 수집

```powershell
cd firmware/p4_data
idf.py set-target esp32p4
idf.py build
idf.py -p COM_P4 flash monitor
```

기본 SoftAP는 `esp32p4-data` / `12345678`이며 촬영 화면은
`http://192.168.4.1/`입니다.

| 경로 | 기능 | SD 카드 |
| --- | --- | --- |
| `/pic` | 요청 후 촬영된 JPEG 반환 | 불필요 |
| `/record` | 요청 후 촬영된 원본 프레임 반환 | 불필요 |
| `/capture` | 새 JPEG 저장 | 필요 |
| `/captures` | 저장 사진 목록 | 필요 |
| `/photo?name=IMG00001.JPG` | 저장 사진 조회 | 필요 |

GPIO1을 GND로 당겨도 SD 카드 촬영을 실행할 수 있습니다.

## B. 실제 추론 시스템 설치

아래 순서대로 한 번씩 플래시한 뒤, 정상 운용 중에는 서버의 USB만 PC에
연결해도 됩니다. P4와 C6는 외부 전원으로 동작할 수 있습니다.

### B-1. 장치 ID 설정

C6마다 [`firmware/c6_sdio_ble/main/device_identity.h`](firmware/c6_sdio_ble/main/device_identity.h)의
`C6_DEVICE_ID`를 `1..32766` 범위에서 고유하게 설정합니다.

```c
#define C6_DEVICE_ID 1U
```

- 서버 주소: `0x0001`
- 장치 ID `N`의 Mesh 주소: `N + 1`
- ID는 모든 C6 source와 ESP32 relay 사이에서 중복되면 안 됨
- 위치 이름은 펌웨어가 아니라
  [`firmware/server/tools/locations.json`](firmware/server/tools/locations.json)에서 관리

현재 예제 위치 매핑은 장치 ID `1`을 `아차산`으로 표시합니다.

### B-2. Mesh 서버

현재 실기기 구성은 ESP32-CAM을 사용하지만 카메라 GPIO나 Wi-Fi는 사용하지
않습니다. 기본 target은 `esp32`입니다.

```powershell
cd firmware/server
idf.py set-target esp32
idf.py build
idf.py -p COM_SERVER flash
```

서버 core는 필요하면 `esp32c3`, `esp32c6`, `esp32c61`, `esp32h2`, `esp32s3`로도
빌드할 수 있습니다. 현재 기본 설정은 다음과 같습니다.

```text
CONFIG_SERVER_SERIAL_IMAGE_ENABLE=y
CONFIG_SERVER_SERIAL_TIME_ENABLE=y
CONFIG_SERVER_WIFI_SNTP_ENABLE=n
```

### B-3. C6 SDIO + BLE Mesh

```powershell
cd firmware/c6_sdio_ble
idf.py set-target esp32c6
idf.py build
idf.py -p COM_C6 flash monitor
```

서버를 먼저 켜고 C6를 부팅하면 서버가 PB-ADV로 자동 provisioning하고 AppKey
bind, publication 목적지 `0x0001`, TTL 및 relay 설정을 수행합니다. 휴대폰 Mesh
앱은 필요하지 않습니다.

### B-4. P4 추론

```powershell
cd firmware/p4_inference
idf.py set-target esp32p4
idf.py build
idf.py -p COM_P4 flash monitor
```

P4는 10초마다 추론하고 `human >= 0.75`일 때만 최대 `30,720`바이트의
`224x224` JPEG를 C6로 보냅니다. `/classify.jpg`는 조회 전용이므로 HTTP 요청만으로
SDIO/BLE 전송이 시작되지는 않습니다.

P4 Ethernet은 `/pic`, `/record`, `/classify.jpg` 확인용이며 절대시각의 원본이
아닙니다. 야외 기본 구성에서는 서버에 연결된 노트북이 기준시각을 제공합니다.

### B-5. 선택형 ESP32 relay

직접 통신 거리가 부족할 때만 일반 ESP32에 relay 펌웨어를 설치합니다.

```powershell
cd firmware/esp32_ble
idf.py build
idf.py -p COM_RELAY flash monitor
```

플래시하기 전에 [`firmware/esp32_ble/main/device_identity.h`](firmware/esp32_ble/main/device_identity.h)의
`ESP32_BLE_DEVICE_ID`를 다른 C6/relay와 겹치지 않게 설정합니다. relay는 BLE
Mesh Network Layer에서 패킷을 바로 전달하며 JPEG 전체를 재조립하지 않습니다.

## PC에서 로그와 이미지 받기

서버의 USB COM은 한 프로그램만 열 수 있습니다. 사진을 받을 때는
`idf.py monitor`를 닫고 아래 Python 도구를 실행합니다.

```powershell
python firmware/server/tools/receive_images.py --port COM_SERVER --baud 921600 --output received_images
```

이 프로세스 하나가 다음을 모두 수행합니다.

- ESP-IDF 로그 실시간 표시
- 검증 완료된 JPEG와 JSON metadata 저장
- `received_images/latest.jpg`, `latest.json` 갱신
- `http://127.0.0.1:8000/` 로컬 웹 화면 실행
- 노트북 Unix 시각을 시작 직후와 1분마다 서버에 전달

이미지 송수신 중에는 시각 갱신을 건너뜁니다. 서버는 마지막 표본과
`esp_timer`를 이용해 시간을 계속 계산하며, 5분 동안 새 표본이 없으면 시각을
유효하지 않은 것으로 처리합니다. 기본 운용에는 Wi-Fi와 SNTP가 필요 없습니다.

브라우저를 자동으로 열지 않으려면 `--no-browser`, 웹 화면을 끄려면
`--no-http`, 노트북 시각 전송을 끄려면 `--no-time-sync`를 추가합니다.

## 정상 동작 확인

1. 서버 Python 로그에서 local address `0x0001`과
   `laptop clock synchronized`를 확인합니다.
2. C6가 처음 등록되면 provisioning, AppKey bind, publication `0x0001`,
   `ready` 로그를 확인합니다.
3. P4에서 `server clock synchronized`와 SDIO link를 확인합니다.
4. 카메라에 사람을 보여 주고 최대 10초 기다립니다.
5. P4의 `ACCEPTED -> SERVER_ACKED`, C6의
   `OPEN -> ACCEPT -> DATA -> END -> COMPLETE`, Python의 `IMAGE` 로그를
   확인합니다.
6. `http://127.0.0.1:8000/` 또는 `received_images/latest.jpg`에서 사진을
   확인합니다.

서버는 누락 chunk만 bitmap NACK으로 요청합니다. C6는 해당 chunk만 다시
보내고, 서버가 JPEG 길이·CRC·SOI/EOI·`224x224` 규격을 모두 검증한 뒤에만
`COMPLETE`를 반환합니다.

## 모델 학습과 펌웨어 모델 교체

`model/`은 펌웨어와 분리된 Python 작업공간입니다. 기본 순서는 다음과 같습니다.

```powershell
python model/prepare_finetune_split.py
python model/download_pretrained.py
python model/train_model.py --epochs 10 --freeze-features --output model/artifacts/checkpoints/stage1.pt
python model/train_model.py --epochs 20 --lr 1e-5 --init-checkpoint model/artifacts/checkpoints/stage1.pt --output model/artifacts/checkpoints/best.pt
python model/export_onnx.py
python model/quantize_espdl.py
python model/evaluate_test.py
```

현재 모델은 MobileNetV2 `width_mult=0.35`, 입력 `224x224`, 클래스
`no_human`/`human`, mixed INT8/INT16 양자화를 사용합니다. P4 빌드에 실제로
들어가는 파일은 다음 두 개입니다.

```text
model/artifacts/espdl/classifier_224_p4.espdl
model/artifacts/espdl/classifier_224_p4.espdl.json
```

매니페스트에는 입력 크기, 라벨 순서, 전처리, threshold, 양자화 설정과 모델
SHA-256이 들어 있습니다. P4 빌드는 두 파일의 일치 여부를 자동 검증합니다.
자세한 절차는 [모델 학습과 변환 문서](docs/model.md)를 참고하세요.

## Mesh 초기화와 복구

- 일반 펌웨어 업데이트에서는 서버와 노드의 NVS를 지우지 않습니다.
- 서버 Mesh/NVS를 초기화하면 기존 C6/relay의 NetKey·AppKey·DevKey와 맞지
  않으므로 모든 등록 노드도 reset 또는 NVS erase 후 다시 provisioning합니다.
- 이미 provisioning된 뒤 장치 ID만 바꾸어도 저장된 Mesh 주소는 자동으로
  바뀌지 않습니다. ID 변경 시 서버와 관련 노드를 함께 초기화합니다.
- 서버 Python 수신기는 실행 중 로그까지 대신 표시하므로 동일 COM에서
  `idf.py monitor`를 동시에 실행하지 않습니다.

## 상세 문서

- [펌웨어 및 HTTP 동작](docs/README.md)
- [카메라·추론·전송 파이프라인](docs/PIPELINE.md)
- [모델 학습과 변환](docs/model.md)
- [P4-C6 SDIO 및 BLE Mesh 통합](docs/sdio_frame_transfer.md)
