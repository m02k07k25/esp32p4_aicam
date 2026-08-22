from __future__ import annotations

import argparse
import csv
from pathlib import Path

from model_utils import (
    CHECKPOINT_PATH,
    FIVE_CROP_HUMAN_THRESHOLD,
    NUM_CLASSES,
    SPLIT_MANIFEST_PATH,
    TEST_PREDICTIONS_PATH,
    TEST_ROOT,
    FixedOrderImageDataset,
    build_model,
    build_transforms,
    checkpoint_state_dict,
    ensure_parent_dir,
    load_labels,
    make_five_crops,
    validate_checkpoint_compatibility,
    validate_finetune_split,
    validate_onnx_metadata,
    validate_split_directories,
)


class FiveCropTransform:
    def __init__(self, base_transform):
        self.base_transform = base_transform

    def __call__(self, image):
        import torch

        return torch.stack(
            [
                self.base_transform(crop)
                for _, crop in make_five_crops(image)
            ]
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Evaluate a compatible checkpoint on the untouched test split."
    )
    parser.add_argument("--test-dir", type=Path, default=TEST_ROOT)
    parser.add_argument(
        "--split-manifest",
        type=Path,
        default=SPLIT_MANIFEST_PATH,
        help="Manifest recorded in the checkpoint when it was trained.",
    )
    parser.add_argument("--checkpoint", type=Path, default=CHECKPOINT_PATH)
    parser.add_argument(
        "--onnx",
        type=Path,
        default=None,
        help="Evaluate this fixed-batch ONNX model instead of the PyTorch checkpoint.",
    )
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--workers", type=int, default=0)
    parser.add_argument(
        "--five-crop",
        action="store_true",
        help="Evaluate the firmware's max-human-score aggregation over five half-frame crops.",
    )
    parser.add_argument(
        "--human-threshold",
        type=float,
        default=FIVE_CROP_HUMAN_THRESHOLD,
        help=(
            "Human-score decision threshold for --five-crop. The default was "
            "selected on val only."
        ),
    )
    parser.add_argument(
        "--device",
        default="auto",
        help='"auto", "cpu", or a torch device string.',
    )
    parser.add_argument(
        "--predictions-csv",
        type=Path,
        default=TEST_PREDICTIONS_PATH,
        help="Per-image prediction output.",
    )
    parser.add_argument(
        "--allow-unverified-split",
        action="store_true",
        help=(
            "Evaluate a checkpoint/ONNX model whose training split provenance is "
            "unverified, with an explicit warning."
        ),
    )
    return parser.parse_args()


def resolve_device(device_arg: str):
    import torch

    if device_arg == "auto":
        return torch.device("cuda" if torch.cuda.is_available() else "cpu")
    return torch.device(device_arg)


