# ESP32 Video Server with YOLO Pipeline 설명

## 📋 목차
1. [프로젝트 개요](#프로젝트-개요)
2. [카메라 파이프라인 (사진 촬영)](#카메라-파이프라인-사진-촬영)
3. [YOLO 객체 감지 파이프라인](#yolo-객체-감지-파이프라인)
4. [HTTP 웹 서버 호스팅](#http-웹-서버-호스팅)
5. [API 엔드포인트](#api-엔드포인트)

---

## 프로젝트 개요

ESP32-P4 기반의 실시간 비디오 스트리밍 및 YOLO 객체 감지 시스템입니다. MIPI CSI 또는 DVP 카메라에서 영상을 캡처하고, YOLO 모델을 사용하여 객체를 감지한 후, HTTP 서버를 통해 결과를 제공합니다.

**주요 특징:**
- V4L2 기반 비디오 캡처
- JPEG 하드웨어 인코딩
- YOLO 기반 실시간 객체 감지
- HTTP 웹 서버 (포트 80)
- mDNS 지원 (esp-web.local)

---

## 카메라 파이프라인 (사진 촬영)

### 1️⃣ 비디오 디바이스 초기화

#### `esp_video_init()`
```c
esp_err_t esp_video_init(const esp_video_init_config_t *cam_config);
```

**파라미터:**
- `cam_config`: 카메라 설정 구조체
  - CSI 카메라: I2C 포트, SCL/SDA 핀, 리셋/파워다운 핀
  - DVP 카메라: 데이터 핀, VSYNC, PCLK, XCLK 등

**역할:** 카메라 하드웨어를 초기화하고 센서와의 통신을 설정합니다.

---

### 2️⃣ 비디오 디바이스 열기

#### `app_video_open()`
```c
int app_video_open(char *dev, example_fmt_t init_fmt);
```

**파라미터:**
- `dev`: 디바이스 경로 (예: `/dev/video0`)
- `init_fmt`: 출력 포맷
  - `EXAMPLE_VIDEO_FMT_RAW8`: RAW 8-bit
  - `EXAMPLE_VIDEO_FMT_RGB565`: RGB565 (YOLO 감지용)
  - `EXAMPLE_VIDEO_FMT_RGB888`: RGB888
  - `EXAMPLE_VIDEO_FMT_YUV422`: YUV422
  - `EXAMPLE_VIDEO_FMT_YUV420`: YUV420
  - `EXAMPLE_VIDEO_FMT_GREY`: Grayscale

**반환값:** 파일 디스크립터 (fd) 또는 -1 (에러 시)

**역할:**
1. V4L2 디바이스를 엽니다
2. 디바이스 capability를 조회합니다
3. 원하는 포맷으로 변경합니다 (필요 시)
4. 파일 디스크립터를 반환합니다

**내부 동작:**
```c
// 1. 디바이스 열기
int fd = open(dev, O_RDONLY);

// 2. Capability 조회
struct v4l2_capability capability;
ioctl(fd, VIDIOC_QUERYCAP, &capability);

// 3. 현재 포맷 가져오기
struct v4l2_format default_format;
ioctl(fd, VIDIOC_G_FMT, &default_format);

// 4. 포맷 변경 (필요 시)
ioctl(fd, VIDIOC_S_FMT, &format);
```

---

### 3️⃣ 웹캠 객체 생성 및 버퍼 설정

#### `new_web_cam()`
```c
esp_err_t new_web_cam(int cam_fd, web_cam_t **ret_wc);
```

**파라미터:**
- `cam_fd`: 카메라 파일 디스크립터
- `ret_wc`: 생성된 웹캠 구조체 포인터

**역할:**
1. 웹캠 구조체 할당
2. JPEG 인코더 설정
3. 비디오 버퍼 할당 (mmap)
4. 스트리밍 시작

**웹캠 구조체 (`web_cam_t`):**
```c
typedef struct web_cam {
    int fd;                                    // 파일 디스크립터
    uint32_t width;                            // 이미지 너비
    uint32_t height;                           // 이미지 높이
    uint32_t pixel_format;                     // 픽셀 포맷
    jpeg_encode_cfg_t jpeg_enc_config;         // JPEG 인코더 설정
    size_t jpeg_enc_output_buf_alloced_size;   // JPEG 출력 버퍼 크기
    jpeg_encoder_handle_t jpeg_handle;         // JPEG 인코더 핸들
    uint8_t *jpeg_out_buf;                     // JPEG 출력 버퍼
    uint8_t *detect_out_buf;                   // YOLO 감지 결과 버퍼
    size_t detect_out_buf_capacity;            // 감지 버퍼 용량
    uint8_t *buffer[EXAMPLE_VIDEO_BUFFER_COUNT]; // 비디오 프레임 버퍼
} web_cam_t;
```

**내부 동작:**
```c
// 1. 웹캠 구조체 할당
web_cam_t *wc = malloc(sizeof(web_cam_t));

// 2. JPEG 인코더 생성
jpeg_new_encoder_engine(&encode_eng_cfg, &wc->jpeg_handle);

// 3. JPEG 출력 버퍼 할당
wc->jpeg_out_buf = jpeg_alloc_encoder_mem(...);

// 4. 비디오 버퍼 요청
struct v4l2_requestbuffers req;
req.count = EXAMPLE_VIDEO_BUFFER_COUNT; // 2개
ioctl(wc->fd, VIDIOC_REQBUFS, &req);

// 5. 각 버퍼를 mmap으로 매핑
for (int i = 0; i < ARRAY_SIZE(wc->buffer); i++) {
    struct v4l2_buffer buf;
    ioctl(wc->fd, VIDIOC_QUERYBUF, &buf);
    wc->buffer[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, 
                         MAP_SHARED, wc->fd, buf.m.offset);
    ioctl(wc->fd, VIDIOC_QBUF, &buf); // 버퍼 큐에 추가
}

// 6. 스트리밍 시작
ioctl(wc->fd, VIDIOC_STREAMON, &type);
```

---

### 4️⃣ 프레임 캡처

#### `VIDIOC_DQBUF` (Dequeue Buffer)
```c
struct v4l2_buffer buf;
memset(&buf, 0, sizeof(buf));
buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
buf.memory = V4L2_MEMORY_MMAP;
ioctl(wc->fd, VIDIOC_DQBUF, &buf);
```

**역할:** 카메라에서 채워진 프레임 버퍼를 가져옵니다.

**반환 데이터:**
- `buf.index`: 버퍼 인덱스
- `buf.bytesused`: 실제 사용된 바이트 수
- `wc->buffer[buf.index]`: 프레임 데이터

#### `VIDIOC_QBUF` (Queue Buffer)
```c
ioctl(wc->fd, VIDIOC_QBUF, &buf);
```

**역할:** 사용한 버퍼를 다시 큐에 반환하여 재사용합니다.

---

### 5️⃣ JPEG 인코딩

#### `jpeg_encoder_process()`
```c
esp_err_t jpeg_encoder_process(
    jpeg_encoder_handle_t jpeg_handle,
    jpeg_encode_cfg_t *encode_cfg,
    uint8_t *in_buf,
    uint32_t in_len,
    uint8_t *out_buf,
    uint32_t out_len,
    uint32_t *out_size
);
```

**파라미터:**
- `jpeg_handle`: JPEG 인코더 핸들
- `encode_cfg`: 인코딩 설정
  - `src_type`: 입력 포맷 (RGB565, YUV422 등)
  - `image_quality`: 품질 (0-100, 기본 80)
  - `width`, `height`: 이미지 크기
  - `sub_sample`: 서브샘플링 방식
- `in_buf`: 입력 프레임 데이터
- `in_len`: 입력 데이터 크기
- `out_buf`: 출력 JPEG 버퍼
- `out_len`: 출력 버퍼 크기
- `out_size`: 실제 인코딩된 JPEG 크기 (출력)

**역할:** RAW 이미지 데이터를 JPEG로 하드웨어 인코딩합니다.

---

## YOLO 객체 감지 파이프라인

### 1️⃣ YOLO 브릿지 초기화

#### `yolo_bridge_init()`
```c
esp_err_t yolo_bridge_init(void);
```

**파라미터:** 없음

**반환값:** `ESP_OK` 또는 에러 코드

**역할:**
1. Mutex (세마포어) 생성 - 동시 접근 제어
2. YOLO 모델 로드 (COCODetect)
3. 첫 HTTP 요청 지연 시간 감소를 위해 eager-loading

**내부 동작:**
```cpp
// 1. Mutex 생성
s_infer_lock = xSemaphoreCreateMutex();

// 2. COCO 객체 감지 모델 생성
s_detector = new COCODetect(
    static_cast<COCODetect::model_type_t>(CONFIG_DEFAULT_COCO_DETECT_MODEL), 
    false
);
```

---

### 2️⃣ RGB565 프레임 처리 및 객체 감지

#### `yolo_bridge_process_rgb565()`
```c
esp_err_t yolo_bridge_process_rgb565(
    const uint8_t *rgb565_frame,
    uint16_t width,
    uint16_t height,
    uint8_t jpeg_quality,
    uint8_t **jpeg_buf,
    size_t *jpeg_len,
    size_t *jpeg_buf_capacity,
    int *box_count,
    float *inference_ms
);
```

**파라미터:**
- `rgb565_frame`: 입력 RGB565 프레임 데이터
- `width`: 프레임 너비
- `height`: 프레임 높이
- `jpeg_quality`: JPEG 품질 (0-100, 기본 60)
- `jpeg_buf`: 출력 JPEG 버퍼 포인터 (자동 확장)
- `jpeg_len`: 출력 JPEG 크기 (출력)
- `jpeg_buf_capacity`: 버퍼 용량 (입출력)
- `box_count`: 감지된 객체 수 (출력, 선택적)
- `inference_ms`: 추론 시간(ms) (출력, 선택적)

**반환값:** `ESP_OK`, `ESP_ERR_INVALID_STATE`, `ESP_ERR_TIMEOUT`, `ESP_FAIL` 등

**역할:**
1. RGB565 프레임을 SPIRAM에 복사
2. YOLO 모델로 객체 감지 실행
3. 감지된 객체에 빨간색 박스 그리기
4. 결과 이미지를 JPEG로 인코딩
5. 성능 메트릭 반환

**내부 동작 흐름:**

```cpp
// 1. Mutex 획득 (최대 5초 대기)
xSemaphoreTake(s_infer_lock, pdMS_TO_TICKS(5000));

// 2. 프레임 복사 (SPIRAM 선호)
size_t frame_size = width * height * 2; // RGB565는 픽셀당 2바이트
frame_copy = heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
memcpy(frame_copy, rgb565_frame, frame_size);

// 3. 이미지 구조체 생성
dl::image::img_t frame;
frame.data = frame_copy;
frame.width = width;
frame.height = height;
frame.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565;

// 4. YOLO 추론 실행
start_us = esp_timer_get_time();
results = &s_detector->run(frame);
end_us = esp_timer_get_time();

// 5. 감지된 객체에 박스 그리기
for (const auto &res : *results) {
    int x1 = res.box[0];  // 좌상단 X
    int y1 = res.box[1];  // 좌상단 Y
    int x2 = res.box[2];  // 우하단 X
    int y2 = res.box[3];  // 우하단 Y
    
    // 경계 검사 및 박스 그리기
    dl::image::draw_hollow_rectangle(frame, x1, y1, x2, y2, 
                                     k_box_color_rgb565, 3);
    detected_boxes++;
}

// 6. JPEG 인코딩
#if CONFIG_SOC_JPEG_CODEC_SUPPORTED
    encoded_jpeg = dl::image::hw_encode_jpeg(frame, 0, jpeg_quality);
#else
    encoded_jpeg = dl::image::sw_encode_jpeg(frame, 0, jpeg_quality);
#endif

// 7. 출력 버퍼에 복사
memcpy(*jpeg_buf, encoded_jpeg.data, encoded_jpeg.data_len);
*jpeg_len = encoded_jpeg.data_len;
*box_count = detected_boxes;
*inference_ms = (float)(end_us - start_us) / 1000.0f;

// 8. 메모리 해제 및 Mutex 반환
heap_caps_free(encoded_jpeg.data);
heap_caps_free(frame_copy);
xSemaphoreGive(s_infer_lock);
```

**감지 결과 구조:**
```cpp
dl::detect::result_t {
    std::vector<int> box;      // [x1, y1, x2, y2]
    float score;               // 신뢰도 점수
    int category;              // COCO 카테고리 ID
}
```

---

### 3️⃣ 박스 색상 및 두께

```cpp
static const std::vector<uint8_t> k_box_color_rgb565 = {0x00, 0xF8}; // 빨간색 (리틀 엔디안)
```

- **색상:** 빨간색 (RGB565: 0xF800)
- **두께:** 3픽셀
- **포맷:** RGB565 리틀 엔디안 (바이트 순서: 하위 바이트, 상위 바이트)

---

## HTTP 웹 서버 호스팅

### 1️⃣ HTTP 서버 초기화

#### `http_server_init()`
```c
esp_err_t http_server_init(int index, web_cam_t *web_cam);
```

**파라미터:**
- `index`: 서버 인덱스 (포트 오프셋)
- `web_cam`: 웹캠 컨텍스트 구조체

**역할:**
1. HTTP 서버 생성
2. URI 핸들러 등록
3. 서버 시작

**서버 설정:**
```c
httpd_config_t config = HTTPD_DEFAULT_CONFIG();
config.stack_size = 1024 * 8;        // 8KB 스택
config.server_port = 80 + index;     // 포트 (기본 80)
config.ctrl_port = 32768 + index;    // 제어 포트
```

---

### 2️⃣ mDNS 설정

#### `initialise_mdns()`
```c
void initialise_mdns(void);
```

**역할:** mDNS 서비스 초기화

**설정 값:**
- **호스트명:** `esp-web.local`
- **인스턴스:** `simple video web`
- **서비스:** `_http._tcp`
- **포트:** 80

**사용 예시:**
```
http://esp-web.local/pic
http://esp-web.local/detect.jpg
```

---

## API 엔드포인트

### 1️⃣ `/pic` - 일반 사진 캡처

#### `pic_handler()`

**메서드:** `GET`

**응답:**
- **Content-Type:** `image/jpeg`
- **Content-Disposition:** `inline; filename=capture.jpg`
- **Access-Control-Allow-Origin:** `*` (CORS 허용)

**동작 흐름:**
```c
// 1. 프레임 가져오기
ioctl(wc->fd, VIDIOC_DQBUF, &buf);

// 2. JPEG 인코딩 (카메라가 JPEG 출력이 아닌 경우)
if (wc->pixel_format != V4L2_PIX_FMT_JPEG) {
    jpeg_encoder_process(wc->jpeg_handle, &wc->jpeg_enc_config, 
                        wc->buffer[buf.index], buf.bytesused, 
                        wc->jpeg_out_buf, wc->jpeg_enc_output_buf_alloced_size, 
                        &jpeg_encoded_size);
}

// 3. HTTP 응답 전송
httpd_resp_send_chunk(req, (const char *)jpeg_ptr, jpeg_size);

// 4. 버퍼 반환
ioctl(wc->fd, VIDIOC_QBUF, &buf);

// 5. 응답 완료
httpd_resp_send_chunk(req, NULL, 0);
```

**사용 예시:**
```bash
curl http://esp-web.local/pic > capture.jpg
```

---

### 2️⃣ `/detect.jpg` - YOLO 객체 감지

#### `detect_handler()`

**메서드:** `GET`

**응답:**
- **Content-Type:** `image/jpeg`
- **Content-Disposition:** `inline; filename=detect.jpg`
- **Access-Control-Allow-Origin:** `*`

**커스텀 헤더:**
- `X-Detect-Boxes`: 감지된 객체 수
- `X-Detect-Time-Ms`: 추론 시간 (밀리초)
- `X-Detect-Total-Ms`: 총 처리 시간 (밀리초)

**동작 흐름:**
```c
// 1. RGB565 포맷 확인
if (wc->pixel_format != EXAMPLE_VIDEO_FMT_RGB565) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                       "YOLO handler requires RGB565 camera format");
    return ESP_FAIL;
}

// 2. 프레임 가져오기
int64_t req_start_us = esp_timer_get_time();
ioctl(wc->fd, VIDIOC_DQBUF, &buf);

// 3. YOLO 처리
yolo_bridge_process_rgb565(
    wc->buffer[buf.index],           // RGB565 프레임
    wc->width,                        // 너비
    wc->height,                       // 높이
    DETECT_JPEG_QUALITY,              // JPEG 품질 (60)
    &wc->detect_out_buf,              // 출력 버퍼
    &detect_jpeg_len,                 // JPEG 길이
    &wc->detect_out_buf_capacity,     // 버퍼 용량
    &box_count,                       // 감지된 객체 수
    &inference_ms                     // 추론 시간
);

// 4. 버퍼 반환
ioctl(wc->fd, VIDIOC_QBUF, &buf);

// 5. 헤더 설정
httpd_resp_set_hdr(req, "X-Detect-Boxes", "3");
httpd_resp_set_hdr(req, "X-Detect-Time-Ms", "145.23");
httpd_resp_set_hdr(req, "X-Detect-Total-Ms", "167.45");

// 6. JPEG 이미지 전송
httpd_resp_send_chunk(req, (const char *)wc->detect_out_buf, detect_jpeg_len);

// 7. 응답 완료
httpd_resp_send_chunk(req, NULL, 0);
```

**사용 예시:**
```bash
curl -I http://esp-web.local/detect.jpg
# X-Detect-Boxes: 3
# X-Detect-Time-Ms: 145.23
# X-Detect-Total-Ms: 167.45

curl http://esp-web.local/detect.jpg > detect.jpg
```

**JavaScript 예시:**
```javascript
fetch('http://esp-web.local/detect.jpg')
  .then(response => {
    const boxes = response.headers.get('X-Detect-Boxes');
    const inferenceTime = response.headers.get('X-Detect-Time-Ms');
    console.log(`감지된 객체: ${boxes}개, 추론 시간: ${inferenceTime}ms`);
    return response.blob();
  })
  .then(blob => {
    const img = document.createElement('img');
    img.src = URL.createObjectURL(blob);
    document.body.appendChild(img);
  });
```

---

### 3️⃣ `/record` - RAW 데이터 다운로드

#### `record_bin_handler()`

**메서드:** `GET`

**응답:**
- **Content-Type:** `application/octet-stream`
- **Content-Disposition:** `inline; filename=record.bin`

**동작 흐름:**
```c
// 1. 프레임 가져오기
ioctl(wc->fd, VIDIOC_DQBUF, &buf);

// 2. RAW 데이터 전송
httpd_resp_send_chunk(req, (const char *)wc->buffer[buf.index], buf.bytesused);

// 3. 버퍼 반환
ioctl(wc->fd, VIDIOC_QBUF, &buf);

// 4. 응답 완료
httpd_resp_send_chunk(req, NULL, 0);
```

**사용 예시:**
```bash
curl http://esp-web.local/record > frame.bin
```

---

## 전체 파이프라인 요약

### 📸 사진 캡처 흐름 (`/pic`)
```
Camera Sensor → V4L2 Driver → VIDIOC_DQBUF → RGB565/YUV Frame
                                                      ↓
                                             JPEG Encoder (HW)
                                                      ↓
                                             HTTP Response → Client
                                                      ↓
                                             VIDIOC_QBUF (버퍼 반환)
```

### 🤖 YOLO 감지 흐름 (`/detect.jpg`)
```
Camera Sensor → V4L2 Driver → VIDIOC_DQBUF → RGB565 Frame
                                                      ↓
                                        Copy to SPIRAM (frame_copy)
                                                      ↓
                                        YOLO Model Inference (COCODetect)
                                                      ↓
                                        Draw Bounding Boxes (Red, 3px)
                                                      ↓
                                        JPEG Encoder (HW/SW)
                                                      ↓
                                        HTTP Response + Headers
                                        (X-Detect-Boxes, X-Detect-Time-Ms)
                                                      ↓
                                        VIDIOC_QBUF (버퍼 반환)
```

---

## 주요 설정 값

| 설정 | 값 | 설명 |
|------|-----|------|
| `EXAMPLE_VIDEO_BUFFER_COUNT` | 2 | 비디오 버퍼 개수 |
| `JPEG_ENC_QUALITY` | 80 | 일반 JPEG 품질 |
| `DETECT_JPEG_QUALITY` | 60 | YOLO 감지 JPEG 품질 |
| `HTTP 포트` | 80 | 웹 서버 포트 |
| `mDNS 호스트명` | esp-web.local | 로컬 도메인 이름 |
| `카메라 포맷` | RGB565 | YOLO 처리용 포맷 |
| `Mutex 타임아웃` | 5000ms | YOLO 추론 대기 시간 |
| `박스 색상` | 0xF800 (빨간색) | RGB565 포맷 |
| `박스 두께` | 3px | 테두리 두께 |

---

## 성능 최적화

### 메모리 관리
1. **SPIRAM 우선 할당**: YOLO 처리에 필요한 큰 버퍼는 SPIRAM에 우선 할당
2. **버퍼 재사용**: `detect_out_buf`는 용량이 충분하면 재할당하지 않음
3. **mmap 사용**: 비디오 버퍼는 zero-copy를 위해 mmap 사용

### 동시성 제어
- **Mutex (세마포어)**: 동시에 하나의 YOLO 추론만 실행 (리소스 경합 방지)
- **타임아웃**: 5초 이내 Mutex 획득 실패 시 에러 반환

### Eager Loading
- **모델 사전 로드**: `yolo_bridge_init()`에서 YOLO 모델을 미리 로드하여 첫 요청 지연 감소

---

## 에러 처리

| 에러 코드 | 원인 | 해결 방법 |
|-----------|------|-----------|
| `ESP_ERR_INVALID_STATE` | YOLO 미초기화 | `yolo_bridge_init()` 호출 확인 |
| `ESP_ERR_TIMEOUT` | Mutex 타임아웃 | 다른 요청 대기 중, 재시도 |
| `ESP_ERR_NO_MEM` | 메모리 부족 | 버퍼 크기 줄이기 또는 SPIRAM 확인 |
| `ESP_FAIL` | JPEG 인코딩 실패 | 입력 데이터 유효성 확인 |
| HTTP 500 (포맷 에러) | RGB565가 아님 | 카메라 포맷 확인 |

---

## 디버깅 로그 예시

```
I (12345) example: Starting HTTP server on port: '80' (/pic, /record, /detect.jpg)
I (12678) yolo_bridge: detect done: boxes=3, inference=145.23 ms, total=167.45 ms, jpeg=45678 B
```

**로그 정보:**
- `boxes`: 감지된 객체 수
- `inference`: 순수 YOLO 추론 시간
- `total`: 프레임 복사 + 추론 + 박스 그리기 + JPEG 인코딩 총 시간
- `jpeg`: 최종 JPEG 파일 크기 (바이트)

---

## 추가 정보

### COCO 데이터셋 카테고리
YOLO 모델은 COCO 데이터셋으로 학습되었으며, 다음과 같은 객체를 감지할 수 있습니다:
- 사람, 자전거, 자동차, 오토바이, 비행기, 버스, 기차, 트럭
- 교통 표지판, 소화전, 의자, 소파, 식물, 침대, 식탁
- TV, 노트북, 마우스, 키보드, 휴대폰, 책
- 및 80개 카테고리 전체

### V4L2 버퍼 관리
- **MMAP 방식**: 커널과 사용자 공간이 버퍼를 공유하여 효율적
- **더블 버퍼링**: 2개 버퍼로 캡처와 처리를 병렬화

### JPEG 인코딩 방식
- **하드웨어 인코딩**: ESP32-P4의 JPEG 코덱 사용 (`CONFIG_SOC_JPEG_CODEC_SUPPORTED`)
- **소프트웨어 인코딩**: 하드웨어 미지원 시 폴백

---

## 문의 및 기여

- **ESP-IDF 버전**: v5.x 이상 권장
- **지원 칩셋**: ESP32-P4
- **라이선스**: ESPRESSIF MIT

