from __future__ import annotations

import argparse
from pathlib import Path

import torch
from PIL import Image

from model_utils import (
    CHECKPOINT_PATH,
    FIVE_CROP_HUMAN_THRESHOLD,
    NUM_CLASSES,
    build_model,
    build_transforms,
    checkpoint_state_dict,
    load_labels,
    make_five_crops,
    validate_checkpoint_compatibility,
)


def parse_args():
    parser = argparse.ArgumentParser(description="5-Crop inference test")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, default=CHECKPOINT_PATH)
    parser.add_argument(
        "--human-threshold",
        type=float,
        default=FIVE_CROP_HUMAN_THRESHOLD,
        help="Human-score threshold selected on the validation split.",
    )
    parser.add_argument(
        "--save-dir",
        type=Path,
        default=Path("crop_results"),
        help="Folder where cropped images will be saved.",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    if not 0.0 <= args.human_threshold <= 1.0:
        raise ValueError("--human-threshold must be between 0 and 1.")

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
    crops = make_five_crops(image)

    human_idx = labels.index("human")
    no_human_idx = labels.index("no_human")

    best_score = -1.0
    best_no_human_score = -1.0
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
                best_no_human_score = no_human_score
                best_region = name

    decision_label = (
        labels[human_idx]
        if best_score >= args.human_threshold
        else labels[no_human_idx]
    )
    decision_score = (
        best_score
        if decision_label == labels[human_idx]
        else best_no_human_score
    )
    print("=" * 70)
    print(f"Selected Region : {best_region}")
    print(f"Best Human Score: {best_score:.4f}")
    print(f"Human Threshold : {args.human_threshold:.8f} (selected on val)")
    print(f"Final Decision  : {decision_label} ({decision_score:.4f})")
    print(f"Crop images saved in: {image_save_dir}")


if __name__ == "__main__":
    main()
