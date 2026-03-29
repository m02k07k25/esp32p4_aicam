# Classification Pipeline

## Runtime Flow

The runtime is split into two responsibilities:

- `main/simple_video_server_example.c`
  - camera setup
  - V4L2 buffer dequeue and requeue
  - HTTP endpoint registration
  - JPEG response handling
- `main/infer_bridge.cpp`
  - classifier model load
  - `224x224` preprocess
  - model inference
  - top-1 postprocess

## Endpoints

### `/pic`

Returns the latest frame as `capture.jpg`.

### `/record`

Returns the raw bytes from the current camera buffer.

### `/classify.jpg`

Runs the classifier on the current `RGB565` frame and returns the original frame as JPEG.

Response headers:

- `X-Class-Index`
- `X-Class-Label`
- `X-Class-Score`
- `X-Inference-Time-Ms`
- `X-Inference-Total-Ms`

## Inference Path

The current path is:

```text
OV5647 sensor
  -> 800x800 frame
  -> app requests RGB565 output
  -> /classify.jpg dequeues one frame
  -> infer_bridge preprocesses 800x800 RGB565 -> 224x224 tensor
  -> MobileNetV2-style classifier runs with esp-dl
  -> top-1 class is returned through HTTP headers
  -> the original frame is JPEG-encoded and returned as classify.jpg
  -> V4L2 buffer is queued back to the driver
```

## Model Configuration

Shared runtime settings live in `main/infer_config.h`:

- input width: `224`
- input height: `224`
- label count: `2`
- preprocess mode: `resize`
- classify JPEG quality: `60`

The fixed default labels are:

```text
no_human
human
```

Keep these aligned with `model/labels.txt`.

## Build Integration

The classifier model is embedded from:

```text
model/artifacts/espdl/classifier_224_p4.espdl
```

`main/CMakeLists.txt` stops the build if the file is missing. This keeps the failure close to the actual configuration problem instead of failing later at link time.

## Model Workspace

`model/` is intentionally separate from the ESP-IDF Python environment.

Workflow:

```text
train_model.py
  -> artifacts/checkpoints/best.pt
export_onnx.py
  -> artifacts/onnx/classifier_224.onnx
quantize_espdl.py
  -> artifacts/espdl/classifier_224_p4.espdl
```

The training, export, calibration, and firmware preprocess paths all use the same `224x224` resize policy and ImageNet normalization.
