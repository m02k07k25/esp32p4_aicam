import time
import hashlib
from pathlib import Path
from io import BytesIO

import requests
from PIL import Image
from tqdm import tqdm
from ddgs import DDGS


PERSON_NUM = 700
NO_PERSON_NUM = 700
IMAGE_SIZE = 224

OUT_DIR = Path("dataset")

PERSON_KEYWORDS = [
    "hiker mountain trail daytime",
    "person hiking mountain daylight",
    "people hiking mountain trail",
    "hiker on mountain path daytime",
    "mountain hiking person outdoor",
    "man hiking mountain trail daytime",
    "woman hiking mountain trail daytime",
    "backpacker mountain trail daylight",
    "trekker mountain path daytime",
    "person walking forest mountain trail",
    "hikers walking in mountains daytime",
    "person on hiking trail mountain",
    "mountain trekking person daylight",
    "person standing mountain trail",
    "hiker forest trail mountain daytime",
]

NO_PERSON_KEYWORDS = [
    "empty mountain trail daytime",
    "mountain path no people daylight",
    "forest mountain trail no person",
    "mountain landscape trail daytime",
    "empty hiking trail mountain",
    "mountain forest path empty daytime",
    "hiking trail no people daylight",
    "mountain trail without people",
    "empty forest hiking path",
    "mountain road no people daytime",
    "quiet mountain trail daylight",
    "nature mountain path no people",
    "empty trekking trail mountain",
    "forest trail no person daytime",
    "mountain scenery no people trail",
]


def square_crop(img):
    w, h = img.size
    side = min(w, h)
    left = (w - side) // 2
    top = (h - side) // 2
    return img.crop((left, top, left + side, top + side))


def rgb565_simulation(img):
    img = img.convert("RGB")
    px = img.load()
    w, h = img.size

    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]

            r5 = r >> 3
            g6 = g >> 2
            b5 = b >> 3

            r8 = (r5 << 3) | (r5 >> 2)
            g8 = (g6 << 2) | (g6 >> 4)
            b8 = (b5 << 3) | (b5 >> 2)

            px[x, y] = (r8, g8, b8)

    return img


def image_hash(img):
    small = img.resize((16, 16)).convert("L")
    return hashlib.md5(small.tobytes()).hexdigest()


def save_image(url, save_path):
    try:
        r = requests.get(
            url,
            timeout=10,
            headers={"User-Agent": "Mozilla/5.0"}
        )
        r.raise_for_status()

        img = Image.open(BytesIO(r.content)).convert("RGB")

        if img.width < 300 or img.height < 300:
            return None

        img = square_crop(img)
        img = img.resize((IMAGE_SIZE, IMAGE_SIZE))
        img = rgb565_simulation(img)

        save_path.parent.mkdir(parents=True, exist_ok=True)
        img.save(save_path, quality=95)

        return image_hash(img)

    except Exception:
        return None


def collect_images(keywords, label, target_num):
    label_dir = OUT_DIR / label
    label_dir.mkdir(parents=True, exist_ok=True)

    saved = len(list(label_dir.glob("*.jpg")))
    used_hashes = set()

    with DDGS() as ddgs:
        for keyword in keywords:
            if saved >= target_num:
                break

            print(f"\n검색어: {keyword}")

            results = ddgs.images(
                keyword,
                max_results=1000,
                safesearch="moderate",
                size="Medium",
                type_image="photo"
            )

            for item in tqdm(results, desc=label):
                if saved >= target_num:
                    break

                url = item.get("image")
                if not url:
                    continue

                save_path = label_dir / f"{label}_{saved:04d}.jpg"
                h = save_image(url, save_path)

                if h is None:
                    if save_path.exists():
                        save_path.unlink()
                    continue

                if h in used_hashes:
                    save_path.unlink()
                    continue

                used_hashes.add(h)
                saved += 1
                time.sleep(0.1)

    print(f"{label}: {saved}장 저장 완료")


def main():
    collect_images(PERSON_KEYWORDS, "person", PERSON_NUM)
    collect_images(NO_PERSON_KEYWORDS, "no_person", NO_PERSON_NUM)

    print("\n완료")
    print(f"person: {len(list((OUT_DIR / 'person').glob('*.jpg')))}장")
    print(f"no_person: {len(list((OUT_DIR / 'no_person').glob('*.jpg')))}장")


if __name__ == "__main__":
    main()