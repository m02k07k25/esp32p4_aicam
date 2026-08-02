# P4 → C6 SDIO 프레임 전송 및 HTTP 확인

이 브랜치는 한 저장소 안에 서로 독립적인 ESP-IDF 프로젝트 두 개를 둡니다.

| 칩 | 프로젝트 위치 | 역할 | HTTP |
| --- | --- | --- | --- |
| ESP32-P4 | 저장소 루트 | 카메라 캡처, 분류, JPEG 송신 | 기존 Ethernet 서버 (`/pic`, `/classify.jpg`) |
| ESP32-C6 | `c6_slave/` | SDIO 수신, JPEG 재조립 | Wi-Fi 서버 (`:8081/`, `/received.jpg`, `/status`) |

두 칩의 펌웨어는 합쳐서 한 번에 플래시하지 않습니다. 각 폴더에서 각 칩의 포트로 따로 빌드하고 플래시합니다. SDIO는 P4와 C6 사이의 통신 버스이며 SD 카드가 필요하다는 뜻이 아닙니다.

## 전송 흐름

1. 브라우저가 P4의 `/classify.jpg`를 요청합니다.
2. P4가 프레임을 분류하고 JPEG를 여러 SDIO custom RPC 청크로 나눕니다.
3. P4 HTTP 응답은 SDIO 완료를 기다리지 않습니다. C6가 없거나 응답하지 않아도 HTTP 요청은 계속 끝납니다.
4. C6가 완성된 JPEG와 분류 메타데이터를 저장합니다.
5. 브라우저가 C6의 `http://<C6_WIFI_IP>:8081/`에서 최신 수신 결과를 확인합니다.

P4를 부팅한 뒤 첫 SDIO 전송이 실패하면 해당 부팅 동안 전달을 비활성화합니다. C6 펌웨어를 먼저 실행한 다음 P4를 재부팅하는 순서가 가장 확실합니다.

## 최초 설정

ESP-IDF PowerShell에서 C6 프로젝트를 설정합니다.

```powershell
cd c6_slave
idf.py set-target esp32c6
idf.py menuconfig
```

`Example Configuration > Wi-Fi default config (pre-provisioning)`에서 실제 Wi-Fi SSID와 비밀번호를 입력합니다. 저장된 `c6_slave/sdkconfig`는 Git에서 제외되므로 비밀번호가 커밋되지 않습니다. 기본값인 `YOUR_WIFI_SSID`와 `YOUR_WIFI_PASSWORD` 상태로는 Wi-Fi에 연결되지 않습니다.

Windows에서 Kconfig의 UTF-8 문자를 읽지 못하면 같은 터미널에서 다음을 먼저 실행합니다.

```powershell
$env:PYTHONUTF8='1'
```

## C6 빌드와 플래시

ESP32-P4-Function-EV-Board의 온보드 C6는 일반적으로 ESP-Prog를 `PROG_C6` 헤더에 연결해 최초 플래시합니다.

| ESP-Prog | PROG_C6 |
| --- | --- |
| `ESP_EN` | `EN` |
| `ESP_TXD` | `TXD` |
| `ESP_RXD` | `RXD` |
| `GND` | `GND` |
| `ESP_IO0` | `IO0` |
| `VDD` | 연결하지 않음 |

C6 플래시 중 P4가 간섭하지 않도록 P4를 다운로드 모드에 둡니다: P4의 `BOOT`를 누른 상태에서 `RST`를 눌렀다 놓고, 마지막에 `BOOT`를 놓습니다.

```powershell
cd c6_slave
idf.py build
idf.py -p COM_C6 flash monitor
```

`COM_C6`에는 ESP-Prog의 실제 COM 포트를 넣습니다. 정상 연결 후 C6 로그에서 다음 형태의 주소를 찾습니다.

```text
Slave sta dhcp {IP[192.168.x.x] ...}
C6 HTTP server started on port 8081
```

## P4 빌드와 플래시

새 ESP-IDF PowerShell을 저장소 루트에서 열고 P4의 USB-UART 포트로 플래시합니다.

```powershell
idf.py build
idf.py -p COM_P4 flash monitor
```

`COM_P4`와 `COM_C6`는 서로 다른 포트입니다. 명령을 실행하는 현재 폴더가 어떤 펌웨어를 빌드하고 플래시할지 결정합니다.

## 동작 확인

1. C6를 먼저 부팅하고 Wi-Fi IP가 출력되는지 확인합니다.
2. P4를 부팅하고 Ethernet IP가 출력되는지 확인합니다.
3. P4에서 분류를 한 번 실행합니다.

```text
http://<P4_ETH_IP>/classify.jpg
```

4. P4 로그에서 `sdio_frame_tx: sent frame=...`을 확인합니다.
5. C6 로그에서 `c6_frame_http: Published frame=...`을 확인합니다.
6. 아래 주소를 엽니다.

```text
http://<C6_WIFI_IP>:8081/
```

C6 엔드포인트는 다음과 같습니다.

| URL | 설명 |
| --- | --- |
| `/` | 최신 JPEG와 JSON 상태를 자동 갱신하는 확인 페이지 |
| `/received.jpg` | P4에서 마지막으로 받은 JPEG |
| `/status` | 프레임 ID, 크기, 클래스, 점수, 추론 시간 |

아직 P4의 `/classify.jpg`를 호출하지 않았다면 `/received.jpg`가 `503`을 반환하는 것이 정상입니다. `/favicon.ico`의 `404`도 브라우저 아이콘 요청일 뿐 동작 오류가 아닙니다.

## 프로토콜

양쪽 프로젝트가 [sdio_frame_protocol.h](../components/sdio_frame_protocol/include/sdio_frame_protocol.h)를 함께 사용합니다. 메시지 ID, 청크 크기, 헤더 구조가 한 파일에 있으므로 한쪽만 변경해 생기는 프로토콜 불일치를 방지합니다.

JPEG는 최대 7,600바이트씩 분할됩니다. C6는 청크 순서, 오프셋, 전체 크기, 첫/마지막 플래그를 검증하고 완성된 프레임만 게시합니다. 기본 최대 JPEG 크기는 192 KiB입니다.

## 오류 해석

- `Req_CustomRpc timeout`: C6가 구형 ESP-Hosted 펌웨어이거나, 수신 핸들러가 없는 펌웨어이거나, C6가 아직 준비되지 않은 상태입니다.
- `SDIO unavailable ... disabling frame forwarding until reboot`: P4 HTTP는 계속 동작하지만 C6 전달은 다음 P4 재부팅까지 생략합니다.
- C6 페이지 접속 불가: C6 로그의 Wi-Fi IP와 포트 `8081`을 사용했는지, PC가 같은 네트워크인지 확인합니다.
- P4 페이지 접속 불가: C6 Wi-Fi IP가 아니라 P4 로그의 Ethernet IP를 사용합니다.
