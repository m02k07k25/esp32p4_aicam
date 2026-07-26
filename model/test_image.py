from __future__ import annotations

import argparse
from pathlib import Path

import torch
from PIL import Image

from model_utils import (
    CHECKPOINT_PATH,
    NUM_CLASSES,
    build_model,
    build_transforms,
    checkpoint_state_dict,
    load_labels,
    validate_checkpoint_compatibility,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Single image inference test")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument(
        "--checkpoint",
        type=Path,
        default=CHECKPOINT_PATH,
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    labels = load_labels()

    model = build_model(
        num_classes=NUM_CLASSES,
        pretrained=False,
    ).to(device)

    checkpoint = torch.load(
        args.checkpoint,
        map_location=device,
        weights_only=True,
    )
    checkpoint_labels = checkpoint.get("labels")
    if checkpoint_labels != labels:
        raise ValueError(f"Checkpoint labels {checkpoint_labels} do not match {labels}.")
    expected_class_to_idx = {
        label: index
        for index, label in enumerate(labels)
    }
    if checkpoint.get("class_to_idx") != expected_class_to_idx:
        raise ValueError(
            f"Checkpoint class_to_idx={checkpoint.get('class_to_idx')} does not "
            f"match {expected_class_to_idx}."
        )
    validate_checkpoint_compatibility(checkpoint, args.checkpoint)
    model.load_state_dict(checkpoint_state_dict(checkpoint))
    model.eval()

    transform = build_transforms(train=False)

    image = Image.open(args.image).convert("RGB")
    tensor = transform(image).unsqueeze(0).to(device)

    with torch.no_grad():
        logits = model(tensor)
        probabilities = torch.softmax(logits, dim=1)[0]

    predicted_index = probabilities.argmax().item()
    predicted_label = labels[predicted_index]

    print("=" * 50)

    for index, label in enumerate(labels):
        print(f"{label:10s}: {probabilities[index].item():.4f}")

    print("=" * 50)
    print(f"Prediction: {predicted_label}")


if __name__ == "__main__":
    main()
