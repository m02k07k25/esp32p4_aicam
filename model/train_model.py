from __future__ import annotations

import argparse
import shutil
from pathlib import Path

from model_utils import (
    CHECKPOINT_PATH,
    FixedOrderImageDataset,
    IMAGE_SIZE,
    INPUT_MEAN,
    INPUT_STD,
    MODEL_ARCHITECTURE,
    MODEL_WIDTH_MULT,
    NUM_CLASSES,
    PREPROCESS_PROFILE,
    SPLIT_MANIFEST_PATH,
    TRAIN_AUGMENTATION_CONFIG,
    TRAIN_AUGMENTATION_PROFILE,
    TRAIN_ROOT,
    VAL_ROOT,
    build_model,
    build_transforms,
    checkpoint_state_dict,
    ensure_parent_dir,
    load_labels,
    validate_checkpoint_compatibility,
    validate_finetune_split,
    validate_split_directories,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Train a 2-class MobileNetV2 0.35-width classifier for ESP-DL export."
    )
    parser.add_argument(
        "--train-dir",
        type=Path,
        default=TRAIN_ROOT,
        help="Training data root.",
    )
    parser.add_argument(
        "--val-dir",
        type=Path,
        default=VAL_ROOT,
        help="Validation data root.",
    )
    parser.add_argument(
        "--split-manifest",
        type=Path,
        default=SPLIT_MANIFEST_PATH,
        help="Manifest binding train/val/test files to this training run.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=CHECKPOINT_PATH,
        help="Best-checkpoint output path.",
    )
    parser.add_argument(
        "--epochs",
        type=int,
        default=10,
        help="Maximum number of epochs.",
    )
    parser.add_argument(
        "--batch-size",
        type=int,
        default=32,
        help="Batch size.",
    )
    parser.add_argument(
        "--lr",
        type=float,
        default=1e-4,
        help="Learning rate.",
    )
    parser.add_argument(
        "--weight-decay",
        type=float,
        default=1e-4,
        help="AdamW weight decay.",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=0,
        help="DataLoader worker processes.",
    )
    parser.add_argument(
        "--device",
        default="auto",
        help='"auto", "cpu", or a torch device string.',
    )
    parser.add_argument(
        "--freeze-features",
        action="store_true",
        help="Freeze the MobileNetV2 feature extractor.",
    )
    parser.add_argument(
        "--no-pretrained",
        action="store_true",
        help="Start from random weights instead of the official Keras ImageNet 0.35 backbone.",
    )
    parser.add_argument(
        "--pretrained-weights",
        type=Path,
        default=None,
        help="Optional path to the official Keras MobileNetV2 0.35 no-top H5 file.",
    )
    parser.add_argument(
        "--offline",
        action="store_true",
        help="Do not download pretrained weights; require an existing cached or explicit H5 file.",
    )
    parser.add_argument(
        "--init-checkpoint",
        type=Path,
        default=None,
        help="Load model weights only for a new fine-tuning run (new optimizer and epoch zero).",
    )
    parser.add_argument(
        "--resume",
        type=Path,
        default=None,
        help="Resume from an existing checkpoint.",
    )
    parser.add_argument(
        "--patience",
        type=int,
        default=5,
        help="Stop training after this many epochs without validation accuracy improvement.",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="Random seed.",
    )
    return parser.parse_args()


def resolve_device(device_arg: str):
    import torch

    if device_arg == "auto":
        return torch.device("cuda" if torch.cuda.is_available() else "cpu")
    return torch.device(device_arg)


