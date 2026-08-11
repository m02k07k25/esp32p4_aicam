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

P4는 Ethernet 연결 직후 SNTP를 비동기로 시작합니다. 카메라 fresh frame을 dequeue한 직후, 추론과 JPEG 인코딩 전에 Unix epoch millisecond를 기록합니다. 따라서 SDIO나 Mesh가 막혀도 이미 기록된 검출시각은 바뀌지 않습니다.

```text
CONFIG_P4_INFERENCE_SNTP_ENABLE=y
CONFIG_P4_INFERENCE_SNTP_SERVER="pool.ntp.org"
```

SNTP에 계속 연결할 필요는 없지만 재부팅 뒤에는 다시 동기화해야 합니다. 최초 동기화 전이나 SNTP 비활성 상태에서는 `detected_at_ms=0`을 보냅니다. 서버에 유효한 clock provider가 있으면 최초 `OPEN` 수신 시각을 추정값으로 고정해 `SERVER_TIME_RX_ESTIMATE`와 함께 제공하고, 없으면 시간 `0`과 `SERVER_TIME_UNKNOWN`을 제공합니다.

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

C6 주소는 firmware 상수가 아닙니다. 서버가 `0x0002`부터 자동 할당하므로 C6마다 다른 바이너리를 만들지 않습니다.

## Generic Mesh server

`firmware/server`는 특정 WROOM32 보드나 Wi-Fi에 의존하지 않는 BLE Mesh provisioner/receiver입니다. 기본 firmware는 다음 역할만 담당합니다.

- local address `0x0001`로 Mesh network 생성 또는 복원
- 최대 10개 C6를 `0x0002`부터 자동 provisioning
- NetKey/AppKey 추가, Vendor Model bind, publication 설정
- 한 프레임씩 수신해 224×224 JPEG 재조립
- 누락 bitmap NACK, CRC/JPEG 검증, 최종 `COMPLETE`
- 완성 이벤트를 callback과 단일 USB console record로 전달

기본 `CONFIG_SERVER_SERIAL_IMAGE_ENABLE=y`에서는 평소 USB console 하나로
텍스트 로그와 framed JPEG를 함께 보냅니다. `receive_images.py`가 같은 COM을
단독으로 열어 로그를 화면에 표시하고 검증된 사진을 `.jpg`/`.json` 및
`latest.jpg`/`latest.json`으로 저장합니다. 별도 UART는 필요 없으며
`idf.py monitor`와 동시에 실행하면 안 됩니다.

Wi-Fi 지원 target에서는 serial adapter를 끄고 `CONFIG_SERVER_HTTP_ENABLE`을
선택해 one-shot SNTP와 `/latest.jpg`, `/latest.json` adapter를 대신 사용할 수
있습니다. 두 adapter는 상호 배타적입니다. 비동기 저장·업로드 코드는 callback이
반환되기 전에 JPEG를 자체 복사해야 합니다.

## COM 수신 기반 통합 테스트

휴대폰 Mesh 앱 없이 P4/C6 monitor와 서버 Python 수신기로 종단 확인이 가능합니다.

```text
Server Python: provisioned addr=0x0002 ... / IMAGE src=0x0002 ... file=...
C6:    mesh ready / OPEN accepted / COMPLETE
P4:    READY / human / ACCEPTED / SERVER_ACKED
```

서버 수신기는 다음처럼 실행합니다.

```powershell
python firmware/server/tools/receive_images.py --port COM_SERVER --baud 921600 --output received_images
```

P4의 `SERVER_ACKED`가 보이면 서버가 전체 JPEG를 재조립하고 CRC 및 224×224 JPEG 검증까지 완료한 것입니다. 자세한 배선, flash 순서와 장애 복구는 [P4-C6 SDIO 및 BLE Mesh 통합](sdio_frame_transfer.md)을 참고합니다.
