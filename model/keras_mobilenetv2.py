from __future__ import annotations

import argparse
import hashlib
from functools import partial
from pathlib import Path
from typing import Any


KERAS_MOBILENET_V2_035_FILENAME = (
    "mobilenet_v2_weights_tf_dim_ordering_tf_kernels_0.35_224_no_top.h5"
)
KERAS_MOBILENET_V2_035_URL = (
    "https://storage.googleapis.com/tensorflow/keras-applications/"
    f"mobilenet_v2/{KERAS_MOBILENET_V2_035_FILENAME}"
)
KERAS_MOBILENET_V2_035_SHA256 = (
    "df1c3db1d0d9193719c4c70f707c56f7021f999f825bdc57454c275cf6def19f"
)
KERAS_MOBILENET_V2_035_SIZE = 2_019_640

MODEL_ROOT = Path(__file__).resolve().parent
PROJECT_ROOT = MODEL_ROOT.parent
DEFAULT_KERAS_WEIGHTS_PATH = (
    PROJECT_ROOT / "download" / "pretrained" / KERAS_MOBILENET_V2_035_FILENAME
)

KERAS_BATCH_NORM_EPS = 1e-3
KERAS_BATCH_NORM_MOMENTUM = 1e-3


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_keras_weights_file(path: Path) -> None:
    if not path.is_file():
        raise FileNotFoundError(f"Missing Keras MobileNetV2 weights: {path}")

    size = path.stat().st_size
    if size != KERAS_MOBILENET_V2_035_SIZE:
        raise RuntimeError(
            f"Unexpected size for {path}: {size} bytes; "
            f"expected {KERAS_MOBILENET_V2_035_SIZE}."
        )

    digest = file_sha256(path)
    if digest != KERAS_MOBILENET_V2_035_SHA256:
        raise RuntimeError(
            f"SHA-256 mismatch for {path}: {digest}; "
            f"expected {KERAS_MOBILENET_V2_035_SHA256}."
        )


def ensure_keras_weights(
    path: Path = DEFAULT_KERAS_WEIGHTS_PATH,
    *,
    allow_download: bool = True,
    force_download: bool = False,
) -> Path:
    path = path.resolve()

    if path.exists() and not force_download:
        validate_keras_weights_file(path)
        return path

    if not allow_download:
        raise FileNotFoundError(
            f"Pretrained weights are not cached at {path}. "
            "Run `python model/download_pretrained.py` while online first."
        )

    if force_download and path.exists():
        path.unlink()

    import torch

    path.parent.mkdir(parents=True, exist_ok=True)
    print(f"Downloading official Keras MobileNetV2 0.35 weights to {path} ...")
    torch.hub.download_url_to_file(
        KERAS_MOBILENET_V2_035_URL,
        str(path),
        hash_prefix=KERAS_MOBILENET_V2_035_SHA256,
        progress=True,
    )
    validate_keras_weights_file(path)
    return path


def _make_keras_same_padding_conv(source):
    import torch
    from torch import nn

    class KerasSamePaddingConv2d(nn.Conv2d):
        """TF SAME padding for the fixed even-sized MobileNetV2 224 path."""

        def forward(self, input_tensor):
            padded = torch.nn.functional.pad(input_tensor, (0, 1, 0, 1))
            return self._conv_forward(padded, self.weight, self.bias)

    replacement = KerasSamePaddingConv2d(
        in_channels=source.in_channels,
        out_channels=source.out_channels,
        kernel_size=source.kernel_size,
        stride=source.stride,
        padding=0,
        dilation=source.dilation,
        groups=source.groups,
        bias=source.bias is not None,
        padding_mode="zeros",
        device=source.weight.device,
        dtype=source.weight.dtype,
    )
    replacement.load_state_dict(source.state_dict())
    replacement.train(source.training)
    return replacement


