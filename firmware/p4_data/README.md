# P4 data capture

OV5647의 `800x800 RGB565` 프레임을 HTTP로 확인하고 SD 카드에 JPEG로 저장하는 ESP32-P4 펌웨어입니다. 카메라는 스트리밍 상태로 유지하지만, 요청이 들어오면 기존 완료 버퍼를 순환시킨 뒤 새 프레임을 기다리므로 버튼을 누르기 전의 오래된 화면을 반환하지 않습니다.

## 접속

C6 Hosted 펌웨어를 먼저 플래시한 뒤 P4를 부팅합니다.

```text
SSID: esp32p4-data
Password: 12345678
URL: http://192.168.4.1/
```

| 경로 | 기능 | SD 카드 |
| --- | --- | --- |
| `/` | 촬영/갤러리 페이지 | 선택 |
| `/pic` | 요청 이후 촬영된 JPEG | 불필요 |
| `/record` | 요청 이후 촬영된 원본 프레임 | 불필요 |
| `/capture` | 새 JPEG를 SD 카드에 저장 | 필요 |
| `/captures` | 저장된 사진 목록 | 필요 |
| `/photo?name=IMG00001.JPG` | 저장 사진 조회 | 필요 |

GPIO1을 GND로 당겨도 촬영할 수 있습니다. SD 카드 마운트에 실패해도 서버와 카메라 미리보기는 계속 시작됩니다.

```powershell
idf.py set-target esp32p4
idf.py build
idf.py -p COM_P4 flash monitor
```