def save_checkpoint(
    output_path: Path,
    model,
    optimizer,
    epoch: int,
    val_accuracy: float,
    val_loss: float,
    labels: list[str],
    class_to_idx: dict[str, int],
    pretrained_provenance: dict | None,
    freeze_features: bool,
    dataset_split: dict[str, object],
) -> None:
    import torch

    checkpoint = {
        "format_version": 2,
        "model_name": "mobilenet_v2",
        "architecture": MODEL_ARCHITECTURE,
        "width_mult": MODEL_WIDTH_MULT,
        "image_size": IMAGE_SIZE,
        "preprocess_profile": PREPROCESS_PROFILE,
        "labels": labels,
        "class_to_idx": class_to_idx,
        "normalization": {
            "mean": list(INPUT_MEAN),
            "std": list(INPUT_STD),
        },
        "pretrained": pretrained_provenance,
        "training_config": {
            "freeze_features": freeze_features,
            "augmentation_profile": TRAIN_AUGMENTATION_PROFILE,
            "augmentation": TRAIN_AUGMENTATION_CONFIG,
        },
        "dataset_split": dataset_split,
        "epoch": epoch,
        "val_accuracy": val_accuracy,
        "val_loss": val_loss,
        "model_state_dict": model.state_dict(),
        "optimizer_state_dict": optimizer.state_dict(),
    }

    ensure_parent_dir(output_path)
    torch.save(checkpoint, output_path)


