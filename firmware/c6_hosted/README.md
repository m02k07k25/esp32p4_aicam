# C6 Hosted receiver

Espressif ESP-Hosted-MCU 2.12.3 기반의 ESP32-C6 펌웨어입니다. 기본 Hosted Wi-Fi 보조 칩 기능에 P4 분류 JPEG 청크 재조립과 HTTP 확인 서버를 추가했습니다. 여기서 SDIO는 P4-C6 통신 버스이며 SD 카드를 뜻하지 않습니다.

첫 설정에서 `idf.py menuconfig`를 열어 `Example Configuration > Wi-Fi default config (pre-provisioning)`의 SSID와 비밀번호를 실제 값으로 바꾸세요.

```powershell
$env:PYTHONUTF8='1'
idf.py set-target esp32c6
idf.py menuconfig
idf.py build
idf.py -p COM_C6 flash monitor
```

HTTP 서버는 C6의 Wi-Fi IP에서 8081 포트를 사용합니다.

- `/`: 자동 갱신 페이지
- `/received.jpg`: 마지막 완성 JPEG
- `/status`: 프레임 ID, 크기, 분류값, 점수와 처리 시간

P4에서 아직 `/classify.jpg`를 호출하지 않았다면 `/received.jpg`가 `503 Service Unavailable`을 반환하는 것이 정상입니다.
