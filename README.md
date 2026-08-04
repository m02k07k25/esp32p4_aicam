# ESP32-P4 AI Camera firmware workspace

ESP32-P4와 ESP32-C6용 펌웨어를 한 `develop` 브랜치에서 관리하되, 빌드와 플래시는 서로 완전히 분리한 저장소입니다. 현재 범위에는 세 프로젝트가 포함됩니다. C6 BLE Mesh 프로젝트는 원본 소스가 없어 이번 통합에서 제외했습니다.

| 프로젝트 | 대상 | 역할 | 기본 네트워크/HTTP |
| --- | --- | --- | --- |
| [`firmware/p4_data`](firmware/p4_data/) | ESP32-P4 | 카메라 데이터 수집 및 SD 저장 | C6 Wi-Fi Remote SoftAP, `http://192.168.4.1/` |
| [`firmware/p4_inference`](firmware/p4_inference/) | ESP32-P4 | ESP-DL 추론 및 선택적 C6 전송 | P4 Ethernet, `/pic`, `/record`, `/classify.jpg` |
| [`firmware/c6_hosted`](firmware/c6_hosted/) | ESP32-C6 | ESP-Hosted 보조 칩 및 P4 결과 수신 | C6 Wi-Fi, `http://<C6_IP>:8081/` |

`model/`은 P4 추론 프로젝트가 사용하는 공용 학습·변환 작업공간입니다. 검증된 모델 파일과 호환성 매니페스트는 다음 두 파일입니다.

```text
model/artifacts/espdl/classifier_224_p4.espdl
model/artifacts/espdl/classifier_224_p4.espdl.json
```

## 프로젝트별 빌드와 플래시

ESP-IDF 5.5 환경에서 각 펌웨어 폴더로 이동해 명령을 실행합니다. `COM_P4`와 `COM_C6`는 실제 포트로 바꾸세요.

### P4 데이터 수집

```powershell
cd firmware/p4_data
idf.py set-target esp32p4
idf.py build
idf.py -p COM_P4 flash monitor
```

생성되는 주 펌웨어는 `firmware/p4_data/build/p4_data_capture.bin`입니다. C6에 Hosted 펌웨어가 올라가 있어야 P4가 C6 무선을 Wi-Fi Remote로 사용할 수 있습니다. 기본 AP는 `esp32p4-data`, 비밀번호는 `12345678`이며 접속 후 `http://192.168.4.1/`을 엽니다.

SD 카드가 없어도 `/pic`과 `/record`는 동작합니다. `/capture`, `/captures`, `/photo` 및 GPIO1 저장 기능만 SD 카드가 필요합니다.

### P4 추론

```powershell
cd firmware/p4_inference
idf.py set-target esp32p4
idf.py build
idf.py -p COM_P4 flash monitor
```

생성되는 주 펌웨어는 `firmware/p4_inference/build/p4_inference.bin`입니다. 로그에 출력된 P4 Ethernet IP로 `/pic` 또는 `/classify.jpg`를 요청합니다. `/classify.jpg` 결과는 P4 HTTP로 즉시 반환되며, C6가 정상 연결된 경우 같은 JPEG와 추론 메타데이터를 SDIO로 C6에도 전달합니다. C6 전송 실패는 P4 HTTP 서버를 중단시키지 않습니다.

### C6 Hosted 수신기

```powershell
cd firmware/c6_hosted
idf.py set-target esp32c6
idf.py menuconfig
idf.py build
idf.py -p COM_C6 flash monitor
```

생성되는 주 펌웨어는 `firmware/c6_hosted/build/c6_hosted.bin`입니다. 첫 빌드 전 `menuconfig`에서 실제 Wi-Fi SSID와 비밀번호를 설정해야 합니다. `sdkconfig.defaults.esp32c6`의 `YOUR_WIFI_SSID`와 `YOUR_WIFI_PASSWORD`는 동작하는 자격 증명이 아닙니다.

C6 로그의 IP를 확인한 뒤 다음 주소를 사용합니다.

- `http://<C6_IP>:8081/`: 자동 갱신 확인 페이지
- `http://<C6_IP>:8081/received.jpg`: 마지막으로 수신한 JPEG
- `http://<C6_IP>:8081/status`: 마지막 프레임의 JSON 메타데이터

P4와 C6는 서로 다른 칩이므로 각각 해당 프로젝트 폴더에서 별도로 플래시합니다. 한 프로젝트의 `build/`나 `sdkconfig`가 다른 프로젝트에 영향을 주지 않습니다.

## menuconfig와 생성 파일

`menuconfig` 결과는 실행한 프로젝트 폴더의 `sdkconfig`에만 저장됩니다. 같은 프로젝트를 다시 빌드할 때는 유지되므로 매번 설정할 필요가 없습니다. 다음 경우에만 다시 확인하면 됩니다.

- `sdkconfig`를 삭제한 경우
- 보드 핀, 카메라, 네트워크 방식 또는 C6 Wi-Fi 자격 증명을 바꾸는 경우
- 다른 프로젝트 폴더로 이동한 경우

`build/`, `sdkconfig*`, `managed_components/`와 모델 평가 CSV 등 생성물은 Git에서 제외됩니다. 공유할 기본값만 각 프로젝트의 `sdkconfig.defaults*`에 기록합니다.

## 문서

- [펌웨어와 HTTP 동작](docs/README.md)
- [카메라·추론 파이프라인](docs/PIPELINE.md)
- [모델 학습과 변환](docs/model.md)
- [P4-C6 SDIO 전송과 검증](docs/sdio_frame_transfer.md)
