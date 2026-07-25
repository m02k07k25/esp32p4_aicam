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
)


def parse_args():
    parser = argparse.ArgumentParser(description="5-Crop inference test")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, default=CHECKPOINT_PATH)
    parser.add_argument(
        "--save-dir",
        type=Path,
        default=Path("crop_results"),
        help="Folder where cropped images will be saved.",
    )
    return parser.parse_args()


def make_five_crops(image: Image.Image):
    width, height = image.size

    half_width = width // 2
    half_height = height // 2

    center_left = (width - half_width) // 2
    center_top = (height - half_height) // 2

    return [
        ("top_left", image.crop((0, 0, half_width, half_height))),
        ("top_right", image.crop((width - half_width, 0, width, half_height))),
        ("bottom_left", image.crop((0, height - half_height, half_width, height))),
        (
            "bottom_right",
            image.crop(
                (
                    width - half_width,
                    height - half_height,
                    width,
                    height,
                )
            ),
        ),
        (
            "center",
            image.crop(
                (
                    center_left,
                    center_top,
                    center_left + half_width,
                    center_top + half_height,
                )
            ),
        ),
    ]


def main():
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
    crops = make_five_crops(image)

    human_idx = labels.index("human")
    no_human_idx = labels.index("no_human")

    best_score = -1.0
    best_region = ""

    # 이미지별 저장 폴더 생성
    image_save_dir = args.save_dir / args.image.stem
    image_save_dir.mkdir(parents=True, exist_ok=True)

    print("=" * 70)

    with torch.no_grad():
        for name, crop in crops:
            # 원본 crop 저장
            crop_path = image_save_dir / f"{name}.jpg"
            crop.save(crop_path, quality=95)

            tensor = transform(crop).unsqueeze(0).to(device)

            output = model(tensor)
            probabilities = torch.softmax(output, dim=1)[0]

            human_score = probabilities[human_idx].item()
            no_human_score = probabilities[no_human_idx].item()

            predicted_label = labels[probabilities.argmax().item()]

            print(
                f"{name:15s}"
                f" prediction={predicted_label:10s}"
                f" human={human_score:.4f}"
                f" no_human={no_human_score:.4f}"
                f" saved={crop_path}"
            )

            if human_score > best_score:
                best_score = human_score
                best_region = name

    print("=" * 70)
    print(f"Selected Region : {best_region}")
    print(f"Best Human Score: {best_score:.4f}")
    print(f"Crop images saved in: {image_save_dir}")


if __name__ == "__main__":
    main()