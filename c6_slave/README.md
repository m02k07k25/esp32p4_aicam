# ESP32-C6 SDIO receiver

이 폴더는 ESP32-P4-Function-EV-Board의 온보드 ESP32-C6용 독립 ESP-IDF 프로젝트입니다. Espressif `esp_hosted` slave 예제 2.12.3을 기반으로 하며 다음 기능을 추가합니다.

- P4 custom RPC JPEG 청크 수신 및 검증
- 최신 완성 프레임 재조립
- C6 Wi-Fi의 `8081` 포트에서 HTTP 제공
- `/`, `/received.jpg`, `/status` 엔드포인트

빌드 전 `idf.py menuconfig`에서 기본 Wi-Fi SSID와 비밀번호를 실제 값으로 바꿔야 합니다.

```powershell
$env:PYTHONUTF8='1'
idf.py set-target esp32c6
idf.py menuconfig
idf.py build
idf.py -p COM_C6 flash monitor
```

P4와 C6를 함께 플래시하고 검증하는 전체 절차는 [P4 → C6 SDIO 프레임 전송 및 HTTP 확인](../docs/sdio_frame_transfer.md)을 참고하세요.

이 프로젝트의 ESP-Hosted 원본 파일은 각 파일에 표시된 Espressif 라이선스를 따릅니다.
