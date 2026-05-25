from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path

from model_utils import CALIB_ROOT, ESPDL_PATH, IMAGE_SIZE, IMAGENET_MEAN, IMAGENET_STD, ONNX_PATH, collect_images, ensure_parent_dir


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Quantize classifier_224.onnx into an ESP-DL .espdl model.")
    parser.add_argument("--onnx", type=Path, default=ONNX_PATH, help="Input ONNX model.")
    parser.add_argument("--calib-dir", type=Path, default=CALIB_ROOT, help="Calibration image directory.")
    parser.add_argument("--output", type=Path, default=ESPDL_PATH, help="Output .espdl file.")
    parser.add_argument("--target", default="esp32p4", help="ESP-PPQ target platform.")
    parser.add_argument("--bits", type=int, default=8, choices=[8], help="Quantization bits.")
    parser.add_argument("--batch-size", type=int, default=1, help="Calibration batch size. Use 1 for the fixed-shape ONNX export.")
    parser.add_argument("--calib-steps", type=int, default=32, help="Maximum calibration steps.")
    parser.add_argument("--workers", type=int, default=0, help="DataLoader worker processes.")
    parser.add_argument("--device", default="cpu", help='Torch device string. Keep this at "cpu" for a CPU-only .venv.')
    parser.add_argument("--skip-export-test-values", action="store_true", help="Disable test-value export in the .espdl file.")
    parser.add_argument("--verbose", type=int, default=1, help="ESP-PPQ verbosity.")
    return parser.parse_args()


def import_ppq():
    if importlib.util.find_spec("ppq") is not None:
        from ppq import QuantizationSettingFactory
        from ppq.api import espdl_quantize_onnx

        return QuantizationSettingFactory, espdl_quantize_onnx

    if importlib.util.find_spec("esp_ppq") is not None:
        from esp_ppq import QuantizationSettingFactory
        from esp_ppq.api import espdl_quantize_onnx

        return QuantizationSettingFactory, espdl_quantize_onnx

    raise SystemExit(
        "ESP-PPQ is not installed in the active interpreter. "
        "Install it inside this project's .venv before running quantize_espdl.py."
    )


def main() -> None:
    args = parse_args()
    QuantizationSettingFactory, espdl_quantize_onnx = import_ppq()

    if args.batch_size != 1:
        raise ValueError("This quantization flow expects a fixed-shape ONNX input of [1, 3, 224, 224], so --batch-size must be 1.")

    import torch
    from PIL import Image
    from torch.utils.data import DataLoader, Dataset
    from torchvision import transforms

    image_paths = collect_images(args.calib_dir)
    if not image_paths:
        raise RuntimeError(f"No calibration images found under {args.calib_dir}")

    transform = transforms.Compose(
        [
            transforms.Resize((IMAGE_SIZE, IMAGE_SIZE)),
            transforms.ToTensor(),
            transforms.Normalize(mean=IMAGENET_MEAN, std=IMAGENET_STD),
        ]
    )

    class CalibrationImageDataset(Dataset):
        def __init__(self, paths: list[Path]):
            self._paths = paths

        def __len__(self) -> int:
            return len(self._paths)

        def __getitem__(self, index: int):
            with Image.open(self._paths[index]) as image:
                return transform(image.convert("RGB"))

    dataset = CalibrationImageDataset(image_paths)
    dataloader = DataLoader(
        dataset,
        batch_size=args.batch_size,
        shuffle=False,
        num_workers=args.workers,
        pin_memory=False,
    )
    calib_steps = min(args.calib_steps, len(dataloader))
    if calib_steps == 0:
        raise RuntimeError("Calibration DataLoader produced zero batches.")

    quant_setting = QuantizationSettingFactory.espdl_setting()
    ensure_parent_dir(args.output)
    espdl_quantize_onnx(
        onnx_import_file=str(args.onnx),
        espdl_export_file=str(args.output),
        calib_dataloader=dataloader,
        calib_steps=calib_steps,
        input_shape=[1, 3, IMAGE_SIZE, IMAGE_SIZE],
        target=args.target,
        num_of_bits=args.bits,
        collate_fn=lambda batch: batch.to(args.device),
        setting=quant_setting,
        device=args.device,
        error_report=False,
        skip_export=False,
        export_test_values=not args.skip_export_test_values,
        verbose=args.verbose,
    )

    print(f"Exported ESP-DL model to {args.output}")


if __name__ == "__main__":
    main()
