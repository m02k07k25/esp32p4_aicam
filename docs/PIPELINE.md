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
  → fresh frame Unix epoch-ms 기록(SNTP 미동기화 시 0)
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

P4는 Ethernet 연결 뒤 SNTP를 시작하고, fresh frame을 얻은 직후 `detected_at_ms`를 기록합니다. 추론 완료시각이나 서버 도착시각이 아니라 사진에 해당하는 카메라 프레임 획득시각입니다.

```text
frame capture time ────────┐
                           ↓ 그대로 전달
inference → JPEG → SDIO → C6 → BLE Mesh → server
```

Mesh 전송이 오래 걸려도 0이 아닌 P4 timestamp는 변하지 않습니다. 서버는 BLE Mesh network header의 source address와 이 timestamp, JPEG를 완성 이벤트로 제공합니다. P4 시각이 `0`이면 선택형 server clock provider가 최초 `OPEN` 수신 시각을 추정값(`SERVER_TIME_RX_ESTIMATE`)으로 고정하며, provider도 유효하지 않으면 시간 `0`/`SERVER_TIME_UNKNOWN`으로 남습니다.

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

## BLE Mesh v2 선택 재전송

```text
C6 → OPEN(frame_id, detected_at_ms, image_len, CRC32)
Server → ACCEPT 또는 BUSY
C6 → DATA(frame_id, chunk_index, JPEG 1..374B)
C6 → END(frame_id)
Server → COMPLETE
       또는 NACK(base_index + missing bitmap)
C6 → bitmap에 표시된 DATA만 재전송 → END
```

BLE business data는 검출시각과 JPEG뿐입니다. source/destination ID, hop, width/height는 payload에 중복 저장하지 않습니다.

- 실제 송신 C6는 Mesh network header의 source address로 식별합니다.
- 목적지는 C6 Vendor Model publication의 server unicast `0x0001`입니다.
- 이미지는 항상 224×224이므로 해상도 field가 없습니다.
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
