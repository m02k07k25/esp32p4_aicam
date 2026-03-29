from __future__ import annotations

import argparse
import json
import random
import shutil
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from urllib.request import urlopen

from model_utils import CALIB_ROOT, DOWNLOAD_ROOT, LABELS_PATH, TRAIN_ROOT, VAL_ROOT, load_labels

COCO_ANNOTATIONS_URL = "http://images.cocodataset.org/annotations/annotations_trainval2017.zip"
COCO_BASE_URL = "http://images.cocodataset.org"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Download a balanced human/no_human subset from COCO 2017 into download/ and model/data/."
    )
    parser.add_argument("--download-dir", type=Path, default=DOWNLOAD_ROOT / "coco_human_no_human")
    parser.add_argument("--train-per-class", type=int, default=1000, help="Number of train images for each class.")
    parser.add_argument("--val-per-class", type=int, default=250, help="Number of val images for each class.")
    parser.add_argument("--calib-per-class", type=int, default=64, help="Number of calibration images for each class.")
    parser.add_argument("--seed", type=int, default=42, help="Random seed.")
    parser.add_argument("--workers", type=int, default=8, help="Concurrent download workers.")
    parser.add_argument(
        "--force-redownload",
        action="store_true",
        help="Redownload images even if they already exist in download/.",
    )
    return parser.parse_args()


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def reset_target_dir(path: Path) -> None:
    ensure_dir(path)
    for child in path.iterdir():
        if child.name == ".gitkeep":
            continue
        if child.is_file():
            child.unlink()
        elif child.is_dir():
            shutil.rmtree(child)


def copy_into_target(source: Path, target_dir: Path, target_name: str | None = None) -> None:
    ensure_dir(target_dir)
    destination = target_dir / (target_name or source.name)
    shutil.copy2(source, destination)


def download_file(url: str, destination: Path) -> None:
    ensure_dir(destination.parent)
    with urlopen(url, timeout=120) as response, destination.open("wb") as fp:
        shutil.copyfileobj(response, fp)


def download_if_needed(url: str, destination: Path, force: bool = False) -> Path:
    if destination.exists() and not force:
        return destination
    download_file(url, destination)
    return destination


def extract_member(zip_path: Path, member_name: str, destination: Path) -> Path:
    if destination.exists():
        return destination
    ensure_dir(destination.parent)
    with zipfile.ZipFile(zip_path) as zf:
        with zf.open(member_name) as src, destination.open("wb") as dst:
            shutil.copyfileobj(src, dst)
    return destination


def load_coco_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as fp:
        return json.load(fp)


def person_category_id(coco: dict) -> int:
    for category in coco["categories"]:
        if category["name"] == "person":
            return int(category["id"])
    raise RuntimeError("COCO categories did not contain 'person'.")


def sample_image_infos(coco: dict, person_id: int, class_label: str, sample_count: int, rng: random.Random) -> list[dict]:
    images_by_id = {int(image["id"]): image for image in coco["images"]}
    human_ids = {int(annotation["image_id"]) for annotation in coco["annotations"] if int(annotation["category_id"]) == person_id}
    all_ids = set(images_by_id.keys())

    if class_label == "human":
        candidate_ids = sorted(human_ids)
    elif class_label == "no_human":
        candidate_ids = sorted(all_ids - human_ids)
    else:
        raise ValueError(f"Unsupported class label: {class_label}")

    if len(candidate_ids) < sample_count:
        raise RuntimeError(f"Requested {sample_count} {class_label} images, but COCO only has {len(candidate_ids)}.")

    selected_ids = rng.sample(candidate_ids, sample_count)
    return [images_by_id[image_id] for image_id in selected_ids]


def image_url(split_name: str, image_info: dict) -> str:
    if image_info.get("coco_url"):
        return str(image_info["coco_url"])
    return f"{COCO_BASE_URL}/{split_name}/{image_info['file_name']}"


