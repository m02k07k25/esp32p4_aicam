# ESP32-P4 AI Camera firmware workspace

ESP32-P4 카메라와 ESP32-C6, BLE Mesh 이미지 서버를 위한 ESP-IDF 5.5 작업공간입니다. 각 펌웨어는 독립 프로젝트이며 해당 폴더에서 별도로 빌드합니다.

| 프로젝트 | 대상 | 역할 | 기본 인터페이스 |
| --- | --- | --- | --- |
| [`firmware/p4_data`](firmware/p4_data/) | ESP32-P4 | 카메라 데이터 수집 및 SD 저장 | C6 Wi-Fi Remote SoftAP, `http://192.168.4.1/` |
| [`firmware/p4_inference`](firmware/p4_inference/) | ESP32-P4 | 30초 주기 human 추론, 224×224 JPEG 생성 | P4 Ethernet, `/pic`, `/record`, `/classify.jpg`, SDIO host |
| [`firmware/c6_hosted`](firmware/c6_hosted/) | ESP32-C6 | 기존 ESP-Hosted Wi-Fi/HTTP 보존 펌웨어 | C6 Wi-Fi, `http://<C6_IP>:8081/` |
| [`firmware/c6_sdio_ble`](firmware/c6_sdio_ble/) | ESP32-C6 | P4 JPEG 수신, BLE Mesh 송신 및 relay | SDIO slave + BLE Mesh source |
| [`firmware/server`](firmware/server/) | ESP32-CAM 등 BLE Mesh 지원 ESP 칩 | 자동 provisioning, 기준 SNTP 시각, JPEG 재조립 및 선택 재전송 요청 | BLE Mesh + Wi-Fi/SNTP + 단일 USB COM 로그/JPEG |

공식 이미지 이벤트 조합은 다음과 같습니다.

```text
P4 inference -- SDIO v3 --> C6 source -- BLE Mesh v2 --> generic server
       224x224 JPEG              relay 가능          재조립/NACK/COMPLETE

P4 monotonic anchor <-- 40B SDIO <-- C6 <-- CA/CB Mesh time <-- server SNTP
```

기존 `p4_data ↔ c6_hosted` 조합은 별도 데이터 수집 경로로 유지되며 신규 Mesh 프로토콜과 호환 대상으로 취급하지 않습니다.

## 모델

P4 추론 빌드는 아래 ESP-DL 모델과 매니페스트를 검사합니다.

```text
model/artifacts/espdl/classifier_224_p4.espdl
model/artifacts/espdl/classifier_224_p4.espdl.json
```

## 빌드와 플래시

각 명령의 `COM_*`은 실제 포트로 바꿉니다.

### P4 데이터 수집

```powershell
cd firmware/p4_data
idf.py set-target esp32p4
idf.py build
idf.py -p COM_P4 flash monitor
```

기본 SoftAP는 `esp32p4-data` / `12345678`이고 접속 주소는 `http://192.168.4.1/`입니다. SD 카드가 없어도 `/pic`, `/record`는 동작하지만 `/capture`, `/captures`, `/photo`와 GPIO1 저장은 SD 카드가 필요합니다.

### P4 추론

```powershell
cd firmware/p4_inference
idf.py set-target esp32p4
idf.py build
idf.py -p COM_P4 flash monitor
```

P4는 SNTP를 실행하지 않습니다. Ethernet은 `/pic`, `/record`,
`/classify.jpg` HTTP용이며 절대시각의 원본이 아닙니다. 기준시각은 아래
ESP32-CAM 서버가 SNTP로 얻고, BLE Mesh `TIME_REQUEST/TIME_STATUS`와 C6의
별도 40바이트 SDIO 메시지를 거쳐 P4에 전달합니다. P4는 이 표본을 monotonic
clock에 고정해 fresh frame 획득시각을 기록합니다.

