# 펌웨어 및 HTTP 동작

## P4 데이터 수집

`firmware/p4_data`는 ESP32-C6를 ESP Wi-Fi Remote로 사용해 SoftAP를 엽니다. PC를 `esp32p4-data`에 연결한 뒤 `http://192.168.4.1/`로 접속합니다.

카메라 경로는 요청 시 완료된 예전 buffer를 순환시킨 뒤 새 프레임을 기다립니다. JPEG 출력 buffer와 카메라는 mutex로 보호되므로 HTTP 요청과 GPIO 저장이 같은 프레임 메모리를 동시에 사용하지 않습니다.

SD 카드가 없거나 mount에 실패해도 HTTP 서버는 시작합니다.

| 경로 | SD 카드 | 설명 |
| --- | --- | --- |
| `/pic` | 불필요 | 요청 뒤 촬영한 JPEG |
| `/record` | 불필요 | 요청 뒤 촬영한 RGB565 원본 |
| `/capture` | 필요 | JPEG를 SD 카드에 저장 |
| `/captures` | 필요 | 저장 목록 |
| `/photo` | 필요 | 저장 사진 조회 |

GPIO1은 internal pull-up, active-low이며 GND에 연결하면 `/capture`와 같은 저장 경로를 실행합니다.

## P4 추론

`firmware/p4_inference`는 P4 Ethernet IP의 port 80에서 HTTP 서버를 엽니다.

| URL | 설명 |
| --- | --- |
| `/pic` | 요청 뒤 생성한 전체 화면 JPEG |
| `/record` | 요청 뒤 생성한 RGB565 원본 |
| `/classify.jpg` | 새 프레임을 five-crop 분류하고 JPEG와 결과 헤더 반환 |

분류 응답 헤더 예시는 다음과 같습니다.

```text
X-Class-Index: 1
X-Class-Label: human
X-Class-Score: 0.9821
X-Inference-Time-Ms: 1342.50
X-Inference-Total-Ms: 1362.10
```

`/classify.jpg`는 조회 전용입니다. HTTP 호출은 SDIO 전송을 만들지 않습니다. 독립적인 30초 task가 fresh frame을 분류하고, 결과가 human이며 C6가 READY일 때 점수가 가장 높은 crop을 224×224 JPEG로 만들어 전송합니다.

### 검출시각

P4는 SNTP를 실행하지 않습니다. ESP32-CAM Mesh 서버가 Wi-Fi/SNTP로 얻은
절대시각이 유일한 기준입니다. 시간 표본은 서버에서 BLE Mesh
`TIME_REQUEST(0xCA)/TIME_STATUS(0xCB)`로 C6에, 다시 별도 40바이트 SDIO
메시지로 P4에 전달됩니다. P4는 왕복 구간의 양 끝에서 읽은 monotonic clock과
서버의 수신/송신 Unix ms를 사용해 monotonic-to-Unix anchor를 만듭니다.

```text
ESP32-CAM SNTP
  → Mesh CA/CB
  → C6
  → 40B SDIO TIME SAMPLE
  → P4 monotonic anchor
  → fresh frame detected_at_ms
```

P4는 5분마다 갱신하고 표본을 15분까지만 사용합니다. 최초 표본 전, 표본
만료, SDIO 연결 해제/C6 재시작, 요청 ID 불일치나 비정상 왕복 표본에서는
`detected_at_ms=0`입니다. 서버 시계가 유효하지만 OPEN의 시각만 0이면 서버는
최초로 승인한 OPEN 수신시각을 `SERVER_TIME_RX_ESTIMATE`로 고정합니다. 서버
SNTP도 유효하지 않으면 시간은 0이고 `SERVER_TIME_UNKNOWN`입니다.

카메라 fresh frame을 dequeue한 직후, 추론과 JPEG 인코딩 전에 anchor에서
Unix epoch millisecond를 읽습니다. 따라서 한번 기록된 검출시각은 SDIO나
Mesh 지연으로 바뀌지 않습니다. P4 Ethernet은 이 문서의 HTTP endpoint용이며
시간 원본이 아닙니다.

HTTP 확인 명령:

```powershell
curl.exe -o pic.jpg http://P4_IP/pic
curl.exe -o classify.jpg -D classify.headers http://P4_IP/classify.jpg
```

두 번째 명령을 반복해도 BLE 이벤트가 중복 생성되지 않습니다.

## 기존 C6 Hosted

`firmware/c6_hosted`는 기존 Wi-Fi/HTTP Hosted 펌웨어입니다. 설정한 공유기에서 받은 C6 IP와 port `8081`을 사용합니다.

| URL | 설명 |
| --- | --- |
| `http://<C6_IP>:8081/` | 갱신 확인 페이지 |
| `http://<C6_IP>:8081/received.jpg` | 마지막 수신 JPEG |
| `http://<C6_IP>:8081/status` | 마지막 프레임 JSON metadata |

이 프로젝트는 `p4_inference ↔ c6_sdio_ble ↔ server` 이미지 이벤트 경로와 별개이며 그대로 보존합니다.

## C6 SDIO + BLE Mesh

`firmware/c6_sdio_ble`는 Wi-Fi/HTTP 서버를 실행하지 않습니다. P4의 SDIO v3 JPEG를 검증하고 generic Mesh server로 보내며, 다른 Mesh packet의 relay도 BLE Mesh stack에서 수행합니다.

