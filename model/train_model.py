from __future__ import annotations

import argparse
from pathlib import Path

from model_utils import (
    CHECKPOINT_PATH,
    IMAGE_SIZE,
    IMAGENET_MEAN,
    IMAGENET_STD,
    NUM_CLASSES,
    TRAIN_ROOT,
    VAL_ROOT,
    build_model,
    build_transforms,
    checkpoint_state_dict,
    ensure_parent_dir,
    load_labels,
    validate_split_directories,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Train a 2-class MobileNetV2 classifier for ESP-DL export.")
    parser.add_argument("--train-dir", type=Path, default=TRAIN_ROOT, help="Training data root.")
    parser.add_argument("--val-dir", type=Path, default=VAL_ROOT, help="Validation data root.")
    parser.add_argument("--output", type=Path, default=CHECKPOINT_PATH, help="Best-checkpoint output path.")
    parser.add_argument("--epochs", type=int, default=10, help="Number of epochs.")
    parser.add_argument("--batch-size", type=int, default=32, help="Batch size.")
    parser.add_argument("--lr", type=float, default=1e-4, help="Learning rate.")
    parser.add_argument("--weight-decay", type=float, default=1e-4, help="AdamW weight decay.")
    parser.add_argument("--workers", type=int, default=0, help="DataLoader worker processes.")
    parser.add_argument("--device", default="auto", help='"auto", "cpu", or a torch device string.')
    parser.add_argument("--freeze-features", action="store_true", help="Freeze the MobileNetV2 feature extractor.")
    parser.add_argument("--no-pretrained", action="store_true", help="Start from random weights instead of ImageNet weights.")
    parser.add_argument("--resume", type=Path, default=None, help="Resume from an existing checkpoint.")
    parser.add_argument("--seed", type=int, default=42, help="Random seed.")
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
    labels: list[str],
    class_to_idx: dict[str, int],
) -> None:
    import torch

    checkpoint = {
        "model_name": "mobilenet_v2",
        "image_size": IMAGE_SIZE,
        "labels": labels,
        "class_to_idx": class_to_idx,
        "normalization": {"mean": list(IMAGENET_MEAN), "std": list(IMAGENET_STD)},
        "epoch": epoch,
        "val_accuracy": val_accuracy,
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
    from torchvision import datasets

    labels = load_labels()
    validate_split_directories(args.train_dir, labels)
    validate_split_directories(args.val_dir, labels)

    class FixedOrderImageFolder(datasets.ImageFolder):
        def __init__(self, root: Path, ordered_labels: list[str], **kwargs):
            self._ordered_labels = ordered_labels
            super().__init__(str(root), **kwargs)

        def find_classes(self, directory: str):
            classes = []
            for label in self._ordered_labels:
                label_dir = Path(directory) / label
                if not label_dir.is_dir():
                    raise FileNotFoundError(f"Missing class directory: {label_dir}")
                classes.append(label)
            return classes, {label: idx for idx, label in enumerate(classes)}

    torch.manual_seed(args.seed)
    device = resolve_device(args.device)

    train_dataset = FixedOrderImageFolder(args.train_dir, labels, transform=build_transforms(train=True))
    val_dataset = FixedOrderImageFolder(args.val_dir, labels, transform=build_transforms(train=False))

    if len(train_dataset) == 0:
        raise RuntimeError(f"No training images found under {args.train_dir}")
    if len(val_dataset) == 0:
        raise RuntimeError(f"No validation images found under {args.val_dir}")

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

    model = build_model(num_classes=NUM_CLASSES, pretrained=not args.no_pretrained).to(device)
    if args.freeze_features:
        for parameter in model.features.parameters():
            parameter.requires_grad = False

    criterion = nn.CrossEntropyLoss()
    optimizer = torch.optim.AdamW(
        (parameter for parameter in model.parameters() if parameter.requires_grad),
        lr=args.lr,
        weight_decay=args.weight_decay,
    )

    start_epoch = 0
    best_val_accuracy = 0.0
    if args.resume is not None:
        checkpoint = torch.load(args.resume, map_location=device)
        checkpoint_labels = checkpoint.get("labels", labels)
        if checkpoint_labels != labels:
            raise ValueError(f"Checkpoint labels {checkpoint_labels} do not match {labels}.")
        model.load_state_dict(checkpoint_state_dict(checkpoint))
        if "optimizer_state_dict" in checkpoint:
            optimizer.load_state_dict(checkpoint["optimizer_state_dict"])
        start_epoch = int(checkpoint.get("epoch", 0))
        best_val_accuracy = float(checkpoint.get("val_accuracy", 0.0))

    print(f"Training on {device} with {len(train_dataset)} train images and {len(val_dataset)} val images.")

    for epoch in range(start_epoch, args.epochs):
        model.train()
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
            train_correct += (predictions == targets).sum().item()
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
                val_correct += (predictions == targets).sum().item()
                val_count += images.size(0)

        val_loss /= val_count
        val_accuracy = val_correct / val_count

        print(
            f"epoch {epoch + 1}/{args.epochs} "
            f"train_loss={train_loss:.4f} train_acc={train_accuracy:.4f} "
            f"val_loss={val_loss:.4f} val_acc={val_accuracy:.4f}"
        )

        if val_accuracy >= best_val_accuracy:
            best_val_accuracy = val_accuracy
            save_checkpoint(args.output, model, optimizer, epoch + 1, val_accuracy, labels, train_dataset.class_to_idx)
            print(f"Saved best checkpoint to {args.output} (val_acc={val_accuracy:.4f})")


if __name__ == "__main__":
    main()