`/classify.jpg`는 JPEG와 분류 헤더를 반환하는 조회 전용 endpoint입니다. 이 HTTP 요청은 SDIO/BLE 전송을 만들지 않으며, 실제 전송은 30초 감시 task에서 human일 때만 시작합니다.

### 기존 C6 Hosted Wi-Fi/HTTP

```powershell
cd firmware/c6_hosted
idf.py set-target esp32c6
idf.py menuconfig
idf.py build
idf.py -p COM_C6 flash monitor
```

Wi-Fi 자격 증명을 먼저 설정합니다. C6가 받은 IP를 확인한 뒤 `http://<C6_IP>:8081/`, `/received.jpg`, `/status`를 사용할 수 있습니다.

### C6 SDIO + BLE Mesh

각 물리 C6의 `firmware/c6_sdio_ble/main/device_identity.h`에서
`C6_DEVICE_ID`를 1~32766 사이의 고유한 값으로 정한 뒤 빌드합니다.
ID `N`의 Mesh unicast 주소는 항상 `N + 1`입니다.

```powershell
cd firmware/c6_sdio_ble
idf.py set-target esp32c6
idf.py build
idf.py -p COM_C6 flash monitor
```

서버는 Device UUID의 ID를 읽어 결정적 주소에 provisioning하고
AppKey bind와 publication 목적지 설정까지 수행합니다. 부팅 순서에
따라 `0x0002`부터 순차 배정하는 방식이 아닙니다.

### Generic Mesh server

서버 core는 WROOM32 전용이 아니지만, 이 구성에서는 Wi-Fi와 BLE를 지원하는
ESP32-CAM을 SNTP client이자 Mesh 기준시각 제공자로 사용합니다. ESP32-CAM의
target은 `esp32`이며 카메라 주변장치나 카메라 GPIO는 사용하지 않습니다.

```powershell
cd firmware/server
idf.py set-target esp32
# 또는 esp32c3, esp32c6, esp32c61, esp32h2, esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM_SERVER flash
```

`Mesh image server`에서 다음을 설정합니다. 실제 SSID와 비밀번호는 추적되는
`sdkconfig.defaults`에 넣지 않습니다.

```text
CONFIG_SERVER_SERIAL_IMAGE_ENABLE=y
CONFIG_SERVER_WIFI_SNTP_ENABLE=y
CONFIG_SERVER_WIFI_SSID="..."
CONFIG_SERVER_WIFI_PASSWORD="..."
CONFIG_SERVER_SNTP_SERVER="pool.ntp.org"
```

직렬 로그/JPEG와 Wi-Fi/SNTP는 동시에 동작합니다. Wi-Fi 재연결·scan은 BLE와
2.4 GHz radio를 공유해 일시적으로 Mesh 지연을 늘릴 수 있지만 SNTP 자체의
트래픽은 작고 USB COM과 충돌하지 않습니다.

서버의 local unicast 주소는 `0x0001`이고 C6 ID `N`의 주소는
`N + 1`입니다. 자동 관리 한도는 기본 10개 node이며 ID는
연속일 필요가 없습니다. 서버를 먼저 켜고 C6를 켜면 PB-ADV로
자동 provisioning하므로 휴대폰 Mesh 앱은 필요하지 않습니다.

기본 설정에서는 서버의 평소 USB console 하나에 텍스트 로그와 검증 가능한
JPEG record가 함께 나옵니다. `idf.py monitor` 대신 Python 도구가 같은 COM을
단독으로 열어 로그를 표시하고 사진을 저장합니다.

```powershell
python -m pip install pyserial
python firmware/server/tools/receive_images.py --port COM_SERVER --baud 921600 --output received_images
```

C6 ID를 설치 위치와 연결하려면
`firmware/server/tools/locations.example.json`을 복사해 현장에 맞게 편집한 뒤
`--locations locations.json`을 추가합니다. 위치 이름은 BLE로 전송하지
않고 PC에서만 매핑하므로, 장치를 옮겼을 때는 펌웨어 대신 JSON만
수정하면 됩니다.

