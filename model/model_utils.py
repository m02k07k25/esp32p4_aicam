from __future__ import annotations

import hashlib
import json
from pathlib import Path

IMAGE_SIZE = 224
NUM_CLASSES = 2
MODEL_WIDTH_MULT = 0.35
MODEL_ARCHITECTURE = "mobilenet_v2_035_keras_tf_same"
PREPROCESS_PROFILE = "keras_mobilenet_v2_minus_one_to_one"
INPUT_MEAN = (0.5, 0.5, 0.5)
INPUT_STD = (0.5, 0.5, 0.5)
FIVE_CROP_AGGREGATION = "max_human_score_over_five_half_frame_crops"
# Field-test override for the current camera/model domain. The previous
# validation-derived threshold (0.97517770528793335) was too conservative for
# the live camera, where human scores around 0.8 were observed.
FIVE_CROP_HUMAN_THRESHOLD = 0.75
SUPPORTED_IMAGE_SUFFIXES = {".bmp", ".jpeg", ".jpg", ".png"}
TRAIN_AUGMENTATION_PROFILE = "exclusive_photometric_v1"
TRAIN_AUGMENTATION_CONFIG = {
    "horizontal_flip_probability": 0.5,
    "rotation_probability": 0.3,
    "rotation_degrees": 6.0,
    "scale_probability": 0.3,
    "scale_range": (0.90, 1.08),
    "photometric_probabilities": {
        "identity": 0.15,
        "color_jitter": 0.55,
        "grayscale": 0.12,
        "night": 0.18,
    },
    "color_jitter": {
        "brightness": 0.25,
        "contrast": 0.25,
        "saturation": 0.25,
    },
    "night": {
        "brightness_range": (0.35, 0.70),
        "contrast_range": (0.70, 1.05),
        "saturation_range": (0.40, 0.90),
        "gamma_range": (1.40, 2.20),
    },
}

