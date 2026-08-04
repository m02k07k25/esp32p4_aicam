from __future__ import annotations

import argparse
import hashlib
import time
from io import BytesIO
from pathlib import Path
from urllib.request import Request, urlopen

from model_utils import DOWNLOAD_ROOT, IMAGE_SIZE, EspDlNearestResize, load_labels

DEFAULT_OUTPUT_ROOT = DOWNLOAD_ROOT / "mountain_person"
EXPECTED_LABELS = ["no_human", "human"]

SEARCH_KEYWORDS = {
    "human": [
        "hiker mountain trail daytime",
        "person hiking mountain daylight",
        "people hiking mountain trail",
        "backpacker mountain trail daylight",
        "trekker mountain path daytime",
        "person walking forest mountain trail",
        "person standing mountain trail",
    ],
    "no_human": [
        "empty mountain trail daytime",
        "mountain path no people daylight",
        "forest mountain trail no person",
        "empty hiking trail mountain",
        "mountain landscape trail daytime",
        "empty forest hiking path",
        "nature mountain path no people",
    ],
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Collect mountain human/no_human candidates and simulate the RGB565 camera input."
    )
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--per-class", type=int, default=700)
    parser.add_argument("--image-size", type=int, default=IMAGE_SIZE)
    parser.add_argument("--results-per-query", type=int, default=200)
    parser.add_argument("--request-delay", type=float, default=0.1)
    return parser.parse_args()


def square_crop(image):
    width, height = image.size
    side = min(width, height)
    left = (width - side) // 2
    top = (height - side) // 2
    return image.crop((left, top, left + side, top + side))


def simulate_rgb565(image):
    image = image.convert("RGB")
    pixels = image.load()
    width, height = image.size

    for y in range(height):
        for x in range(width):
            red, green, blue = pixels[x, y]
            red5, green6, blue5 = red >> 3, green >> 2, blue >> 3
            pixels[x, y] = (
                (red5 << 3) | (red5 >> 2),
                (green6 << 2) | (green6 >> 4),
                (blue5 << 3) | (blue5 >> 2),
            )

    return image


def image_digest(image) -> str:
    preview = image.resize((16, 16)).convert("L")
    return hashlib.sha256(preview.tobytes()).hexdigest()


def download_candidate(url: str, image_size: int):
    from PIL import Image

    request = Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urlopen(request, timeout=10) as response:
        image = Image.open(BytesIO(response.read())).convert("RGB")

    if image.width < image_size or image.height < image_size:
        return None

    image = simulate_rgb565(square_crop(image))
    return EspDlNearestResize((image_size, image_size))(image)


def existing_digests(label_dir: Path) -> set[str]:
    from PIL import Image

    digests: set[str] = set()
    for path in label_dir.glob("*.jpg"):
        try:
            with Image.open(path) as image:
                digests.add(image_digest(image))
        except OSError:
            continue
    return digests


def collect_label(label: str, args: argparse.Namespace) -> None:
    try:
        from ddgs import DDGS
    except ImportError as exc:
        raise RuntimeError("Install the collector dependencies with: pip install pillow ddgs") from exc

    label_dir = args.output_dir / label
    label_dir.mkdir(parents=True, exist_ok=True)
    saved = len(list(label_dir.glob("*.jpg")))
    digests = existing_digests(label_dir)

    with DDGS() as search:
        for keyword in SEARCH_KEYWORDS[label]:
            if saved >= args.per_class:
                break

            print(f"[{label}] searching: {keyword}")
            results = search.images(
                keyword,
                max_results=args.results_per_query,
                safesearch="moderate",
                size="Medium",
                type_image="photo",
            )
            for item in results:
                if saved >= args.per_class:
                    break

                url = item.get("image")
                if not url:
                    continue

                try:
                    image = download_candidate(url, args.image_size)
                except Exception:
                    continue
                if image is None:
                    continue

                digest = image_digest(image)
                if digest in digests:
                    continue

                output_path = label_dir / f"mountain_{label}_{saved:04d}.jpg"
                image.save(output_path, format="JPEG", quality=95)
                digests.add(digest)
                saved += 1
                time.sleep(max(0.0, args.request_delay))

    print(f"[{label}] collected {saved}/{args.per_class} images in {label_dir}")


def main() -> None:
    args = parse_args()
    if args.per_class <= 0 or args.image_size <= 0 or args.results_per_query <= 0:
        raise ValueError("per-class, image-size, and results-per-query must be positive")

    labels = load_labels()
    if labels != EXPECTED_LABELS:
        raise RuntimeError(f"Expected labels {EXPECTED_LABELS}, got {labels}.")

    for label in labels:
        collect_label(label, args)

    print("Review labels and image usage rights before copying candidates into model/data train and val splits.")


if __name__ == "__main__":
    main()
