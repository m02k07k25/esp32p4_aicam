# 카메라·추론 파이프라인

## 데이터 수집 경로

```text
OV5647 800x800
  -> V4L2 RGB565 스트림
  -> 요청 시 완료된 오래된 버퍼 한 사이클 재큐잉
  -> 요청 이후의 새 프레임 대기
  -> JPEG 인코딩
  -> HTTP 응답 또는 /sdcard/captures/IMGxxxxx.JPG 저장
```

GPIO1은 내부 pull-up, active-low로 설정됩니다. GPIO1을 GND에 연결하면 HTTP `/capture`와 같은 저장 경로를 실행합니다.

## 추론 경로

```text
OV5647 800x800 RGB565
  -> 요청 이후 새 프레임
  -> 좌상/우상/좌하/우하/중앙 400x400 crop
  -> 각 crop을 224x224로 resize
  -> Keras MobileNetV2 방식 x / 127.5 - 1 전처리
  -> ESP-DL 분류를 5회 실행
  -> 가장 큰 human score와 threshold 0.72482645511627197 비교
  -> JPEG + HTTP 결과 헤더
  -> 선택적으로 JPEG/메타데이터를 C6에 비동기 SDIO 전송
```

라벨 순서는 `no_human`, `human`입니다. 가장 큰 human score가 threshold 이상일 때만 최종 클래스를 `human`으로 결정합니다.

카메라와 공유 JPEG 출력 버퍼는 하나의 mutex로 보호합니다. `/pic`, `/record`, `/classify.jpg`가 동시에 호출되어도 프레임이 다른 요청에 의해 덮어써지지 않습니다.

추론은 `/classify.jpg` 요청에 의해 시작됩니다. 현재 펌웨어에는 10초 주기 등의 백그라운드 촬영·추론 타이머가 없습니다.