def main() -> None:
    args = parse_args()

    import torch
    from torch import nn
    from torch.utils.data import DataLoader

    if args.batch_size < 1:
        raise ValueError("--batch-size must be at least 1.")
    if args.workers < 0:
        raise ValueError("--workers cannot be negative.")
    if not 0.0 <= args.human_threshold <= 1.0:
        raise ValueError("--human-threshold must be between 0 and 1.")

    labels = load_labels()
    expected_class_to_idx = {
        label: index
        for index, label in enumerate(labels)
    }
    split_root = args.split_manifest.parent
    expected_test_dir = (split_root / "test").resolve()
    if args.test_dir.resolve() != expected_test_dir:
        raise ValueError(
            f"--test-dir must be {expected_test_dir} for "
            f"--split-manifest {args.split_manifest}."
        )
    dataset_split = validate_finetune_split(
        split_root,
        args.split_manifest,
        labels,
    )
    validate_split_directories(args.test_dir, labels)

    checkpoint = None
    onnx_session = None
    onnx_input_name = None
    if args.onnx is None:
        checkpoint = torch.load(
            args.checkpoint,
            map_location="cpu",
            weights_only=True,
        )
        checkpoint_labels = checkpoint.get("labels")
        if checkpoint_labels != labels:
            raise ValueError(
                f"Checkpoint labels {checkpoint_labels} do not match {labels}."
            )
        checkpoint_class_to_idx = checkpoint.get("class_to_idx")
        if checkpoint_class_to_idx != expected_class_to_idx:
            raise ValueError(
                f"Checkpoint class_to_idx={checkpoint_class_to_idx} does not match "
                f"{expected_class_to_idx}."
            )
        validate_checkpoint_compatibility(checkpoint, args.checkpoint)
        checkpoint_dataset_split = checkpoint.get("dataset_split")
        checkpoint_digest = (
            checkpoint_dataset_split.get("manifest_sha256")
            if isinstance(checkpoint_dataset_split, dict)
            else None
        )
        if checkpoint_digest != dataset_split["manifest_sha256"]:
            if not args.allow_unverified_split:
                raise ValueError(
                    "Checkpoint and test split manifest do not match. Evaluate only on "
                    "the untouched test split that was recorded during training, or "
                    "pass --allow-unverified-split for an explicitly unverified checkpoint."
                )
            print(
                "WARNING: checkpoint split provenance is unverified; this test result "
                "is diagnostic and is not an official training-split comparison."
            )
    else:
        if args.batch_size != 1:
            raise ValueError(
                "The exported ONNX model has a fixed batch size of 1; "
                "use --batch-size 1."
            )
        metadata = validate_onnx_metadata(args.onnx)
        if metadata["dataset_split_manifest_sha256"] != dataset_split["manifest_sha256"]:
            if not args.allow_unverified_split or metadata.get(
                "dataset_split_verified", "true"
            ) != "false":
                raise ValueError("ONNX model and test split manifest do not match.")
            print(
                "WARNING: ONNX checkpoint split provenance is unverified; this test "
                "result is diagnostic only."
            )
        import onnxruntime as ort

        onnx_session = ort.InferenceSession(
            str(args.onnx),
            providers=["CPUExecutionProvider"],
        )
        onnx_input_name = onnx_session.get_inputs()[0].name

    base_transform = build_transforms(train=False)
    dataset = FixedOrderImageDataset(
        args.test_dir,
        labels,
        transform=(
            FiveCropTransform(base_transform)
            if args.five_crop
            else base_transform
        ),
    )
    device = (
        torch.device("cpu")
        if onnx_session is not None
        else resolve_device(args.device)
    )
    dataloader = DataLoader(
        dataset,
        batch_size=args.batch_size,
        shuffle=False,
        num_workers=args.workers,
        pin_memory=device.type == "cuda",
    )

    model = None
    if checkpoint is not None:
        model = build_model(num_classes=NUM_CLASSES, pretrained=False)
        model.load_state_dict(checkpoint_state_dict(checkpoint))
        model.to(device)
        model.eval()

    criterion = nn.CrossEntropyLoss(reduction="sum")
    confusion = torch.zeros((NUM_CLASSES, NUM_CLASSES), dtype=torch.int64)
    total_loss = 0.0
    total_correct = 0
    total_samples = 0
    prediction_rows: list[dict[str, object]] = []
    human_index = labels.index("human")
    no_human_index = labels.index("no_human")
    crop_names = (
        "top_left",
        "top_right",
        "bottom_left",
        "bottom_right",
        "center",
    )

    with torch.inference_mode():
        for images, targets in dataloader:
            batch_start = total_samples
            images = images.to(device)
            targets = targets.to(device)

            if args.five_crop:
                batch_size, crop_count, channels, height, width = images.shape
                flat_images = images.reshape(
                    batch_size * crop_count,
                    channels,
                    height,
                    width,
                )
            else:
                batch_size = targets.size(0)
                crop_count = 1
                flat_images = images

            if onnx_session is not None:
                assert onnx_input_name is not None
                flat_logits = torch.cat(
                    [
                        torch.from_numpy(
                            onnx_session.run(
                                None,
                                {onnx_input_name: image.unsqueeze(0).numpy()},
                            )[0]
                        )
                        for image in flat_images
                    ],
                    dim=0,
                )
            else:
                assert model is not None
                flat_logits = model(flat_images)

            if args.five_crop:
                crop_logits = flat_logits.reshape(
                    batch_size,
                    crop_count,
                    NUM_CLASSES,
                )
                crop_probabilities = torch.softmax(crop_logits, dim=2)
                selected_crops = crop_probabilities[
                    :, :, human_index
                ].argmax(dim=1)
                logits = crop_logits[
                    torch.arange(batch_size, device=selected_crops.device),
                    selected_crops,
                ]
                selected_crops_cpu = selected_crops.detach().cpu()
            else:
                logits = flat_logits
                selected_crops_cpu = None
            probabilities = torch.softmax(logits, dim=1)
            if args.five_crop:
                predictions = torch.where(
                    probabilities[:, human_index] >= args.human_threshold,
                    torch.full_like(targets, human_index),
                    torch.full_like(targets, no_human_index),
                )
            else:
                predictions = probabilities.argmax(dim=1)

            total_loss += criterion(logits, targets).item()
            total_correct += (predictions == targets).sum().item()
            total_samples += batch_size

            flat_indices = (
                targets.detach().cpu() * NUM_CLASSES
                + predictions.detach().cpu()
            )
            confusion += torch.bincount(
                flat_indices,
                minlength=NUM_CLASSES * NUM_CLASSES,
            ).reshape(NUM_CLASSES, NUM_CLASSES)

            probabilities_cpu = probabilities.detach().cpu()
            predictions_cpu = predictions.detach().cpu()
            targets_cpu = targets.detach().cpu()
            for batch_index in range(batch_size):
                image_path, dataset_target = dataset.samples[
                    batch_start + batch_index
                ]
                target_index = targets_cpu[batch_index].item()
                if dataset_target != target_index:
                    raise RuntimeError("DataLoader sample order changed unexpectedly.")
                predicted_index = predictions_cpu[batch_index].item()
                row: dict[str, object] = {
                    "path": image_path.relative_to(args.test_dir).as_posix(),
                    "true_label": labels[target_index],
                    "predicted_label": labels[predicted_index],
                    "correct": predicted_index == target_index,
                    "selected_crop": (
                        crop_names[selected_crops_cpu[batch_index].item()]
                        if selected_crops_cpu is not None
                        else "full"
                    ),
                    "human_threshold": (
                        args.human_threshold if args.five_crop else ""
                    ),
                }
                for label_index, label in enumerate(labels):
                    row[f"prob_{label}"] = float(
                        probabilities_cpu[batch_index, label_index]
                    )
                prediction_rows.append(row)

    if total_samples == 0:
        raise RuntimeError(f"No test images found under {args.test_dir}.")

    average_loss = total_loss / total_samples
    accuracy = total_correct / total_samples
    class_recalls = [
        confusion[class_index, class_index].item()
        / confusion[class_index].sum().item()
        for class_index in range(NUM_CLASSES)
    ]
    balanced_accuracy = sum(class_recalls) / NUM_CLASSES

    ensure_parent_dir(args.predictions_csv)
    fieldnames = [
        "path",
        "true_label",
        "predicted_label",
        "correct",
        "selected_crop",
        "human_threshold",
        *(f"prob_{label}" for label in labels),
    ]
    with args.predictions_csv.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(prediction_rows)

    print(
        f"{'ONNX' if onnx_session is not None else 'PyTorch'} "
        f"{'5-crop' if args.five_crop else 'single-image'} test "
        f"samples={total_samples} loss={average_loss:.4f} "
        f"accuracy={accuracy:.4f} ({total_correct}/{total_samples}) "
        f"balanced_accuracy={balanced_accuracy:.4f}"
    )
    if args.five_crop:
        print(f"Five-crop human threshold={args.human_threshold:.8f} (selected on val)")
    print("Confusion matrix (rows=true, columns=predicted):")
    print(" " * 14 + " ".join(f"{label:>10s}" for label in labels))
    for class_index, label in enumerate(labels):
        values = " ".join(
            f"{confusion[class_index, predicted_index].item():10d}"
            for predicted_index in range(NUM_CLASSES)
        )
        print(f"{label:>12s}: {values}")

    print("Per-class metrics:")
    for class_index, label in enumerate(labels):
        support = confusion[class_index].sum().item()
        predicted_count = confusion[:, class_index].sum().item()
        correct = confusion[class_index, class_index].item()
        recall = class_recalls[class_index]
        precision = correct / predicted_count if predicted_count else 0.0
        f1_score = (
            2.0 * precision * recall / (precision + recall)
            if precision + recall
            else 0.0
        )
        print(
            f"  {label}: precision={precision:.4f} recall={recall:.4f} "
            f"f1={f1_score:.4f} support={support}"
        )
    print(f"Per-image predictions: {args.predictions_csv}")


if __name__ == "__main__":
    main()
