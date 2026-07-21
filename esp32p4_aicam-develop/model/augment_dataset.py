from pathlib import Path
import random

from PIL import Image
from torchvision import transforms

TARGET_COUNT = 700

ROOT = Path(__file__).resolve().parent / "finetune_train"

random.seed(42)

augment = transforms.Compose([
    transforms.RandomHorizontalFlip(p=0.5),

    transforms.RandomApply([
        transforms.ColorJitter(
            brightness=0.3,
            contrast=0.3,
            saturation=0.3
        )
    ], p=0.8),

    transforms.RandomRotation(8),

    transforms.RandomResizedCrop(
        size=224,
        scale=(0.9, 1.0),
        ratio=(0.95, 1.05)
    ),

    transforms.RandomGrayscale(p=0.15),
])


def augment_folder(folder: Path):
    images = sorted(list(folder.glob("*.jpg")))
    images += sorted(list(folder.glob("*.png")))
    images += sorted(list(folder.glob("*.jpeg")))

    original_images = [img for img in images if not img.name.startswith("aug_")]

    current = len(images)

    print(f"\n{folder.name}")
    print(f"Current : {current}")

    idx = 0

    while current < TARGET_COUNT:

        img_path = random.choice(original_images)

        image = Image.open(img_path).convert("RGB")

        aug = augment(image)

        save_name = folder / f"aug_{idx:06d}.jpg"

        while save_name.exists():
            idx += 1
            save_name = folder / f"aug_{idx:06d}.jpg"

        aug.save(save_name, quality=95)

        current += 1
        idx += 1

    print(f"Done -> {current}")


if __name__ == "__main__":

    augment_folder(ROOT / "human")
    augment_folder(ROOT / "no_human")