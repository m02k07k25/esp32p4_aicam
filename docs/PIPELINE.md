# 카메라·추론·전송 파이프라인

## 데이터 수집 경로

```text
OV5647 800x800
  → V4L2 RGB565 stream
  → 완료된 오래된 buffer 한 사이클 반환
  → 요청 뒤 생성된 fresh frame 대기
  → JPEG encode
  → HTTP 응답 또는 /sdcard/captures/IMGxxxxx.JPG 저장
```

`firmware/p4_data`의 GPIO1은 internal pull-up, active-low입니다. GPIO1을 GND에 연결하면 HTTP `/capture`와 같은 SD 저장 경로를 실행합니다.

## 추론 경로

```text
OV5647 800x800 RGB565 fresh frame
  → server-derived monotonic anchor에서 fresh frame Unix epoch-ms 기록
     (anchor가 없거나 만료되면 0)
  → 좌상/우상/좌하/우하/중앙 400x400 crop
  → 각 crop을 224x224로 nearest-neighbor resize
  → Keras MobileNetV2 방식 x / 127.5 - 1 전처리
  → ESP-DL 분류 5회
  → 최대 human score와 threshold 0.72482645511627197 비교
```

라벨 순서는 `no_human`, `human`입니다. 최대 human score가 threshold 이상일 때만 최종 class를 human으로 결정합니다.

### HTTP 분기

`/classify.jpg`는 전체 화면 JPEG와 분류 결과 header를 반환하고 종료합니다. 이 경로에서는 SDIO submit API를 호출하지 않습니다.

### 자동 이벤트 분기

```text
30초 주기 + human + C6 READY
  → 최대 human score crop 선택
  → 정확히 224x224 RGB565
  → JPEG quality 60, 55, ... 20 순서로 encode
  → 30,720B 이하인 첫 JPEG 선택
  → SDIO v3 전송
```

품질 20에서도 제한을 넘으면 그 주기는 전송하지 않습니다. no_human, C6 NOT_READY/BUSY 상태에서도 JPEG encode와 전송을 건너뜁니다.

## 이벤트 시각

노트북 Python 수신기가 ESP32-CAM Mesh 서버에 기준시각을 주며 P4 Ethernet은
HTTP용입니다. 서버와 P4 사이의 시간 경로는 이미지 경로의 반대 방향으로 먼저
만들어집니다.

```text
Laptop OS clock → 28B server console-UART update
  → TIME_STATUS 0xCB(server RX/TX Unix ms)
  → C6
  → 40B SDIO TIME SAMPLE
  → P4 monotonic-to-Unix anchor

P4 fresh DQBUF → detected_at_ms ───────────────┐
                                                ↓ 그대로 전달
inference → JPEG → SDIO → C6 → BLE Mesh OPEN → server
```

P4가 C6 READY/idle 상태에서 보내는 `TIME QUERY`는 C6가 Mesh
`TIME_REQUEST(0xCA)`로 서버에 전달합니다. 서버는 요청을 받은 시각과 응답을
queue하기 직전의 시각을 `TIME_STATUS(0xCB)`로 돌려줍니다. P4는 자체 송신/수신
monotonic 시각까지 더해 왕복 지연을 제한하고 midpoint anchor를 계산합니다.
anchor는 5분마다 갱신하며 15분 뒤 만료됩니다. SDIO disconnect, C6 restart,
stale/foreign 응답 또는 모순된 표본은 거절하거나 기존 anchor를 무효화합니다.

P4는 fresh frame을 dequeue한 직후 anchor에서 `detected_at_ms`를 읽습니다. 이는
추론 완료시각이나 서버의 이미지 도착시각이 아니라 사진의 획득시각입니다.
Mesh 전송이 오래 걸려도 0이 아닌 timestamp는 변하지 않습니다. 서버는 BLE
Mesh network header의 source address, 그 주소에서 역산한 `device_id`, timestamp와
JPEG를 완성 이벤트로 제공합니다. 현재 출력 enum 이름
`SERVER_TIME_P4_DETECTED`는 “P4가 frame에 붙인 capture time”이라는 뜻이며 그
절대시각의 권위는 노트북 표본을 받은 ESP32-CAM 서버입니다.