def main() -> None:
    args = parse_args()

    import torch
    from torch import nn
    from torch.utils.data import DataLoader

    if args.patience < 1:
        raise ValueError("--patience must be at least 1.")
    if args.epochs < 1:
        raise ValueError("--epochs must be at least 1.")
    if args.resume is not None and args.init_checkpoint is not None:
        raise ValueError("--resume and --init-checkpoint are mutually exclusive.")
    if args.pretrained_weights is not None and (
        args.no_pretrained or args.resume is not None or args.init_checkpoint is not None
    ):
        raise ValueError(
            "--pretrained-weights cannot be combined with --no-pretrained, "
            "--resume, or --init-checkpoint."
        )

    labels = load_labels()
    split_root = args.split_manifest.parent
    expected_train_dir = (split_root / "train").resolve()
    expected_val_dir = (split_root / "val").resolve()
    if args.train_dir.resolve() != expected_train_dir:
        raise ValueError(
            f"--train-dir must be {expected_train_dir} for "
            f"--split-manifest {args.split_manifest}."
        )
    if args.val_dir.resolve() != expected_val_dir:
        raise ValueError(
            f"--val-dir must be {expected_val_dir} for "
            f"--split-manifest {args.split_manifest}."
        )
    dataset_split = validate_finetune_split(
        split_root,
        args.split_manifest,
        labels,
    )
    validate_split_directories(args.train_dir, labels)
    validate_split_directories(args.val_dir, labels)

    torch.manual_seed(args.seed)
    device = resolve_device(args.device)

    train_dataset = FixedOrderImageDataset(
        args.train_dir,
        labels,
        transform=build_transforms(train=True),
    )
    val_dataset = FixedOrderImageDataset(
        args.val_dir,
        labels,
        transform=build_transforms(train=False),
    )

    if len(train_dataset) == 0:
        raise RuntimeError(
            f"No training images found under {args.train_dir}"
        )

    if len(val_dataset) == 0:
        raise RuntimeError(
            f"No validation images found under {args.val_dir}"
        )

    train_loader = DataLoader(
        train_dataset,
        batch_size=args.batch_size,
        shuffle=True,
        num_workers=args.workers,
        pin_memory=device.type == "cuda",
    )
    val_loader = DataLoader(
        val_dataset,
        batch_size=args.batch_size,
        shuffle=False,
        num_workers=args.workers,
        pin_memory=device.type == "cuda",
    )

    use_keras_pretrained = (
        not args.no_pretrained
        and args.resume is None
        and args.init_checkpoint is None
    )
    model = build_model(
        num_classes=NUM_CLASSES,
        pretrained=use_keras_pretrained,
        pretrained_weights_path=args.pretrained_weights,
        allow_download=not args.offline,
    ).to(device)

    pretrained_provenance = getattr(model, "pretrained_provenance", None)
    loaded_checkpoint = None
    checkpoint_path = args.resume or args.init_checkpoint
    if checkpoint_path is not None:
        loaded_checkpoint = torch.load(
            checkpoint_path,
            map_location=device,
            weights_only=True,
        )
        checkpoint_labels = loaded_checkpoint.get("labels")
        if checkpoint_labels != labels:
            raise ValueError(
                f"Checkpoint labels {checkpoint_labels} do not match {labels}."
            )
        checkpoint_class_to_idx = loaded_checkpoint.get("class_to_idx")
        if checkpoint_class_to_idx != train_dataset.class_to_idx:
            raise ValueError(
                f"Checkpoint class_to_idx={checkpoint_class_to_idx} does not match "
                f"{train_dataset.class_to_idx}."
            )
        validate_checkpoint_compatibility(loaded_checkpoint, checkpoint_path)
        saved_dataset_split = loaded_checkpoint.get("dataset_split")
        if (
            not isinstance(saved_dataset_split, dict)
            or saved_dataset_split.get("manifest_sha256")
            != dataset_split["manifest_sha256"]
        ):
            operation = "resume" if args.resume is not None else "initialize"
            raise ValueError(
                f"Checkpoint {checkpoint_path} cannot {operation} this run because "
                "it was trained with a different or unrecorded dataset split. "
                "Reusing it could leak current test images into the model."
            )
        if args.resume is not None:
            training_config = loaded_checkpoint.get("training_config", {})
            saved_freeze_features = training_config.get("freeze_features")
            if not isinstance(saved_freeze_features, bool):
                raise ValueError(
                    f"Checkpoint {checkpoint_path} does not record freeze_features, "
                    "so it cannot be resumed safely. Use --init-checkpoint to load "
                    "its model weights into a new training run."
                )
            if saved_freeze_features != args.freeze_features:
                raise ValueError(
                    f"Checkpoint {checkpoint_path} was saved with "
                    f"freeze_features={saved_freeze_features}, but this run uses "
                    f"freeze_features={args.freeze_features}. Keep the same setting "
                    "for --resume, or use --init-checkpoint for a new optimizer."
                )
            saved_augmentation_profile = training_config.get(
                "augmentation_profile"
            )
            if saved_augmentation_profile != TRAIN_AUGMENTATION_PROFILE:
                raise ValueError(
                    f"Checkpoint {checkpoint_path} uses augmentation profile "
                    f"{saved_augmentation_profile!r}, but this run uses "
                    f"{TRAIN_AUGMENTATION_PROFILE!r}. Use --init-checkpoint "
                    "to start a new optimizer with the current augmentation."
                )
        model.load_state_dict(checkpoint_state_dict(loaded_checkpoint))
        pretrained_provenance = loaded_checkpoint.get("pretrained")

    if args.freeze_features:
        if not use_keras_pretrained and loaded_checkpoint is None:
            raise ValueError(
                "--freeze-features requires pretrained weights or a compatible checkpoint; "
                "refusing to freeze a random backbone."
            )
        for parameter in model.features.parameters():
            parameter.requires_grad = False

    criterion = nn.CrossEntropyLoss()

    optimizer = torch.optim.AdamW(
        (
            parameter
            for parameter in model.parameters()
            if parameter.requires_grad
        ),
        lr=args.lr,
        weight_decay=args.weight_decay,
    )

    start_epoch = 0
    best_val_accuracy = float("-inf")
    best_val_loss = float("inf")
    epochs_without_improvement = 0
    best_checkpoint_path = args.resume if args.resume is not None else args.output

    if loaded_checkpoint is not None:
        best_val_accuracy = float(
            loaded_checkpoint.get("val_accuracy", float("-inf"))
        )
        best_val_loss = float(
            loaded_checkpoint.get("val_loss", float("inf"))
        )
        best_checkpoint_path = checkpoint_path

    if (
        args.init_checkpoint is not None
        and args.init_checkpoint.resolve() != args.output.resolve()
    ):
        ensure_parent_dir(args.output)
        shutil.copy2(args.init_checkpoint, args.output)
        best_checkpoint_path = args.output
        print(
            f"Copied the validated initialization baseline to {args.output}."
        )

    if args.resume is not None:
        assert loaded_checkpoint is not None
        if "optimizer_state_dict" not in loaded_checkpoint:
            raise ValueError(
                f"Checkpoint {args.resume} has no optimizer_state_dict and cannot "
                "be resumed safely. Use --init-checkpoint for a new optimizer."
            )
        optimizer.load_state_dict(
            loaded_checkpoint["optimizer_state_dict"]
        )

        start_epoch = int(
            loaded_checkpoint.get("epoch", 0)
        )
    print(
        f"Training on {device} with "
        f"{len(train_dataset)} train images and "
        f"{len(val_dataset)} val images."
    )
    print(f"Training augmentation: {TRAIN_AUGMENTATION_PROFILE}")
    if args.resume is not None:
        print(f"Resumed model, optimizer, and epoch state from {args.resume}.")
    elif args.init_checkpoint is not None:
        print(f"Initialized model weights from {args.init_checkpoint}.")
    elif pretrained_provenance is not None:
        print(
            "Initialized the backbone from official Keras MobileNetV2 0.35 "
            f"weights (sha256={pretrained_provenance['sha256']})."
        )
    else:
        print("Initialized the model from random weights.")
    print(
        f"Early stopping patience: {args.patience}"
    )

    for epoch in range(start_epoch, args.epochs):
        model.train()
        if args.freeze_features:
            model.features.eval()

        train_loss = 0.0
        train_correct = 0
        train_count = 0

        for images, targets in train_loader:
            images = images.to(device)
            targets = targets.to(device)

            optimizer.zero_grad(set_to_none=True)

            logits = model(images)
            loss = criterion(logits, targets)

            loss.backward()
            optimizer.step()

            train_loss += loss.item() * images.size(0)

            predictions = logits.argmax(dim=1)
            train_correct += (
                predictions == targets
            ).sum().item()

            train_count += images.size(0)

        train_loss /= train_count
        train_accuracy = train_correct / train_count

        model.eval()

        val_loss = 0.0
        val_correct = 0
        val_count = 0

        with torch.no_grad():
            for images, targets in val_loader:
                images = images.to(device)
                targets = targets.to(device)

                logits = model(images)
                loss = criterion(logits, targets)

                val_loss += loss.item() * images.size(0)

                predictions = logits.argmax(dim=1)
                val_correct += (
                    predictions == targets
                ).sum().item()

                val_count += images.size(0)

        val_loss /= val_count
        val_accuracy = val_correct / val_count

        print(
            f"epoch {epoch + 1}/{args.epochs} "
            f"train_loss={train_loss:.4f} "
            f"train_acc={train_accuracy:.4f} "
            f"val_loss={val_loss:.4f} "
            f"val_acc={val_accuracy:.4f}"
        )

        validation_improved = (
            val_accuracy > best_val_accuracy
            or (
                val_accuracy == best_val_accuracy
                and val_loss < best_val_loss
            )
        )
        if validation_improved:
            best_val_accuracy = val_accuracy
            best_val_loss = val_loss
            epochs_without_improvement = 0

            save_checkpoint(
                args.output,
                model,
                optimizer,
                epoch + 1,
                val_accuracy,
                val_loss,
                labels,
                train_dataset.class_to_idx,
                pretrained_provenance,
                args.freeze_features,
                dataset_split,
            )
            best_checkpoint_path = args.output

            print(
                f"Saved best checkpoint to {args.output} "
                f"(val_acc={val_accuracy:.4f}, val_loss={val_loss:.4f})"
            )

        else:
            epochs_without_improvement += 1

            print(
                "No validation accuracy improvement: "
                f"{epochs_without_improvement}/"
                f"{args.patience}"
            )

            if (
                epochs_without_improvement
                >= args.patience
            ):
                print(
                    f"Early stopping at epoch {epoch + 1}. "
                    f"Best val_acc={best_val_accuracy:.4f}, "
                    f"val_loss={best_val_loss:.4f}"
                )
                break

    print(
        f"Training finished. "
        f"Best val_acc={best_val_accuracy:.4f}, "
        f"val_loss={best_val_loss:.4f}"
    )
    print(
        f"Best checkpoint: {best_checkpoint_path}"
    )


if __name__ == "__main__":
    main()
