# P4-C6 SDIO 및 BLE Mesh 통합

SDIO는 P4와 C6 사이의 ESP-Hosted 통신 버스이며 SD 카드를 뜻하지 않습니다. 공식 이미지 이벤트 조합은 다음 세 프로젝트입니다.

| 장치 | 프로젝트 | 역할 | 주소/인터페이스 |
| --- | --- | --- | --- |
| ESP32-P4 | `firmware/p4_inference` | human 추론, server-derived 시각 기록, 224×224 JPEG, SDIO host | Ethernet HTTP `:80` |
| ESP32-C6 | `firmware/c6_sdio_ble` | SDIO slave, JPEG 검증, Mesh source/relay | ID `N` → unicast `N + 1` |
| ESP32-CAM 등 BLE Mesh ESP | `firmware/server` | provisioner, 노트북 기준시각, 이미지 sink, 재조립/NACK | local unicast `0x0001`, 양방향 USB console |

기존 `firmware/c6_hosted`는 Wi-Fi/HTTP 수신기와 SDIO v1을 보존하며 이 조합의 상대 펌웨어가 아닙니다.

## SDIO 배선

ESP32-P4 Function EV Board 기본 설정은 다음과 같습니다. 같은 번호끼리 잇는 것이 아니라 같은 SDIO 신호끼리 연결합니다.

| 신호 | P4 GPIO | C6 GPIO |
| --- | ---: | ---: |
| CLK | 18 | 19 |
| CMD | 19 | 18 |
| D0 | 14 | 20 |
| D1 | 15 | 21 |
| D2 | 16 | 22 |
| D3 | 17 | 23 |
| C6 reset/EN | 54 | EN |

- 두 보드의 GND를 반드시 공통으로 연결합니다.
- 신호는 3.3V logic level을 사용하고 GPIO에서 상대 보드를 전원 공급하지 않습니다.
- CMD와 DAT0~DAT3에는 보드에 없는 경우 적절한 외부 pull-up을 사용합니다.
- 배선을 바꾸기 전 두 보드의 전원을 끕니다.

Mesh server는 C6와 무선으로 통신하므로 별도 데이터 배선이 없습니다.

## 시간 동기화

ESP32-CAM Mesh 서버가 Mesh의 wall-clock authority입니다. 기본 구성에서는 PC
Python 수신기가 실행 직후와 1분마다 노트북 Unix ms를 USB console RX로 보내고,
서버는 이를 monotonic clock에 고정합니다. 이미지 송수신 중인 갱신은 건너뛰며,
5분 동안 유효한 갱신이 없으면 만료됩니다.
P4는 SNTP를 실행하지 않으며 Ethernet 연결은 `/pic`, `/record`,
`/classify.jpg` HTTP용이지 시각 원본이 아닙니다.

```text
Laptop OS clock → 28B CRC UART packet → ESP32-CAM server
  → TIME_STATUS 0xCB(server RX/TX Unix ms)
  → C6
  → 40B SDIO TIME SAMPLE
  → P4 monotonic-to-Unix anchor
  → fresh DQBUF detected_at_ms
```

P4가 C6 READY/idle 상태에서 40바이트 SDIO `TIME QUERY`를 보내면 C6가
4바이트 Mesh `TIME_REQUEST(0xCA)`로 server publication 주소 `0x0001`에
전달합니다. 서버는 요청 수신 Unix ms와 응답 queue 직전 Unix ms를 담은
24바이트 `TIME_STATUS(0xCB)`를 보냅니다. C6는 이를 인증·검증해 40바이트
SDIO `TIME SAMPLE`로 돌려주며 자체 절대시각을 만들지 않습니다.

P4는 QUERY 송신/응답 수신 monotonic 시각과 서버의 RX/TX 표본으로 왕복 지연을
검사하고 midpoint anchor를 계산합니다. anchor는 5분마다 갱신하고 15분이
지나면 사용하지 않습니다. SDIO transport down, C6 재시작, stale/foreign
request ID, 잘못된 timestamp 순서 또는 10초를 넘긴 round trip은 표본을
무효화하거나 거절합니다. fresh camera frame dequeue 직후 이 anchor에서 Unix
epoch millisecond를 읽으므로 이후 추론·SDIO·Mesh 병목이 검출시각을 바꾸지
않습니다.

