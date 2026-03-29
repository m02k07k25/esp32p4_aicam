# Model Workspace

This directory contains the training and export flow for the `224x224` 2-class classifier used by the firmware.

## Labels

`labels.txt` is the source of truth for class order. The default labels are:

```text
no_human
human
```

Keep `labels.txt` aligned with the firmware labels in `main/infer_config.h`.

## Directory Layout

```text
model/
  labels.txt
  train_model.py
  export_onnx.py
  quantize_espdl.py
  data/
    train/<label>/*.jpg
    val/<label>/*.jpg
    calib/**/*.jpg
  artifacts/
    checkpoints/best.pt
    onnx/classifier_224.onnx
    espdl/classifier_224_p4.espdl
```

`data/train` and `data/val` must contain one directory per label, using the exact order from `labels.txt`.

## Python Environment

Use a project-local `.venv` for model tooling. Do not change the ESP-IDF Python interpreter.

Typical setup on Windows:

```powershell
python -m venv .venv
& .\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
pip install torch torchvision --index-url https://download.pytorch.org/whl/cpu
pip install onnx pillow
pip install esp-ppq
```

## COCO Bootstrap

For a quick `human / no_human` bootstrap dataset, use the selective COCO downloader:

```powershell
python model/prepare_coco_human_dataset.py
```

This downloads COCO annotations plus only the sampled images you need into `download/`, then copies them into:

```text
model/data/train/no_human
model/data/train/human
model/data/val/no_human
model/data/val/human
model/data/calib
```

## Workflow

Train the classifier:

```powershell
python model/train_model.py --epochs 20
```

Export the best checkpoint to ONNX:

```powershell
python model/export_onnx.py
```

Quantize the ONNX graph into the firmware asset:

```powershell
python model/quantize_espdl.py
```

After quantization, the firmware build consumes:

```text
model/artifacts/espdl/classifier_224_p4.espdl
```

The application CMake file fails early if that file does not exist.
