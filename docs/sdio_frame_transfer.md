# P4-C6 SDIO 전송과 검증

SDIO는 P4와 C6 사이의 ESP-Hosted 통신 버스입니다. 이 기능에는 SD 카드가 필요하지 않습니다.

## 구성

| 칩 | 프로젝트 | 역할 | HTTP |
| --- | --- | --- | --- |
| ESP32-P4 | `firmware/p4_inference` | 촬영, 추론, JPEG 생성 및 custom data 송신 | Ethernet `:80` |
| ESP32-C6 | `firmware/c6_hosted` | custom data 수신, JPEG 재조립 및 게시 | Wi-Fi `:8081` |

두 프로젝트는 각각 동일한 `sdio_frame_protocol` 컴포넌트 사본을 포함하므로 독립적으로 빌드됩니다. 프로토콜을 바꿀 때는 P4와 C6의 헤더를 함께 수정해야 합니다.

## 동작 순서

1. C6를 먼저 플래시하고 실제 Wi-Fi SSID/비밀번호로 부팅합니다.
2. P4 추론 펌웨어를 플래시하고 Ethernet IP를 확인합니다.
3. `http://<P4_ETH_IP>/classify.jpg`를 요청합니다.
4. P4는 HTTP 결과를 반환하는 동시에 JPEG를 최대 7,600바이트 데이터 청크로 나눠 전송 큐에 복사합니다.
5. C6는 magic, 버전, frame ID, 청크 순서·크기와 전체 JPEG 크기를 검증한 뒤 완성 프레임만 게시합니다.
6. `http://<C6_WIFI_IP>:8081/` 또는 `/received.jpg`에서 같은 결과를 확인합니다.

P4 전송 큐의 길이는 1이며 HTTP 처리와 분리된 worker가 송신합니다. 첫 실제 SDIO 전송 실패 후에는 재부팅 전까지 C6 전달을 중지하여 매 요청마다 5초 RPC timeout이 반복되는 일을 막습니다.

## 주요 로그

정상일 때는 다음 형태의 로그가 나타납니다.

```text
P4: sdio_frame_tx: sent frame=... jpeg=... chunks=... class=... score=...
C6: c6_frame_http: Published frame=... jpeg=... chunks=... class=... score=...
```

`Req_CustomRpc timeout`은 보통 C6에 구형/다른 Hosted 펌웨어가 올라가 있거나 C6 수신 handler가 아직 준비되지 않았다는 뜻입니다. `SDIO unavailable ... disabling frame forwarding until reboot` 이후에도 P4 Ethernet HTTP는 정상 동작해야 합니다.

C6 페이지 접속이 안 되면 C6 로그에서 Wi-Fi IP와 `C6 HTTP server started on port 8081`을 모두 확인하세요. `192.168.4.1`은 P4 데이터 수집 SoftAP의 주소이며, C6 Hosted station IP와 같다고 가정하면 안 됩니다.