정상 사진은 고유 이름의 `.jpg`/`.json`과 `received_images/latest.jpg`,
`latest.json`으로 저장됩니다. JSON에는 `device_id`, 선택적 `location`,
Mesh source 주소와 시각이 남습니다. 별도 UART나 USB-UART adapter는
필요 없습니다.
부팅 로그도 보려면 Python 실행 뒤 서버의 reset/EN을 누릅니다.

Wi-Fi HTTP 이미지 출력을 COM 대신 사용하려면 menuconfig에서
`CONFIG_SERVER_SERIAL_IMAGE_ENABLE=n`, `CONFIG_SERVER_HTTP_ENABLE=y`로
설정합니다. 직렬/HTTP 이미지 출력 adapter만 상호 배타적이며,
`CONFIG_SERVER_WIFI_SNTP_ENABLE`은 직렬 출력과 병행할 수 있습니다. 이
구성의 기본 시험은 단일 COM 방식입니다.

## 빠른 통합 확인

1. 서버 Python 수신기에서 provisioner 초기화, local address `0x0001`,
   `SNTP synchronized` 로그를 확인합니다.
2. 개별 COM 진단이 가능하면 C6 monitor에서 provisioning 완료, AppKey bind,
   publication `0x0001`, `ready`를 확인합니다.
3. 개별 COM 진단이 가능하면 P4 monitor에서 Ethernet IP와
   `server clock synchronized`를 확인합니다.
4. 브라우저나 curl로 조회 전용 HTTP 경로를 확인합니다.

   ```powershell
   curl.exe -o classify.jpg -D classify.headers http://P4_IP/classify.jpg
   ```

5. 카메라에 사람을 보여 주고 최대 30초 기다립니다. P4의 `ACCEPTED → SERVER_ACKED`, C6의 `OPEN → ACCEPT → DATA → END → COMPLETE`, Python의 `IMAGE ...` 로그를 확인합니다.
6. `received_images/latest.jpg`가 실제 224×224 JPEG인지 열어 보고 `latest.json`의 device ID/location/source/time metadata를 확인합니다.

USB 데이터 케이블이 하나뿐이어도 서버, C6, P4를 순서대로 flash할 수
있습니다. 종단 시험 중에는 P4/C6와 서버가 모두 동시에 켜져 있어야 하며,
사진과 로그를 PC로 받으려면 그때의 USB/COM은 서버에 연결합니다. P4와 C6는
외부 전원으로 구동해도 되므로 세 장치 모두를 PC의 COM에 동시에 연결할
필요는 없습니다.

서버가 누락 chunk를 발견하면 bitmap NACK을 보내고 C6는 표시된 chunk만 재전송합니다. `SERVER_ACKED`는 서버가 224×224 JPEG 재조립과 CRC 검증을 마치고 `COMPLETE`를 반환했다는 뜻입니다.

## Mesh 초기화 규칙

- 정상 펌웨어 업데이트에서 서버와 C6의 NVS를 보존했다면 다시 provisioning할 필요가 없습니다.
- 서버의 Mesh/NVS를 factory erase하면 기존 C6가 가진 NetKey, AppKey, DevKey 상태와 서버가 불일치합니다. 이때는 기존 C6도 node reset 또는 NVS erase한 뒤 모두 다시 provisioning합니다.
- `C6_DEVICE_ID`를 바꾸어도 이미 provisioning된 Mesh 주소는 저절로
  바뀌지 않습니다. 현재 서버에는 node 하나만 삭제하는 운영 CLI가
  없으므로, ID를 변경할 때는 서버 NVS와 등록된 모든 C6 Mesh
  NVS를 초기화한 후 모두 재provisioning합니다.

## 문서

- [펌웨어 및 HTTP 동작](docs/README.md)
- [카메라·추론·전송 파이프라인](docs/PIPELINE.md)
- [모델 학습과 변환](docs/model.md)
- [P4-C6 SDIO 및 BLE Mesh 통합](docs/sdio_frame_transfer.md)