- 서버의 노트북 시각 표본이 유효함: TIME_STATUS `OK`, P4가 server-derived
  `detected_at_ms`를 frame 획득 시 붙임
- 서버가 부팅 후 아직 동기화되지 않았거나 server clock이 만료됨:
  TIME_STATUS `UNAVAILABLE`과 두 개의 0 timestamp
- P4 anchor가 없거나 만료됨: 사진 전송은 계속하며 `detected_at_ms=0`
- 서버 clock은 유효하지만 OPEN time만 0임: 최초 승인한 OPEN 수신시각을
  `SERVER_TIME_RX_ESTIMATE`로 고정
- OPEN time도 0이고 서버 clock도 유효하지 않음: 시간 0,
  `SERVER_TIME_UNKNOWN`

현재 serial/Python 출력의 `SERVER_TIME_P4_DETECTED`라는 enum 이름은 P4가
frame 획득 시 OPEN에 붙인 시각이라는 뜻입니다. 그 수치의 원본은 노트북
표본을 받은 ESP32-CAM 서버이며 P4 자체 SNTP가 아닙니다.

## SDIO protocol v3

P4와 C6는 동일한 `sdio_frame_protocol.h` v3 ABI를 사용합니다.

### 44바이트 JPEG chunk header

| offset | 형식 | field |
| ---: | --- | --- |
| 0 | `uint32_t` | magic |
| 4 | `uint16_t` | version = 3 |
| 6 | `uint16_t` | header size = 44 |
| 8 | `uint32_t` | P4 frame ID |
| 12 | `uint16_t` | chunk index |
| 14 | `uint16_t` | chunk count |
| 16 | `uint32_t` | first/last flags |
| 20 | `uint32_t` | chunk offset |
| 24 | `uint32_t` | chunk size |
| 28 | `uint32_t` | JPEG size |
| 32 | `uint64_t` | detected_at_ms |
| 40 | `uint32_t` | complete JPEG CRC32 |

JPEG는 최대 30,720바이트이며 SDIO data chunk는 최대 7,600바이트입니다. class, score, inference timing, class name은 wire에서 제거했습니다.

### 40바이트 server-time message

이미지 v3 ABI와 별도의 custom message ID `SDIO_TIME_MSG_ID`를 양방향으로
사용합니다.

| offset | 형식 | field |
| ---: | --- | --- |
| 0 | `uint32_t` | time magic |
| 4 | `uint16_t` | version = 3 |
| 6 | `uint16_t` | size = 40 |
| 8 | `uint16_t` | kind = QUERY/SAMPLE |
| 10 | `uint16_t` | status |
| 12 | `uint32_t` | request ID |
| 16 | `uint64_t` | P4 client TX monotonic us |
| 24 | `uint64_t` | server RX Unix ms |
| 32 | `uint64_t` | server TX Unix ms |

QUERY의 서버 timestamp는 0이며 SAMPLE은 request ID와 P4 monotonic 값을
그대로 echo합니다. `NOT_READY`, `BUSY`, `UNAVAILABLE`, `FAILED` SAMPLE은
두 server timestamp를 0으로 보냅니다. 이 교환은 이미지와 같은 단일 C6
worker를 사용하므로 동시에 실행되지 않으며 이미지 44바이트 header나 24바이트
상태 control의 ABI를 변경하지 않습니다.

### 24바이트 image control

```text
QUERY → NOT_READY/READY
READY → P4 frame 전송 → ACCEPTED
ACCEPTED → BLE Mesh session
Server COMPLETE → SERVER_ACKED → READY
오류 → FAILED → READY/재연결
```

`ACCEPTED`는 C6가 SDIO JPEG를 조립·검증해 BLE worker에 넘겼다는 뜻입니다. `SERVER_ACKED`만 서버의 최종 JPEG 완료를 뜻합니다.

C6는 SDIO header/metadata 일치, chunk 순서와 offset, 크기, CRC32, JPEG SOI/EOI를 검사합니다. 조립 중 5초 동안 유효 chunk가 없으면 해당 frame을 `FAILED/TIMEOUT`으로 폐기합니다.

## BLE Mesh image protocol v2

- Company ID: `0x02E5`
- Vendor Model ID: `0x0002`
- Server unicast: `0x0001`
- C6 node 주소: `C6_DEVICE_ID` `N`에 따라 `N + 1`로 결정
- JPEG: 정확히 224×224, 최대 30,720바이트
- DATA JPEG payload: 최대 374바이트
- 최대 chunk 수: 83

