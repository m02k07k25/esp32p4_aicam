| Supported Targets | ESP32-P4 |
| ----------------- | -------- |

# Simple Video Server Classifier Example

## Overview

This project serves one-shot camera frames over HTTP and runs a local `224x224` image classifier on demand.

The current default assumptions are:

- camera sensor: `OV5647`
- capture format: `800x800` sensor frame, converted to `RGB565`
- classifier input: `224x224`
- model type: 2-class MobileNetV2-style classifier exported as `classifier_224_p4.espdl`

Available HTTP endpoints:

| URL | Method | Description |
| --- | ------ | ----------- |
| `/pic` | `GET` | Returns the latest frame as `capture.jpg`. |
| `/record` | `GET` | Returns the raw frame bytes from the current camera buffer. |
| `/classify.jpg` | `GET` | Runs classification on the current frame, returns the original frame as JPEG, and attaches the classification result in HTTP headers. |

Classification headers returned by `/classify.jpg`:

- `X-Class-Index`
- `X-Class-Label`
- `X-Class-Score`
- `X-Inference-Time-Ms`
- `X-Inference-Total-Ms`

The application does not draw overlays on the classified image. The response JPEG is the original frame.

## Model Asset Requirement

The firmware build embeds the classifier model directly from:

```text
model/artifacts/espdl/classifier_224_p4.espdl
```

If that file does not exist, CMake fails early with a clear error. Generate it with the scripts under `model/` before building the firmware.

## Model Workflow

The `model/` directory is a separate Python workspace for classifier training and export. Use a project-local `.venv` for this flow. Do not change the ESP-IDF Python interpreter.

Typical sequence:

```powershell
python model/train_model.py --epochs 20
python model/export_onnx.py
python model/quantize_espdl.py
```

See `model/README.md` for dataset layout and dependency setup.

## Configure the Project

Open the project configuration menu:

```text
idf.py menuconfig
```

Important expectations for this refactor:

- the camera sensor should stay on the `OV5647` path
- the default sensor format should stay at `RAW8 800x800`
- the application will request `RGB565` output from the video device before JPEG encode and classification

## Build and Flash

After the `.espdl` model exists, build and flash as usual:

```text
idf.py -p PORT flash monitor
```

## Example Requests

Fetch a plain JPEG:

```bash
curl http://esp-web.local/pic > capture.jpg
```

Fetch a classified frame and inspect headers:

```bash
curl -i http://esp-web.local/classify.jpg
```

Example response headers:

```text
X-Class-Index: 1
X-Class-Label: human
X-Class-Score: 0.9821
X-Inference-Time-Ms: 24.73
X-Inference-Total-Ms: 38.46
```

## Notes

- The runtime preprocess path is a direct resize from the square camera frame to `224x224`.
- The firmware normalization matches torchvision ImageNet normalization using 0-255-scaled mean and std.
- `labels.txt` and `main/infer_config.h` must keep the same 2-label order.