MODEL_ROOT = Path(__file__).resolve().parent
PROJECT_ROOT = MODEL_ROOT.parent
DOWNLOAD_ROOT = PROJECT_ROOT / "download"
DATA_ROOT = MODEL_ROOT / "data"
TRAIN_ROOT = DATA_ROOT / "train"
VAL_ROOT = DATA_ROOT / "val"
TEST_ROOT = DATA_ROOT / "test"
CALIB_ROOT = DATA_ROOT / "calib"
SPLIT_MANIFEST_PATH = DATA_ROOT / "split_manifest.json"
FINETUNE_SOURCE_ROOT = DATA_ROOT / "finetune_train"
ARTIFACTS_ROOT = MODEL_ROOT / "artifacts"
CHECKPOINT_PATH = ARTIFACTS_ROOT / "checkpoints" / "best.pt"
ONNX_PATH = ARTIFACTS_ROOT / "onnx" / "classifier_224.onnx"
ESPDL_PATH = ARTIFACTS_ROOT / "espdl" / "classifier_224_p4.espdl"
TEST_PREDICTIONS_PATH = ARTIFACTS_ROOT / "evaluation" / "test_predictions.csv"
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


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checkpoint_split_provenance(
    checkpoint: dict[str, object],
    checkpoint_path: Path,
    *,
    allow_unverified: bool = False,
) -> dict[str, object]:
    """Return a reproducible split digest without hiding weak provenance.

    Training checkpoints produced by this repository contain the SHA-256 of
    ``model/data/split_manifest.json``.  A checkpoint supplied from outside
    the pipeline may only say ``manual`` (or omit the field entirely).  Such a
    checkpoint can still be exported deliberately, but its derived digest is
    marked unverified and is never silently treated as the repository split.
    """

    raw_split = checkpoint.get("dataset_split")
    if not isinstance(raw_split, dict):
        raw_split = {}

    reported_digest = raw_split.get("manifest_sha256")
    if (
        isinstance(reported_digest, str)
        and len(reported_digest) == 64
        and all(character in "0123456789abcdef" for character in reported_digest.lower())
    ):
        return {
            "digest": reported_digest.lower(),
            "verified": True,
            "reported": reported_digest,
            "description": "checkpoint references a content-addressed split manifest",
        }

    if not allow_unverified:
        raise ValueError(
            f"Checkpoint {checkpoint_path} does not contain a valid dataset split digest. "
            "Pass --allow-unverified-split only when the training split is unavailable."
        )

    canonical = json.dumps(
        raw_split,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("utf-8")
    derived_digest = hashlib.sha256(canonical).hexdigest()
    return {
        "digest": derived_digest,
        "verified": False,
        "reported": reported_digest,
        "description": (
            "checkpoint split provenance is unverified; digest is derived from "
            "the checkpoint's dataset_split metadata"
        ),
    }


def validate_onnx_metadata(onnx_path: Path) -> dict[str, str]:
    import onnx

    model = onnx.load(str(onnx_path))
    metadata = {entry.key: entry.value for entry in model.metadata_props}
    expected = {
        "architecture": MODEL_ARCHITECTURE,
        "width_mult": str(MODEL_WIDTH_MULT),
        "image_size": str(IMAGE_SIZE),
        "preprocess_profile": PREPROCESS_PROFILE,
        "input_mean": json.dumps(list(INPUT_MEAN)),
        "input_std": json.dumps(list(INPUT_STD)),
        "labels": json.dumps(load_labels()),
    }
    mismatches = {
        key: {"actual": metadata.get(key), "expected": value}
        for key, value in expected.items()
        if metadata.get(key) != value
    }
    if mismatches:
        raise ValueError(
            f"ONNX metadata is missing or incompatible: {mismatches}. "
            "Export it again with the current export_onnx.py."
        )

    for key in (
        "source_checkpoint_sha256",
        "dataset_split_manifest_sha256",
    ):
        value = metadata.get(key, "")
        if len(value) != 64 or any(
            character not in "0123456789abcdef"
            for character in value.lower()
        ):
            raise ValueError(
                f"ONNX metadata {key} is not a valid SHA-256 digest. "
                "Export it again with the current export_onnx.py."
            )
    return metadata


def collect_images(root: Path) -> list[Path]:
    if not root.exists():
        raise FileNotFoundError(f"Missing directory: {root}")
    images = [
        candidate
        for candidate in sorted(root.rglob("*"))
        if candidate.is_file() and candidate.suffix.lower() in SUPPORTED_IMAGE_SUFFIXES
    ]
    return images


class FixedOrderImageDataset:
    """Image dataset whose class indices always follow model/labels.txt."""

    def __init__(self, root: Path, ordered_labels: list[str], transform=None):
        self.root = Path(root)
        self.classes = list(ordered_labels)
        self.class_to_idx = {
            label: index
            for index, label in enumerate(self.classes)
        }
        self.transform = transform
        self.samples: list[tuple[Path, int]] = []

        validate_split_directories(self.root, self.classes)
        for label, class_index in self.class_to_idx.items():
            class_images = collect_images(self.root / label)
            if not class_images:
                raise RuntimeError(
                    f"Class {label!r} has no images under {self.root / label}."
                )
            self.samples.extend(
                (image_path, class_index)
                for image_path in class_images
            )

        self.targets = [target for _, target in self.samples]

    def __len__(self) -> int:
        return len(self.samples)

    def __getitem__(self, index: int):
        from PIL import Image

        image_path, target = self.samples[index]
        with Image.open(image_path) as image:
            sample = image.convert("RGB")
            if self.transform is not None:
                sample = self.transform(sample)
        return sample, target


def validate_split_directories(split_root: Path, labels: list[str]) -> None:
    missing = [label for label in labels if not (split_root / label).is_dir()]
    if missing:
        missing_dirs = ", ".join(str(split_root / label) for label in missing)
        raise FileNotFoundError(f"Missing class directories under {split_root}: {missing_dirs}")


def validate_finetune_split(
    split_root: Path,
    manifest_path: Path,
    labels: list[str],
) -> dict[str, object]:
    if not manifest_path.is_file():
        raise FileNotFoundError(f"Missing fine-tuning split manifest: {manifest_path}")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("format_version") != 2:
        raise ValueError(
            f"Unsupported split manifest format_version={manifest.get('format_version')!r}; "
            "regenerate it with prepare_finetune_split.py."
        )

    class_summaries = manifest.get("classes")
    if not isinstance(class_summaries, dict):
        raise ValueError("Split manifest classes must be an object.")
    manifest_labels = manifest.get("labels", list(class_summaries))
    if manifest_labels != labels or set(class_summaries) != set(labels):
        raise ValueError(
            f"Split manifest labels {manifest_labels} do not match {labels}."
        )

    valid_splits = ("train", "val", "test")
    ratios = manifest.get("ratios")
    if (
        not isinstance(ratios, dict)
        or set(ratios) != set(valid_splits)
        or any(
            not isinstance(ratios[split], (int, float))
            or ratios[split] <= 0.0
            for split in valid_splits
        )
        or abs(sum(ratios.values()) - 1.0) > 1e-9
    ):
        raise ValueError(f"Invalid train/val/test ratios in split manifest: {ratios}")
    for split in valid_splits:
        validate_split_directories(split_root / split, labels)

    entries = manifest.get("images")
    if not isinstance(entries, list):
        raise ValueError("Split manifest images must be a list.")

    counts = {
        label: {split: 0 for split in valid_splits}
        for label in labels
    }
    group_splits: dict[tuple[str, str], set[str]] = {}
    digest_splits: dict[str, set[str]] = {}
    expected_files: set[Path] = set()
    split_root_resolved = split_root.resolve()

    for entry in entries:
        if not isinstance(entry, dict):
            raise ValueError("Every split manifest image entry must be an object.")
        label = entry.get("class")
        split = entry.get("split")
        group = entry.get("group")
        digest = entry.get("sha256")
        source_name = entry.get("source")
        if (
            label not in labels
            or split not in valid_splits
            or not isinstance(group, str)
            or not isinstance(digest, str)
            or not isinstance(source_name, str)
        ):
            raise ValueError(f"Invalid split manifest image entry: {entry}")

        source_relative = Path(source_name)
        if (
            source_relative.is_absolute()
            or ".." in source_relative.parts
            or not source_relative.parts
            or source_relative.parts[0] != label
        ):
            raise ValueError(f"Unsafe split manifest source path: {source_name}")

        destination = (split_root / split / source_relative).resolve()
        if split_root_resolved not in destination.parents:
            raise ValueError(f"Split image escapes the split root: {destination}")
        if not destination.is_file():
            raise FileNotFoundError(
                f"Split manifest references a missing image: {destination}"
            )
        if file_sha256(destination) != digest:
            raise ValueError(f"Split image checksum mismatch: {destination}")
        if destination in expected_files:
            raise ValueError(
                f"Duplicate split manifest destination: {destination}"
            )

        counts[label][split] += 1
        group_splits.setdefault((label, group), set()).add(split)
        digest_splits.setdefault(digest, set()).add(split)
        expected_files.add(destination)

    leaking_groups = {
        key: splits
        for key, splits in group_splits.items()
        if len(splits) != 1
    }
    leaking_digests = {
        digest: splits
        for digest, splits in digest_splits.items()
        if len(splits) != 1
    }
    if leaking_groups or leaking_digests:
        raise ValueError(
            "Fine-tuning split leakage detected: "
            f"groups={leaking_groups}, sha256={leaking_digests}"
        )

    actual_files = {
        path.resolve()
        for split in valid_splits
        for path in collect_images(split_root / split)
    }
    if actual_files != expected_files:
        raise ValueError(
            "Fine-tuning split files do not match split_manifest.json. "
            "Regenerate the split before training or evaluation."
        )

    for label in labels:
        summary = class_summaries[label]
        if not isinstance(summary, dict):
            raise ValueError(f"Split manifest summary for {label} must be an object.")
        for split in valid_splits:
            if counts[label][split] == 0:
                raise ValueError(f"Split {split}/{label} must not be empty.")
            if summary.get(split) != counts[label][split]:
                raise ValueError(
                    f"Split manifest count mismatch for {split}/{label}."
                )
        if summary.get("unique") != sum(counts[label].values()):
            raise ValueError(f"Split manifest unique count mismatch for {label}.")
        unique_count = summary["unique"]
        actual_ratios = summary.get("actual_ratios")
        if not isinstance(actual_ratios, dict) or any(
            not isinstance(actual_ratios.get(split), (int, float))
            or abs(
                actual_ratios[split]
                - counts[label][split] / unique_count
            )
            > 1e-12
            for split in valid_splits
        ):
            raise ValueError(f"Split manifest actual_ratios mismatch for {label}.")
        requested_counts = summary.get("requested_counts")
        if (
            not isinstance(requested_counts, dict)
            or set(requested_counts) != set(valid_splits)
            or any(
                not isinstance(requested_counts[split], int)
                for split in valid_splits
            )
            or sum(requested_counts.values()) != unique_count
        ):
            raise ValueError(f"Split manifest requested_counts mismatch for {label}.")

    return {
        "format_version": manifest["format_version"],
        "manifest_filename": manifest_path.name,
        "manifest_sha256": file_sha256(manifest_path),
        "ratios": manifest.get("ratios"),
        "counts": counts,
    }


def validate_calibration_set(
    data_root: Path,
    manifest_path: Path,
    labels: list[str],
) -> dict[str, int]:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    calibration = manifest.get("calibration")
    if not isinstance(calibration, dict):
        raise ValueError(
            "Split manifest has no calibration provenance; regenerate the dataset."
        )
    if calibration.get("source_split") != "train":
        raise ValueError("Calibration images must be copied only from train.")
    requested_per_class = calibration.get("requested_per_class")
    images_by_label = calibration.get("images")
    if (
        not isinstance(requested_per_class, int)
        or requested_per_class <= 0
        or not isinstance(images_by_label, dict)
        or set(images_by_label) != set(labels)
    ):
        raise ValueError("Invalid calibration section in split manifest.")

    expected_files: set[Path] = set()
    counts: dict[str, int] = {}
    data_root_resolved = data_root.resolve()
    for label in labels:
        entries = images_by_label[label]
        if not isinstance(entries, list) or len(entries) != requested_per_class:
            raise ValueError(
                f"Calibration manifest count mismatch for {label}."
            )
        seen_sources: set[str] = set()
        for entry in entries:
            if not isinstance(entry, dict):
                raise ValueError("Every calibration entry must be an object.")
            source_name = entry.get("source")
            digest = entry.get("sha256")
            if not isinstance(source_name, str) or not isinstance(digest, str):
                raise ValueError(f"Invalid calibration entry: {entry}")
            source_relative = Path(source_name)
            if (
                source_relative.is_absolute()
                or ".." in source_relative.parts
                or len(source_relative.parts) < 3
                or source_relative.parts[0] != "train"
                or source_relative.parts[1] != label
                or source_name in seen_sources
            ):
                raise ValueError(
                    f"Unsafe or duplicate calibration source: {source_name}"
                )
            seen_sources.add(source_name)

            training_path = (data_root / source_relative).resolve()
            calibration_relative = Path(
                "calib",
                *source_relative.parts[1:],
            )
            calibration_path = (data_root / calibration_relative).resolve()
            if (
                data_root_resolved not in training_path.parents
                or data_root_resolved not in calibration_path.parents
            ):
                raise ValueError(
                    f"Calibration entry escapes data root: {source_name}"
                )
            for image_path in (training_path, calibration_path):
                if not image_path.is_file():
                    raise FileNotFoundError(
                        f"Missing calibration provenance image: {image_path}"
                    )
                if file_sha256(image_path) != digest:
                    raise ValueError(
                        f"Calibration image checksum mismatch: {image_path}"
                    )
            expected_files.add(calibration_path)
        counts[label] = len(entries)

    actual_files = {
        path.resolve()
        for path in collect_images(data_root / "calib")
    }
    if actual_files != expected_files:
        raise ValueError(
            "Calibration files do not match split_manifest.json."
        )
    return counts


class EspDlNearestResize:
    """Resize PIL images with the same floor-index mapping used by ESP-DL."""

    def __init__(self, size: tuple[int, int]):
        self.height, self.width = size

    def __call__(self, image):
        import numpy as np
        from PIL import Image

        if not isinstance(image, Image.Image):
            raise TypeError(
                "EspDlNearestResize expects a PIL image, "
                f"but received {type(image).__name__}."
            )

        source_width, source_height = image.size
        if source_width == self.width and source_height == self.height:
            return image.copy()

        source = np.asarray(image)
        x_indices = (
            np.arange(self.width, dtype=np.int64) * source_width
        ) // self.width
        y_indices = (
            np.arange(self.height, dtype=np.int64) * source_height
        ) // self.height
        resized = source[y_indices[:, None], x_indices[None, :]]
        return Image.fromarray(np.ascontiguousarray(resized))

    def __repr__(self) -> str:
        return f"{self.__class__.__name__}(size=({self.height}, {self.width}))"


class NightAugment:
    """Create a low-light image without overlapping other color modes."""

    def __init__(
        self,
        *,
        brightness_range: tuple[float, float],
        contrast_range: tuple[float, float],
        saturation_range: tuple[float, float],
        gamma_range: tuple[float, float],
    ):
        self.brightness_range = brightness_range
        self.contrast_range = contrast_range
        self.saturation_range = saturation_range
        self.gamma_range = gamma_range

    @staticmethod
    def _sample(bounds: tuple[float, float]) -> float:
        import torch

        return torch.empty(1).uniform_(*bounds).item()

    def __call__(self, image):
        from torchvision.transforms import functional

        image = functional.adjust_saturation(
            image,
            self._sample(self.saturation_range),
        )
        image = functional.adjust_contrast(
            image,
            self._sample(self.contrast_range),
        )
        image = functional.adjust_gamma(
            image,
            self._sample(self.gamma_range),
        )
        return functional.adjust_brightness(
            image,
            self._sample(self.brightness_range),
        )


class ExclusivePhotometricAugment:
    """Choose exactly one of identity, color jitter, grayscale, or night."""

    MODES = ("identity", "color_jitter", "grayscale", "night")

    def __init__(
        self,
        probabilities: dict[str, float],
        color_jitter: dict[str, float],
        night: dict[str, tuple[float, float]],
    ):
        from torchvision import transforms

        if set(probabilities) != set(self.MODES):
            raise ValueError(
                f"Photometric probabilities must define {self.MODES}."
            )
        if any(probabilities[mode] < 0.0 for mode in self.MODES):
            raise ValueError("Photometric probabilities cannot be negative.")
        if abs(sum(probabilities.values()) - 1.0) > 1e-9:
            raise ValueError("Photometric probabilities must add up to 1.0.")

        self.probabilities = tuple(
            probabilities[mode]
            for mode in self.MODES
        )
        self.color_jitter = transforms.ColorJitter(**color_jitter)
        self.night = NightAugment(**night)

    def sample_mode(self) -> str:
        import torch

        draw = torch.rand(1).item()
        cumulative = 0.0
        for mode, probability in zip(self.MODES, self.probabilities):
            cumulative += probability
            if draw < cumulative:
                return mode
        return self.MODES[-1]

    def __call__(self, image):
        from torchvision.transforms import functional

        mode = self.sample_mode()
        if mode == "color_jitter":
            return self.color_jitter(image)
        if mode == "grayscale":
            return functional.rgb_to_grayscale(image, num_output_channels=3)
        if mode == "night":
            return self.night(image)
        return image


def make_five_crops(image):
    """Return the same four corner crops plus center crop used by firmware."""
    width, height = image.size
    half_width = width // 2
    half_height = height // 2
    center_left = (width - half_width) // 2
    center_top = (height - half_height) // 2
    boxes = (
        ("top_left", (0, 0, half_width, half_height)),
        ("top_right", (width - half_width, 0, width, half_height)),
        ("bottom_left", (0, height - half_height, half_width, height)),
        (
            "bottom_right",
            (width - half_width, height - half_height, width, height),
        ),
        (
            "center",
            (
                center_left,
                center_top,
                center_left + half_width,
                center_top + half_height,
            ),
        ),
    )
    return [(name, image.crop(box)) for name, box in boxes]


def build_transforms(train: bool):
    from torchvision import transforms
    from torchvision.transforms import InterpolationMode

    ops = []
    if train:
        ops.extend(
            [
                transforms.RandomHorizontalFlip(
                    p=TRAIN_AUGMENTATION_CONFIG[
                        "horizontal_flip_probability"
                    ]
                ),
                transforms.RandomApply(
                    [
                        transforms.RandomRotation(
                            degrees=TRAIN_AUGMENTATION_CONFIG[
                                "rotation_degrees"
                            ],
                            interpolation=InterpolationMode.BILINEAR,
                            fill=0,
                        )
                    ],
                    p=TRAIN_AUGMENTATION_CONFIG["rotation_probability"],
                ),
                transforms.RandomApply(
                    [
                        transforms.RandomAffine(
                            degrees=0,
                            scale=TRAIN_AUGMENTATION_CONFIG["scale_range"],
                            interpolation=InterpolationMode.BILINEAR,
                            fill=0,
                        )
                    ],
                    p=TRAIN_AUGMENTATION_CONFIG["scale_probability"],
                ),
                ExclusivePhotometricAugment(
                    TRAIN_AUGMENTATION_CONFIG[
                        "photometric_probabilities"
                    ],
                    TRAIN_AUGMENTATION_CONFIG["color_jitter"],
                    TRAIN_AUGMENTATION_CONFIG["night"],
                ),
            ]
        )
    ops.extend(
        [
            EspDlNearestResize((IMAGE_SIZE, IMAGE_SIZE)),
            transforms.ToTensor(),
            transforms.Normalize(mean=INPUT_MEAN, std=INPUT_STD),
        ]
    )
    return transforms.Compose(ops)


def build_model(
    num_classes: int = NUM_CLASSES,
    pretrained: bool = True,
    width_mult: float = MODEL_WIDTH_MULT,
    pretrained_weights_path: Path | None = None,
    allow_download: bool = True,
):
    from keras_mobilenetv2 import (
        build_keras_compatible_mobilenet_v2,
        initialize_keras_pretrained_backbone,
    )

    model = build_keras_compatible_mobilenet_v2(
        num_classes=num_classes,
        width_mult=width_mult,
    )
    if pretrained:
        initialize_keras_pretrained_backbone(
            model,
            weights_path=pretrained_weights_path,
            allow_download=allow_download,
        )
    return model


def checkpoint_state_dict(checkpoint: dict) -> dict:
    if "model_state_dict" in checkpoint:
        return checkpoint["model_state_dict"]
    return checkpoint


def validate_checkpoint_compatibility(checkpoint: dict, checkpoint_path: Path | None = None) -> None:
    location = f" {checkpoint_path}" if checkpoint_path is not None else ""

    checkpoint_width = checkpoint.get("width_mult")
    if checkpoint_width is None or float(checkpoint_width) != MODEL_WIDTH_MULT:
        raise ValueError(
            f"Checkpoint{location} width_mult={checkpoint_width!r} does not match "
            f"the required width_mult={MODEL_WIDTH_MULT}."
        )

    architecture = checkpoint.get("architecture")
    if architecture != MODEL_ARCHITECTURE:
        raise ValueError(
            f"Checkpoint{location} architecture={architecture!r} does not match "
            f"{MODEL_ARCHITECTURE!r}. Retrain with the Keras-compatible 0.35 model."
        )

    image_size = checkpoint.get("image_size")
    if image_size != IMAGE_SIZE:
        raise ValueError(
            f"Checkpoint{location} image_size={image_size!r} does not match {IMAGE_SIZE}."
        )

    preprocess_profile = checkpoint.get("preprocess_profile")
    if preprocess_profile != PREPROCESS_PROFILE:
        raise ValueError(
            f"Checkpoint{location} preprocess_profile={preprocess_profile!r} does not match "
            f"{PREPROCESS_PROFILE!r}."
        )

    normalization = checkpoint.get("normalization", {})
    if normalization.get("mean") != list(INPUT_MEAN) or normalization.get("std") != list(INPUT_STD):
        raise ValueError(
            f"Checkpoint{location} normalization={normalization!r} does not match "
            f"mean={list(INPUT_MEAN)}, std={list(INPUT_STD)}."
        )