def download_selected_images(
    split_name: str,
    image_infos: list[dict],
    image_root: Path,
    workers: int,
    force_redownload: bool,
) -> list[Path]:
    ensure_dir(image_root)
    downloaded_paths: list[Path] = [image_root / str(info["file_name"]) for info in image_infos]

    def task(info: dict, destination: Path) -> Path:
        download_if_needed(image_url(split_name, info), destination, force=force_redownload)
        return destination

    with ThreadPoolExecutor(max_workers=max(1, workers)) as executor:
        futures = [executor.submit(task, info, destination) for info, destination in zip(image_infos, downloaded_paths)]
        for future in as_completed(futures):
            future.result()

    return downloaded_paths


def populate_split(target_root: Path, label_to_images: dict[str, list[Path]]) -> None:
    for label, source_paths in label_to_images.items():
        target_dir = target_root / label
        reset_target_dir(target_dir)
        for source_path in source_paths:
            copy_into_target(source_path, target_dir)


def populate_calib(calib_root: Path, calib_images: list[tuple[str, Path]]) -> None:
    reset_target_dir(calib_root)
    for index, (label, source_path) in enumerate(calib_images):
        target_name = f"{index:04d}_{label}_{source_path.name}"
        copy_into_target(source_path, calib_root, target_name=target_name)


def main() -> None:
    args = parse_args()
    labels = load_labels(LABELS_PATH)
    expected_labels = ["no_human", "human"]
    if labels != expected_labels:
        raise RuntimeError(f"Expected labels.txt to be {expected_labels}, got {labels}.")

    rng = random.Random(args.seed)
    download_root = args.download_dir
    annotations_root = download_root / "annotations"
    raw_image_root = download_root / "images"
    ensure_dir(annotations_root)
    ensure_dir(raw_image_root)

    annotations_zip = download_root / "annotations_trainval2017.zip"
    print(f"Downloading COCO annotations to {annotations_zip} ...")
    download_if_needed(COCO_ANNOTATIONS_URL, annotations_zip)

    train_json_path = extract_member(
        annotations_zip,
        "annotations/instances_train2017.json",
        annotations_root / "instances_train2017.json",
    )
    val_json_path = extract_member(
        annotations_zip,
        "annotations/instances_val2017.json",
        annotations_root / "instances_val2017.json",
    )

    print("Loading COCO annotations ...")
    train_coco = load_coco_json(train_json_path)
    val_coco = load_coco_json(val_json_path)
    person_id = person_category_id(train_coco)

    print("Sampling image ids ...")
    train_samples = {
        label: sample_image_infos(train_coco, person_id, label, args.train_per_class, rng) for label in labels
    }
    val_samples = {
        label: sample_image_infos(val_coco, person_id, label, args.val_per_class, rng) for label in labels
    }

    print("Downloading selected train images ...")
    downloaded_train = {
        label: download_selected_images(
            "train2017",
            train_samples[label],
            raw_image_root / "train2017" / label,
            args.workers,
            args.force_redownload,
        )
        for label in labels
    }
    print("Downloading selected val images ...")
    downloaded_val = {
        label: download_selected_images(
            "val2017",
            val_samples[label],
            raw_image_root / "val2017" / label,
            args.workers,
            args.force_redownload,
        )
        for label in labels
    }

    print("Populating model/data/train and model/data/val ...")
    populate_split(TRAIN_ROOT, downloaded_train)
    populate_split(VAL_ROOT, downloaded_val)

    calib_images: list[tuple[str, Path]] = []
    for label in labels:
        calib_subset = downloaded_train[label][: args.calib_per_class]
        if len(calib_subset) < args.calib_per_class:
            raise RuntimeError(f"Not enough train images for calib class {label}.")
        calib_images.extend((label, image_path) for image_path in calib_subset)
    rng.shuffle(calib_images)
    print("Populating model/data/calib ...")
    populate_calib(CALIB_ROOT, calib_images)

    print("Done.")
    print(f"Train: {args.train_per_class} per class -> {TRAIN_ROOT}")
    print(f"Val: {args.val_per_class} per class -> {VAL_ROOT}")
    print(f"Calib: {args.calib_per_class} per class -> {CALIB_ROOT}")
    print(f"Raw downloads cached under {download_root}")


if __name__ == "__main__":
    main()