Opcode가 protocol generation과 packet 종류를 구분하므로 packet별 magic/version은 없습니다. 모든 정수는 little-endian입니다.

| 메시지 | wire 내용 | 역할 |
| --- | --- | --- |
| `TIME_REQUEST` (`0xCA`) | request ID 4B | C6가 server clock 표본 요청 |
| `TIME_STATUS` (`0xCB`) | request ID 4B, status 1B, reserved 3B, server RX/TX Unix ms 각 8B | server clock 표본 또는 명시적 UNAVAILABLE |
| `OPEN` | frame ID 2B, detected time 8B, image length 2B, CRC32 4B | 서버 slot 요청 |
| `DATA` | frame ID 2B, chunk index 1B, JPEG 1~374B | 실제 이미지 |
| `END` | frame ID 2B | 1차 전송/복구 round 종료 |
| `ACCEPT` / `BUSY` | frame ID 2B | slot 승인/backpressure |
| `NACK` | frame ID 2B, base index 1B, bitmap 1~5B | 누락 chunk 선택 요청 |
| `COMPLETE` | frame ID 2B | CRC/JPEG 검증까지 최종 성공 |
| `RESTART` | frame ID 2B | 서버가 session을 잃어 OPEN부터 재시작 요청 |
| `REJECT` | frame ID 2B, reason 1B | 형식/크기/상태/CRC/JPEG/timeout 오류 |

30,720바이트 최대 frame의 83개 bit는 5바이트 bitmap 구간 최대 3개로 표현합니다. 서버는 END를 받은 뒤 빠진 bit만 세우며 C6는 그 chunk만 다시 보냅니다. DATA 마지막 packet도 실제 JPEG 길이만 전송하므로 고정 padding이 없습니다.

```text
C6     OPEN ────────────────────────────────> Server
        <──────────────────────────── ACCEPT/BUSY
        DATA 0 ... DATA N, END ─────────────>
        <──────── COMPLETE 또는 bitmap NACK
        NACK 해당 DATA만, END ──────────────>
        <──────────────────────────── COMPLETE
```

chunk별 application ACK은 없습니다. BLE Mesh segmented unicast의 transport 복구에 더해 frame 종료 시 누락된 application chunk만 선택적으로 보완합니다.

서버는 `(Mesh source address, frame ID)`를 session 조회의 기본 key로 사용합니다. 완료 cache의 중복 `OPEN`을 `COMPLETE`로 인정할 때는 `detected_at_ms`, JPEG 길이와 CRC32까지 모두 일치해야 합니다. payload에 source/destination/hop/width/height를 중복 저장하지 않습니다.

## 자동 provisioning

서버가 provisioner이므로 휴대폰 앱은 필요하지 않습니다.

1. 각 물리 C6의 `main/device_identity.h`에서 1~32766 사이의
   고유한 `C6_DEVICE_ID`를 선택하고 해당 바이너리를 flash합니다.
2. 서버를 먼저 부팅합니다. local address `0x0001`로 network와
   NetKey/AppKey를 NVS에서 복원하거나 생성합니다.
3. 아직 provision되지 않은 C6를 부팅합니다. C6는 UUID에 Bluetooth
   identity address와 `C6_DEVICE_ID`를 실어 PB-ADV/PB-GATT bearer를 엽니다.
4. 서버가 UUID의 ID `N`을 검증하고 unicast `N + 1`로 provisioning합니다.
   동일 ID를 다른 UUID가 사용하거나 주소가 기존 node와 충돌하면 거부합니다.
5. 서버가 C6 Vendor Model에 AppKey를 bind하고 publication address를
   `0x0001`로 설정합니다.
6. C6가 설정을 NVS에 저장하고 P4에 READY를 알립니다.

서버가 동시에 자동 관리하는 C6는 기본 최대 10개입니다. ID는
연속일 필요가 없지만 장치별로 반드시 고유해야 합니다. 주소는 발견
순서가 아니라 ID에서 결정됩니다.

`C6_DEVICE_ID`와 Device UUID는 위치 식별자이지 비밀키나 인증수단이
아닙니다. 현재 자동 등록은 OOB 인증 없이 동작하므로 신뢰할 수 있는
설치 환경에서 등록하고, 운영 보안이 필요하면 UUID/MAC allowlist 또는
OOB 인증을 별도로 추가해야 합니다.