서버가 아직 유효한 노트북 시각을 받지 못했거나 마지막 갱신 후 5분이 지나면
TIME_STATUS는 `UNAVAILABLE`과 두 개의 0
timestamp를 반환하고 P4는 frame time 0을 보냅니다. 반대로 서버 clock은
유효하지만 P4 anchor만 없거나 만료되어 OPEN time이 0이면 서버는 최초 승인한
OPEN 수신시각을 `SERVER_TIME_RX_ESTIMATE`로 고정합니다. 그 시점에도 서버
clock이 유효하지 않으면 시간 0/`SERVER_TIME_UNKNOWN`으로 남습니다.

## SDIO v3

P4는 JPEG를 최대 7,600바이트씩 나누고 각 packet에 44바이트 header를 붙입니다.

```text
magic/version/header_size
P4 frame_id
chunk index/count/flags/offset/size
JPEG total size
detected_at_ms
JPEG CRC32
```

분류 class, score, inference time, 이름은 SDIO wire에 넣지 않습니다. C6는 크기, metadata 일치, 순서, offset, CRC32, JPEG SOI/EOI를 검증하고 한 개 BLE worker에 넘깁니다.

시간 교환은 이미지 header와 별도의 고정 40바이트
`sdio_time_message_t`를 사용합니다. QUERY에는 P4 request ID와 송신 monotonic
시각이, SAMPLE에는 이를 그대로 echo한 값과 서버 RX/TX Unix ms가 들어갑니다.
따라서 기존 44바이트 이미지 SDIO v3 ABI는 바뀌지 않습니다.

## BLE Mesh v2 선택 재전송

```text
P4 → SDIO TIME QUERY → C6 → TIME_REQUEST(0xCA) → Server
P4 ← SDIO TIME SAMPLE ← C6 ← TIME_STATUS(0xCB) ← Server

C6 → OPEN(frame_id, detected_at_ms, image_len, CRC32)
Server → ACCEPT 또는 BUSY
C6 → DATA(frame_id, chunk_index, JPEG 1..374B)
C6 → END(frame_id)
Server → COMPLETE
       또는 NACK(base_index + missing bitmap)
C6 → bitmap에 표시된 DATA만 재전송 → END
```

BLE business data는 검출시각과 JPEG뿐입니다. source/destination ID, hop, width/height는 payload에 중복 저장하지 않습니다.

- 실제 송신 C6는 Mesh network header의 source address로 식별하며
  관리 ID는 `device_id = source_addr - 1`입니다.
- 목적지는 C6 Vendor Model publication의 server unicast `0x0001`입니다.
- 이미지는 항상 224×224이므로 해상도 field가 없습니다.
- 위치 이름은 PC `--locations` JSON에서 ID와 연결하며 BLE payload에는
  넣지 않습니다.
- DATA header는 3바이트이고 JPEG는 packet당 최대 374바이트입니다.
- 30,720바이트는 최대 83개 chunk입니다.
- NACK은 최대 5바이트 missing bitmap을 사용하며 필요한 구간만 최대 3개 메시지로 보냅니다.
- 모든 chunk와 CRC/JPEG 검증이 성공해야 서버가 `COMPLETE`를 보냅니다.

이것은 chunk마다 application ACK을 받는 TCP 형태가 아닙니다. BLE Mesh segmented unicast가 radio segment를 복구하고, application 계층은 한 프레임을 다 보낸 뒤 실제로 빠진 이미지 chunk만 선택적으로 복구합니다.

## Backpressure와 buffer 수명

- P4와 C6는 각각 한 프레임만 in-flight로 유지합니다.
- 서버는 한 프레임만 active로 재조립하며 다른 C6 OPEN에는 BUSY를 보냅니다.
- C6는 full-jitter backoff 뒤 OPEN을 다시 시도합니다.
- C6는 서버 COMPLETE 전까지 실제 SDIO JPEG buffer를 보존합니다.
- 서버 COMPLETE가 C6 `SERVER_ACKED`, 이어 P4 `SERVER_ACKED`로 전달된 뒤 buffer를 재사용합니다.
- 서버가 재시작해 세션을 잃으면 RESTART를 보내고 C6가 OPEN부터 다시 시작합니다.
- 중복 DATA는 내용이 같으면 무시하고, 내용이 다르면 해당 세션을 거부합니다.

카메라와 JPEG encoder는 mutex로 직렬화됩니다. `/pic`, `/record`, `/classify.jpg`, 자동 추론 task가 동시에 실행돼도 같은 camera/JPEG buffer를 서로 덮어쓰지 않습니다.
