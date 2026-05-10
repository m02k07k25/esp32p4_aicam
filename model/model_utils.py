from __future__ import annotations

from pathlib import Path

IMAGE_SIZE = 224
NUM_CLASSES = 2
MODEL_WIDTH_MULT = 0.35
IMAGENET_MEAN = (0.485, 0.456, 0.406)
IMAGENET_STD = (0.229, 0.224, 0.225)
SUPPORTED_IMAGE_SUFFIXES = {".bmp", ".jpeg", ".jpg", ".png"}

MODEL_ROOT = Path(__file__).resolve().parent
PROJECT_ROOT = MODEL_ROOT.parent
DOWNLOAD_ROOT = PROJECT_ROOT / "download"
DATA_ROOT = MODEL_ROOT / "data"
TRAIN_ROOT = DATA_ROOT / "train"
VAL_ROOT = DATA_ROOT / "val"
CALIB_ROOT = DATA_ROOT / "calib"
ARTIFACTS_ROOT = MODEL_ROOT / "artifacts"
CHECKPOINT_PATH = ARTIFACTS_ROOT / "checkpoints" / "best.pt"
ONNX_PATH = ARTIFACTS_ROOT / "onnx" / "classifier_224.onnx"
ESPDL_PATH = ARTIFACTS_ROOT / "espdl" / "classifier_224_p4.espdl"
LABELS_PATH = MODEL_ROOT / "labels.txt"


def load_labels(labels_path: Path = LABELS_PATH) -> list[str]:
    labels = [line.strip() for line in labels_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    if len(labels) != NUM_CLASSES:
        raise ValueError(f"{labels_path} must contain exactly {NUM_CLASSES} labels, found {len(labels)}.")
    if len(set(labels)) != NUM_CLASSES:
        raise ValueError(f"{labels_path} must contain unique labels.")
    return labels


def ensure_parent_dir(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def collect_images(root: Path) -> list[Path]:
    if not root.exists():
        raise FileNotFoundError(f"Missing directory: {root}")
    images = [
        candidate
        for candidate in sorted(root.rglob("*"))
        if candidate.is_file() and candidate.suffix.lower() in SUPPORTED_IMAGE_SUFFIXES
    ]
    return images


def validate_split_directories(split_root: Path, labels: list[str]) -> None:
    missing = [label for label in labels if not (split_root / label).is_dir()]
    if missing:
        missing_dirs = ", ".join(str(split_root / label) for label in missing)
        raise FileNotFoundError(f"Missing class directories under {split_root}: {missing_dirs}")


def build_transforms(train: bool):
    from torchvision import transforms

    ops = [transforms.Resize((IMAGE_SIZE, IMAGE_SIZE))]
    if train:
        ops.append(transforms.RandomHorizontalFlip(p=0.5))
    ops.extend(
        [
            transforms.ToTensor(),
            transforms.Normalize(mean=IMAGENET_MEAN, std=IMAGENET_STD),
        ]
    )
    return transforms.Compose(ops)


def build_model(num_classes: int = NUM_CLASSES, pretrained: bool = True, width_mult: float = MODEL_WIDTH_MULT):
    import math
    import warnings

    import torch.nn as nn
    import torchvision
    from torchvision.models import MobileNet_V2_Weights

    weights = None
    if pretrained:
        if math.isclose(width_mult, 1.0):
            weights = MobileNet_V2_Weights.IMAGENET1K_V1
        else:
            warnings.warn(
                "torchvision MobileNetV2 pretrained weights are only compatible with width_mult=1.0; "
                f"training width_mult={width_mult} from random initialization.",
                RuntimeWarning,
                stacklevel=2,
            )
    model = torchvision.models.mobilenet_v2(weights=weights, width_mult=width_mult)
    in_features = model.classifier[1].in_features
    model.classifier[1] = nn.Linear(in_features, num_classes)
    return model


def checkpoint_state_dict(checkpoint: dict) -> dict:
    if "model_state_dict" in checkpoint:
        return checkpoint["model_state_dict"]
    return checkpoint