## 빌드, 플래시, monitor

ESP-IDF 5.5 shell에서 실행하고 포트는 실제 값으로 바꿉니다.

### 1. Generic server

```powershell
cd firmware/server
idf.py set-target esp32
# 또는 esp32c3, esp32c6, esp32c61, esp32h2, esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM_SERVER flash
```

ESP32-CAM은 target `esp32`로 빌드합니다. 이 firmware는 ESP32-CAM의 카메라
주변장치나 카메라 GPIO를 사용하지 않으며, 선택한 다른 칩도 ESP-IDF BLE Mesh를
지원해야 합니다. 안정적인 5 V 전원을 사용합니다.

ESP32-CAM에서 COM 로그/JPEG와 노트북 기준시각을 함께 사용하려면 기본
menuconfig가 다음과 같아야 합니다.

```text
CONFIG_SERVER_SERIAL_IMAGE_ENABLE=y
CONFIG_SERVER_SERIAL_TIME_ENABLE=y
CONFIG_SERVER_WIFI_SNTP_ENABLE=n
```

SSID나 Internet 연결은 필요 없습니다. `receive_images.py`가 보내는 28바이트
`BMTIME01` 패킷은 CRC-32로 보호되며 마지막 유효 패킷 후 5분까지만 서버
clock으로 사용됩니다. 노트북 OS의 시간이 맞아야 합니다. UART는 full-duplex라
PC→서버 시각과 서버→PC 로그/JPEG가 같은 COM에서 동시에 동작합니다. 선택형
Wi-Fi/SNTP를 다시 쓰려면 serial time을 끄고 Wi-Fi/SNTP를 켭니다.

기본 설정은 `CONFIG_SERVER_SERIAL_IMAGE_ENABLE=y`이며 console UART를
921600 baud로 사용합니다. 일반 로그와 완성 JPEG frame이 같은 USB console
stream에 섞여 나오므로 `idf.py monitor` 대신 전용 도구 하나가 COM port를
단독으로 열어야 합니다.

```powershell
cd ../..
python -m pip install pyserial
python firmware/server/tools/receive_images.py --port COM_SERVER --baud 921600 --output received_images
```

위치 이름을 함께 남기려면 예제 JSON을 복사해 ID별 위치를 편집합니다.

```powershell
Copy-Item firmware/server/tools/locations.example.json locations.json
# locations.json의 "1", "2" ... 항목을 실제 설치 위치로 수정
python firmware/server/tools/receive_images.py --port COM_SERVER --baud 921600 --output received_images --locations locations.json
```

위치 매핑은 PC에서 `device_id`에 적용하며 BLE/USB binary record의 wire
형식을 늘리지 않습니다. 장치를 다른 위치로 옮겼다면 ID를 바꾸지
말고 JSON의 위치만 갱신합니다.

별도 image UART는 필요하지 않습니다. 기본 target 설정은 secondary console 없이
primary UART0를 사용하므로 개발보드에서는 Flash/로그에 쓰던 내장 USB-UART bridge와
같은 USB/COM 연결을 그대로 사용합니다. Bare module이면 평소 Flash에 사용하는
USB-UART bridge 하나를 primary UART0에 연결합니다.

C3/C6/H2/S3처럼 native USB Serial/JTAG를 지원하는 chip에서는 이를 ESP-IDF의
**primary console**로 선택해도 됩니다. 이때는 해당 COM을 사용하며 UART baud 값은
물리적으로 적용되지 않습니다. 단, nonblocking secondary console로 설정한 native
USB에는 30 KiB image를 보내면 안 됩니다. 출력이 유실될 수 있습니다.

어떤 primary console을 선택해도 같은 COM port를 `idf.py monitor`와 Python 도구가
동시에 열 수 없으며, 일반 monitor는 binary JPEG 구간을 처리하지 못합니다.
Python 도구가 ESP-IDF text log를 실시간으로 화면에 출력하므로 이 도구 자체가
monitor 역할도 합니다. 시작 후 reset/EN을 누르면 부팅 로그부터 확인할 수 있습니다.