C6가 READY가 되려면 다음 조건이 모두 필요합니다.

- 서버가 C6를 provisioning함
- Vendor Model `0x0002`, Company ID `0x02E5`에 AppKey가 bind됨
- publication 목적지가 서버 `0x0001`로 설정됨
- 진행 중인 JPEG가 없고 BLE transport가 정상임

각 C6에는 `main/device_identity.h`의 고유한 `C6_DEVICE_ID`(1~32766)가
필요합니다. 서버는 UUID에 실린 ID `N`을 읽어 Mesh unicast `N + 1`로
provisioning합니다. 따라서 장치별 ID가 다르면 C6 빌드도 달라야
하며, 주소는 부팅 순서에 따라 순차 배정되지 않습니다.

## Generic Mesh server

`firmware/server` core는 특정 WROOM32 보드나 Wi-Fi에 종속되지 않는 BLE Mesh
provisioner/receiver입니다. 현재 구성에서는 ESP32-CAM을 `esp32` target으로
빌드하고, 독립적인 Wi-Fi/SNTP adapter를 켜 기준시각도 제공합니다. 서버
firmware는 ESP32-CAM의 카메라 주변장치와 GPIO를 사용하지 않습니다.

- local address `0x0001`로 Mesh network 생성 또는 복원
- 최대 10개 C6를 `C6_DEVICE_ID + 1`의 결정적 주소로 자동 provisioning
- NetKey/AppKey 추가, Vendor Model bind, publication 설정
- 한 프레임씩 수신해 224×224 JPEG 재조립
- 누락 bitmap NACK, CRC/JPEG 검증, 최종 `COMPLETE`
- 완성 이벤트를 callback과 단일 USB console record로 전달
- SNTP 기준시각을 C6의 Mesh TIME 요청에 응답

기본 `CONFIG_SERVER_SERIAL_IMAGE_ENABLE=y`에서는 평소 USB console 하나로
텍스트 로그와 framed JPEG를 함께 보냅니다. `receive_images.py`가 같은 COM을
단독으로 열어 로그를 화면에 표시하고 검증된 사진을 `.jpg`/`.json` 및
`latest.jpg`/`latest.json`으로 저장합니다. 별도 UART는 필요 없으며
`idf.py monitor`와 동시에 실행하면 안 됩니다.

ESP32-CAM에서는 menuconfig의 `Mesh image server`에서 다음을 설정합니다.

```text
CONFIG_SERVER_SERIAL_IMAGE_ENABLE=y
CONFIG_SERVER_WIFI_SNTP_ENABLE=y
CONFIG_SERVER_WIFI_SSID="..."
CONFIG_SERVER_WIFI_PASSWORD="..."
CONFIG_SERVER_SNTP_SERVER="pool.ntp.org"
```

직렬 이미지 출력과 Wi-Fi/SNTP는 함께 동작합니다. 선택형 HTTP 이미지 출력을
사용할 때만 serial adapter를 끄고 `CONFIG_SERVER_HTTP_ENABLE=y`로 설정합니다.
즉 상호 배타적인 것은 serial/HTTP 출력이고, SNTP는 어느 출력과도 독립된
clock owner입니다. 실제 자격 증명은 추적되는 기본 설정 파일에 넣지 않습니다.
비동기 저장·업로드 코드는 callback이 반환되기 전에 JPEG를 자체 복사해야 합니다.

## COM 수신 기반 통합 테스트

휴대폰 Mesh 앱 없이 P4/C6 monitor와 서버 Python 수신기로 종단 확인이 가능합니다.

```text
Server Python: provisioned id=1 addr=0x0002 ... / IMAGE id=1 location=... src=0x0002 ...
C6:    mesh ready / OPEN accepted / COMPLETE
P4:    READY / human / ACCEPTED / SERVER_ACKED
```

시간 경로가 준비되면 서버에는 `SNTP synchronized`, P4에는
`server clock synchronized`가 출력됩니다. 서버가 아직 동기화되지 않은 동안에도
사진 전송은 중단하지 않으며 위 fallback 규칙에 따라 시각만 0이 될 수 있습니다.

서버 수신기는 다음처럼 실행합니다.

```powershell
python firmware/server/tools/receive_images.py --port COM_SERVER --baud 921600 --output received_images
```

선택적 위치 매핑은 `firmware/server/tools/locations.example.json`을 복사·편집하고
위 명령에 `--locations locations.json`을 추가합니다. 위치는 PC 출력
메타데이터일 뿐 BLE payload에는 추가되지 않습니다.

USB 데이터 케이블 하나로 세 펌웨어를 순차 flash해도 됩니다. 종단 시험에서는
P4/C6와 서버가 동시에 켜져 있어야 하지만 모두 PC COM에 연결할 필요는
없습니다. Python으로 사진과 로그를 받을 때는 서버의 USB/COM을 PC에 연결하고
P4와 C6는 외부 전원으로 구동할 수 있습니다.

P4의 `SERVER_ACKED`가 보이면 서버가 전체 JPEG를 재조립하고 CRC 및 224×224 JPEG 검증까지 완료한 것입니다. 자세한 배선, flash 순서와 장애 복구는 [P4-C6 SDIO 및 BLE Mesh 통합](sdio_frame_transfer.md)을 참고합니다.
