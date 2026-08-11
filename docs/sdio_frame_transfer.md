# P4-C6 SDIO 및 BLE Mesh 통합

SDIO는 P4와 C6 사이의 ESP-Hosted 통신 버스이며 SD 카드를 뜻하지 않습니다. 공식 이미지 이벤트 조합은 다음 세 프로젝트입니다.

| 장치 | 프로젝트 | 역할 | 주소/인터페이스 |
| --- | --- | --- | --- |
| ESP32-P4 | `firmware/p4_inference` | human 추론, 시각 기록, 224×224 JPEG, SDIO host | Ethernet HTTP `:80` |
| ESP32-C6 | `firmware/c6_sdio_ble` | SDIO slave, JPEG 검증, Mesh source/relay | 자동 할당 `0x0002` 이상 |
| BLE 지원 ESP | `firmware/server` | provisioner, 이미지 sink, 재조립/NACK | local unicast `0x0001`, USB console |

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

P4는 `example_connect()`로 Ethernet IP를 받은 뒤 SNTP client를 시작하지만 동기화를 기다리며 부팅을 막지는 않습니다.

```text
CONFIG_P4_INFERENCE_SNTP_ENABLE=y
CONFIG_P4_INFERENCE_SNTP_SERVER="pool.ntp.org"
```

폐쇄망에서는 public hostname 대신 접근 가능한 LAN NTP 주소를 설정합니다. P4는 fresh camera frame dequeue 직후 Unix epoch millisecond를 기록합니다.

- SNTP 동기화 성공: 실제 `detected_at_ms` 전송
- 최초 동기화 전/비활성: `detected_at_ms=0`
- 서버는 0이 아닌 P4 timestamp를 수정하지 않고 완성 이벤트에 전달
- P4 timestamp가 0이면 선택형 server clock provider가 최초 `OPEN` 수신 시각을 추정값으로 고정하고, provider도 유효하지 않으면 0/UNKNOWN으로 전달
- Mesh 병목은 도착시간만 늦추며 이미 찍힌 검출시각을 바꾸지 않음

서버 firmware에는 특정 Wi-Fi/SNTP 구현을 필수로 넣지 않습니다. 따라서 WROOM32가 아닌 BLE 지원 ESP 칩에서도 같은 Mesh core를 사용할 수 있습니다.

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

### 24바이트 control

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
- C6 node 주소: provisioning 시 `0x0002`부터 자동 할당
- JPEG: 정확히 224×224, 최대 30,720바이트
- DATA JPEG payload: 최대 374바이트
- 최대 chunk 수: 83

Opcode가 protocol generation과 packet 종류를 구분하므로 packet별 magic/version은 없습니다. 모든 정수는 little-endian입니다.

| 메시지 | wire 내용 | 역할 |
| --- | --- | --- |
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

1. 서버를 먼저 부팅합니다. local address `0x0001`로 network와 NetKey/AppKey를 NVS에서 복원하거나 생성합니다.
2. 아직 provision되지 않은 C6를 부팅합니다. C6는 PB-ADV/PB-GATT bearer를 엽니다.
3. 서버가 PB-ADV로 C6를 찾아 `0x0002`부터 주소를 할당합니다.
4. 서버가 C6 Vendor Model에 AppKey를 bind하고 publication address를 `0x0001`로 설정합니다.
5. C6가 설정을 NVS에 저장하고 P4에 READY를 알립니다.

서버가 자동 관리하는 C6는 최대 10개입니다. 모든 C6에 동일한 `c6_sdio_ble.bin`을 사용하며 주소를 바꾸기 위해 firmware를 다시 빌드하지 않습니다.

## 빌드, 플래시, monitor

ESP-IDF 5.5 shell에서 실행하고 포트는 실제 값으로 바꿉니다.

### 1. Generic server

```powershell
cd firmware/server
idf.py set-target esp32
# 또는 esp32c3, esp32c6, esp32c61, esp32h2, esp32s3
idf.py build
idf.py -p COM_SERVER flash
```

서버는 target을 강제하지 않지만 선택한 칩이 ESP-IDF BLE Mesh를 지원해야 합니다.

기본 설정은 `CONFIG_SERVER_SERIAL_IMAGE_ENABLE=y`이며 console UART를
921600 baud로 사용합니다. 일반 로그와 완성 JPEG frame이 같은 USB console
stream에 섞여 나오므로 `idf.py monitor` 대신 전용 도구 하나가 COM port를
단독으로 열어야 합니다.

```powershell
cd ../..
python -m pip install pyserial
python firmware/server/tools/receive_images.py --port COM_SERVER --baud 921600 --output received_images
```

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

P4 monitor에서 Ethernet IP와 다음 로그를 먼저 확인합니다.

```text
SNTP started ...
SNTP synchronized: epoch_ms=...
```

조회 전용 HTTP 테스트:

```powershell
curl.exe -o classify.jpg -D classify.headers http://P4_IP/classify.jpg
```

- `classify.jpg`가 열리고 분류 결과 header가 반환되는지 확인합니다.
- 이 요청 직후 P4/C6에 새 SDIO/BLE frame 로그가 없어야 합니다.

기본 server에서는 위 `receive_images.py`가 계속 실행 중이어야 합니다. 정상
수신 시 console에 다음과 같은 한 줄을 추가로 출력하고 파일을 만듭니다.

```text
IMAGE src=0x0002 seq=1 event_ms=... time=P4_DETECTED bytes=... file=image_....jpg
received_images/latest.jpg
received_images/latest.json
```

Wi-Fi/HTTP 방식은 대체 adapter입니다. 꼭 필요할 때만
`CONFIG_SERVER_SERIAL_IMAGE_ENABLE=n`, `CONFIG_SERVER_HTTP_ENABLE=y`로
빌드합니다. 두 adapter는 동시에 켤 수 없습니다.

자동 이벤트 테스트:

1. P4 카메라에 사람을 보여 주고 최대 30초 기다립니다.
2. P4에서 human crop, SDIO frame ID, `ACCEPTED`를 확인합니다.
3. C6에서 OPEN/ACCEPT, DATA, END를 확인합니다.
4. Python console에서 source address, event time, JPEG length와 `IMAGE` 로그를 확인합니다.
5. C6와 P4에서 `COMPLETE → SERVER_ACKED → READY`를 확인합니다.
6. `received_images/latest.jpg`를 열고 `latest.json` metadata와 비교합니다.

누락 복구는 host test로 반복할 수 있습니다.

```powershell
python firmware/c6_sdio_ble/tests/run_host_tests.py
python firmware/server/tests/run_host_tests.py
```

실제 무선에서 NACK을 만들려면 packet loss가 필요하므로 정상 근거리 smoke test에서 NACK 로그가 없을 수 있습니다. COMPLETE가 최종 성공 기준입니다.

## Reset과 NVS 운영 규칙

Mesh key, node address, DevKey, AppKey binding과 publication 설정은 NVS에 저장됩니다.

- 일반 `flash` 또는 firmware update에서 NVS가 보존되면 재provision하지 않습니다.
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

여러 C6를 사용하면 기존 node 모두에 같은 reset 규칙을 적용합니다. 재부팅 뒤 서버가 다시 `0x0002`부터 주소를 배정합니다.

## 장애 확인

- P4 timestamp가 계속 `0`: Ethernet DNS/NTP 접근과 SNTP server 설정 확인
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