USB 데이터 케이블이 하나뿐이어도 서버, C6, P4를 차례로 flash할 수 있습니다.
종단 시험에는 P4/C6와 서버가 동시에 켜져 있어야 하지만 세 장치를 모두 PC의
USB 데이터 포트에 연결할 필요는 없습니다. Python으로 사진과 로그를 받을
때는 케이블을 서버 COM에 두고 P4와 C6는 외부 전원으로 구동합니다. 이때
P4/C6 개별 monitor는 볼 수 없어도 서버의 TIME/OPEN/COMPLETE 로그와 최종
JPEG로 종단 성공을 판정할 수 있습니다.

서버가 내보내는 record는 다음 40바이트 little-endian header 뒤에 JPEG가
padding 없이 이어지는 형식입니다.

```text
<8sHHHBBQIIII
BMJPEG01, version=1, header_size=40, source_addr, time_source,
reserved=0, event_time_ms, jpeg_len, jpeg_crc32, sequence, header_crc32
```

header CRC32는 앞 36바이트, JPEG CRC32는 정확히 `jpeg_len`바이트를
검사합니다. 도구는 magic 앞뒤의 text log를 보존하고, 손상된 header/JPEG를
버린 뒤 다음 magic에서 자동 재동기화합니다. 정상 frame만 timestamped JPEG와
JSON으로 저장하며 `latest.jpg`와 `latest.json`을 원자적으로 갱신합니다.

### 2. C6 source

장치를 처음 등록하기 전 `main/device_identity.h`의
`C6_DEVICE_ID`를 해당 물리 C6의 고유 ID로 수정합니다.

```powershell
cd firmware/c6_sdio_ble
idf.py set-target esp32c6
idf.py build
idf.py -p COM_C6 flash monitor
```

### 3. P4 inference

```powershell
cd firmware/p4_inference
idf.py set-target esp32p4
idf.py build
idf.py -p COM_P4 flash monitor
```

서버와 C6를 먼저 준비한 뒤 P4를 부팅하면 초기 READY 확인이 쉽습니다. 순서가 달라도 P4 Hosted link는 1, 2, 4, 8, 최대 10초 backoff로 C6 연결을 복구합니다.

## USB 이미지와 종단 테스트

서버 COM을 Python 도구로 연 뒤 ESP32-CAM에서 다음 로그를 먼저 확인합니다.

```text
laptop clock synchronized: epoch_ms=... sequence=...
TIME_STATUS src=0x0002 request=... status=0 rx_ms=... tx_ms=...
```

P4 COM을 별도로 볼 수 있을 때는 `server clock synchronized request=...`가
시간 bridge와 monotonic anchor의 최종 확인 로그입니다. P4 Ethernet IP는 아래
HTTP 조회에만 필요합니다.

조회 전용 HTTP 테스트:

```powershell
curl.exe -o classify.jpg -D classify.headers http://P4_IP/classify.jpg
```

- `classify.jpg`가 열리고 분류 결과 header가 반환되는지 확인합니다.
- 이 요청 직후 P4/C6에 새 SDIO/BLE frame 로그가 없어야 합니다.

기본 server에서는 위 `receive_images.py`가 계속 실행 중이어야 합니다. 정상
수신 시 console에 다음과 같은 한 줄을 추가로 출력하고 파일을 만듭니다.

```text
IMAGE id=1 location=front_entrance src=0x0002 seq=1 event_ms=... time=P4_DETECTED bytes=... file=image_....jpg
received_images/latest.jpg
received_images/latest.json
```

여기서 `P4_DETECTED`는 P4가 fresh frame에 붙인 capture timestamp라는
provenance 이름이며, 절대시각 값 자체는 노트북 UART 표본에서 파생됩니다.
P4가 0을 보냈고 server fallback이 적용되면 `RX_ESTIMATE`, 양쪽 시계가 모두
유효하지 않으면 `UNKNOWN`이 표시됩니다.

기본 serial time/image 구성에서는 Wi-Fi를 켜지 않습니다. 펌웨어 HTTP 이미지
출력은 COM 이미지 출력의 대체 adapter이므로 꼭 필요할 때만 serial time과
serial image를 끄고 `CONFIG_SERVER_SERIAL_IMAGE_ENABLE=n`,
`CONFIG_SERVER_HTTP_ENABLE=y`로 빌드합니다. serial/HTTP 출력 두 adapter는
동시에 켤 수 없습니다.

자동 이벤트 테스트:

1. P4 카메라에 사람을 보여 주고 최대 10초 기다립니다.
2. P4 monitor를 사용할 수 있으면 human crop, SDIO frame ID, `ACCEPTED`를 확인합니다.
3. C6 monitor를 사용할 수 있으면 OPEN/ACCEPT, DATA, END를 확인합니다.
4. Python console에서 device ID/location, source address, event time, JPEG length와 `IMAGE` 로그를 확인합니다.
5. 개별 monitor를 사용할 수 있으면 C6와 P4에서
   `COMPLETE → SERVER_ACKED → READY`를 확인합니다.
6. `received_images/latest.jpg`를 열고 `latest.json` metadata와 비교합니다.

누락 복구는 host test로 반복할 수 있습니다.

```powershell
python firmware/c6_sdio_ble/tests/run_host_tests.py
python firmware/server/tests/run_host_tests.py
```

실제 무선에서 NACK을 만들려면 packet loss가 필요하므로 정상 근거리 smoke test에서 NACK 로그가 없을 수 있습니다. COMPLETE가 최종 성공 기준입니다.

## Reset과 NVS 운영 규칙

Mesh key, node address, DevKey, AppKey binding과 publication 설정은 NVS에 저장됩니다.

- 일반 `flash` 또는 firmware update에서 NVS가 보존되고
  `C6_DEVICE_ID`도 같다면 재provision하지 않습니다.
- 서버의 Mesh/NVS를 factory erase하면 서버가 예전 C6의 DevKey/network 상태를 잃습니다.
- 이 경우 기존 C6도 BLE Mesh node reset 또는 NVS erase하고 모든 node를 다시 provisioning해야 합니다.
- 서버만 초기화하거나 C6 일부만 예전 network에 남겨 두면 자동 복구되지 않습니다.

UART factory-reset 명령이 없는 빌드에서는 `erase-flash` 후 firmware를 다시 flash합니다. 이 명령은 해당 장치의 전체 flash와 NVS를 지우므로 대상 포트를 반드시 확인합니다.

```powershell
cd firmware/server
idf.py -p COM_SERVER erase-flash
idf.py -p COM_SERVER flash

cd ../c6_sdio_ble
idf.py -p COM_C6 erase-flash
idf.py -p COM_C6 flash
```

이미 등록된 C6의 ID를 바꾸어도 NVS의 기존 Mesh 주소는 자동으로
바뀌지 않습니다. 현재 서버 펌웨어는 node별 삭제 CLI를 제공하지
않으므로, ID 변경이 필요하면 서버 NVS와 등록된 모든 C6의
Mesh NVS를 초기화하고 재provisioning합니다. 이후 각 C6는
`C6_DEVICE_ID + 1`의 같은 결정적 주소를 다시 받습니다.

## 장애 확인

- P4 timestamp가 계속 `0`: Python 수신기의 `laptop time sync enabled`,
  ESP32-CAM의 `laptop clock synchronized`, C6의 CA/CB TIME route, P4의
  `server clock synchronized` 및 SDIO link 확인; 5분 server clock/P4 anchor
  만료 여부도 확인
- P4가 `NOT_READY`: C6 provisioning, AppKey bind, publication `0x0001` 확인
- 서버가 `BUSY`: 다른 source의 active frame 종료 대기; C6가 jitter backoff 후 재시도
- `NACK`: 서버 bitmap에 표시된 chunk만 C6가 재전송하는지 확인
- `RESTART`: 서버가 active session을 잃었으며 C6가 OPEN부터 다시 시작
- `REJECT_CRC/JPEG`: SDIO CRC, JPEG 224×224와 SOI/EOI 확인
- SDIO DOWN: CLK/CMD/DAT 배선, pull-up, 공통 GND, C6 EN 확인
- BLE local send completion이 timeout: C6가 FAILED를 알리고 2초 뒤 NVS를 보존한 채 자동 재시작
- 서버 complete 뒤 P4가 READY로 안 돌아옴: C6의 COMPLETE callback과 SDIO `SERVER_ACKED` control 확인
- Python에서 COM open 실패: `idf.py monitor` 등 같은 COM을 쓰는 프로그램을 모두 닫고 port 확인
- log 글자가 깨지거나 CRC가 계속 실패: firmware와 도구 baud를 모두 921600으로 맞추고 console LF 설정 확인
- `SERIAL SEQUENCE_GAP`: USB stream에서 하나 이상의 완성 record가 유실·손상됐거나 server가 재부팅되어 sequence가 다시 시작됨
