from __future__ import annotations

import argparse
from pathlib import Path

import torch
from PIL import Image

from model_utils import (
    NUM_CLASSES,
    build_model,
    build_transforms,
    checkpoint_state_dict,
    load_labels,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Single image inference test")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument(
        "--checkpoint",
        type=Path,
        default=Path("artifacts/checkpoints/best.pt"),
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

    checkpoint = torch.load(args.checkpoint, map_location=device)
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