def _apply_keras_same_padding(model) -> None:
    from torch import nn

    replacements = [
        (model.features[0], 0),
        (model.features[2].conv[1], 0),
        (model.features[4].conv[1], 0),
        (model.features[7].conv[1], 0),
        (model.features[14].conv[1], 0),
    ]

    for container, index in replacements:
        source = container[index]
        if not isinstance(source, nn.Conv2d):
            raise TypeError(f"Expected Conv2d at Keras SAME padding location, got {type(source)!r}.")
        if source.kernel_size != (3, 3) or source.stride != (2, 2):
            raise ValueError(
                "Keras SAME padding locations must be stride-2 3x3 convolutions; "
                f"got kernel={source.kernel_size}, stride={source.stride}."
            )
        container[index] = _make_keras_same_padding_conv(source)


def build_keras_compatible_mobilenet_v2(
    *,
    num_classes: int,
    width_mult: float,
):
    import math

    import torch.nn as nn
    import torchvision

    if not math.isclose(width_mult, 0.35):
        raise ValueError(
            "The official Keras checkpoint integrated here is only compatible "
            f"with width_mult=0.35, got {width_mult}."
        )

    norm_layer = partial(
        nn.BatchNorm2d,
        eps=KERAS_BATCH_NORM_EPS,
        momentum=KERAS_BATCH_NORM_MOMENTUM,
    )
    model = torchvision.models.mobilenet_v2(
        weights=None,
        width_mult=width_mult,
        num_classes=num_classes,
        norm_layer=norm_layer,
    )
    _apply_keras_same_padding(model)
    return model


def _all_h5_dataset_names(h5_file) -> set[str]:
    import h5py

    names: set[str] = set()

    def collect(name: str, item) -> None:
        if isinstance(item, h5py.Dataset):
            names.add(name)

    h5_file.visititems(collect)
    return names


def load_keras_backbone(model, weights_path: Path) -> dict[str, Any]:
    try:
        import h5py
    except ImportError as exc:
        raise RuntimeError(
            "h5py is required to convert the official Keras weights. "
            "Install it with `.venv\\Scripts\\python.exe -m pip install h5py`."
        ) from exc

    import numpy as np
    import torch
    from torch import nn

    validate_keras_weights_file(weights_path)
    consumed: set[str] = set()

    def read_array(h5_file, dataset_name: str) -> np.ndarray:
        if dataset_name in consumed:
            raise RuntimeError(f"Keras dataset was mapped more than once: {dataset_name}")
        if dataset_name not in h5_file:
            raise KeyError(f"Missing Keras dataset: {dataset_name}")
        consumed.add(dataset_name)
        return np.asarray(h5_file[dataset_name][()])

    def copy_tensor(destination, array: np.ndarray, source_name: str) -> None:
        converted = torch.from_numpy(np.ascontiguousarray(array))
        if tuple(converted.shape) != tuple(destination.shape):
            raise RuntimeError(
                f"Shape mismatch mapping {source_name}: source={tuple(converted.shape)}, "
                f"destination={tuple(destination.shape)}."
            )
        destination.copy_(converted.to(device=destination.device, dtype=destination.dtype))

    def copy_conv(h5_file, destination: nn.Conv2d, group_name: str, *, depthwise: bool = False) -> None:
        parameter_name = "depthwise_kernel:0" if depthwise else "kernel:0"
        source_name = f"{group_name}/{group_name}/{parameter_name}"
        array = read_array(h5_file, source_name)
        if depthwise:
            array = array.transpose(2, 3, 0, 1)
        else:
            array = array.transpose(3, 2, 0, 1)
        copy_tensor(destination.weight, array, source_name)

    def copy_batch_norm(h5_file, destination: nn.BatchNorm2d, group_name: str) -> None:
        mappings = (
            ("gamma:0", destination.weight),
            ("beta:0", destination.bias),
            ("moving_mean:0", destination.running_mean),
            ("moving_variance:0", destination.running_var),
        )
        for parameter_name, destination_tensor in mappings:
            source_name = f"{group_name}/{group_name}/{parameter_name}"
            copy_tensor(destination_tensor, read_array(h5_file, source_name), source_name)
        destination.num_batches_tracked.zero_()

    with h5py.File(weights_path, "r") as h5_file, torch.no_grad():
        copy_conv(h5_file, model.features[0][0], "Conv1")
        copy_batch_norm(h5_file, model.features[0][1], "bn_Conv1")

        block_zero = model.features[1].conv
        copy_conv(h5_file, block_zero[0][0], "mobl0_conv_0_depthwise", depthwise=True)
        copy_batch_norm(h5_file, block_zero[0][1], "bn0_conv_0_bn_depthwise")
        copy_conv(h5_file, block_zero[1], "mobl0_conv_0_project")
        copy_batch_norm(h5_file, block_zero[2], "bn0_conv_0_bn_project")

        for block_id in range(1, 17):
            block = model.features[block_id + 1].conv
            copy_conv(h5_file, block[0][0], f"mobl{block_id}_conv_{block_id}_expand")
            copy_batch_norm(h5_file, block[0][1], f"bn{block_id}_conv_{block_id}_bn_expand")
            copy_conv(
                h5_file,
                block[1][0],
                f"mobl{block_id}_conv_{block_id}_depthwise",
                depthwise=True,
            )
            copy_batch_norm(h5_file, block[1][1], f"bn{block_id}_conv_{block_id}_bn_depthwise")
            copy_conv(h5_file, block[2], f"mobl{block_id}_conv_{block_id}_project")
            copy_batch_norm(h5_file, block[3], f"bn{block_id}_conv_{block_id}_bn_project")

        copy_conv(h5_file, model.features[18][0], "Conv_1")
        copy_batch_norm(h5_file, model.features[18][1], "Conv_1_bn")

        all_datasets = _all_h5_dataset_names(h5_file)
        if consumed != all_datasets:
            missing = sorted(all_datasets - consumed)
            unexpected = sorted(consumed - all_datasets)
            raise RuntimeError(
                "Keras conversion did not consume the checkpoint exactly: "
                f"unmapped={missing}, unexpected={unexpected}."
            )

    provenance = {
        "name": "Keras MobileNetV2 alpha=0.35 ImageNet no-top",
        "url": KERAS_MOBILENET_V2_035_URL,
        "sha256": KERAS_MOBILENET_V2_035_SHA256,
        "filename": weights_path.name,
        "converted_dataset_count": len(consumed),
    }
    model.pretrained_provenance = provenance
    return provenance


def initialize_keras_pretrained_backbone(
    model,
    *,
    weights_path: Path | None = None,
    allow_download: bool = True,
) -> dict[str, Any]:
    resolved_path = ensure_keras_weights(
        weights_path or DEFAULT_KERAS_WEIGHTS_PATH,
        allow_download=allow_download,
    )
    return load_keras_backbone(model, resolved_path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Download, checksum, and validate the official Keras MobileNetV2 "
            "alpha=0.35 ImageNet backbone without TensorFlow."
        )
    )
    parser.add_argument("--output", type=Path, default=DEFAULT_KERAS_WEIGHTS_PATH)
    parser.add_argument("--force", action="store_true", help="Download the official file again.")
    parser.add_argument(
        "--download-only",
        action="store_true",
        help="Only download/checksum the H5; skip conversion validation.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    weights_path = ensure_keras_weights(args.output, force_download=args.force)
    print(f"Verified official weights: {weights_path}")
    print(f"SHA-256: {KERAS_MOBILENET_V2_035_SHA256}")

    if not args.download_only:
        model = build_keras_compatible_mobilenet_v2(num_classes=2, width_mult=0.35)
        provenance = load_keras_backbone(model, weights_path)
        print(
            "Validated H5 -> PyTorch conversion: "
            f"{provenance['converted_dataset_count']} tensors mapped; classifier left newly initialized."
        )


if __name__ == "__main__":
    main()
