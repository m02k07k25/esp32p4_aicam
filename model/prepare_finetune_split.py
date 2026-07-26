from __future__ import annotations

import argparse
import hashlib
import json
import random
import shutil
from dataclasses import dataclass
from pathlib import Path

from PIL import Image

from model_utils import (
    DATA_ROOT,
    FINETUNE_SOURCE_ROOT,
    SUPPORTED_IMAGE_SUFFIXES,
    load_labels,
    validate_finetune_split,
)


DEFAULT_SOURCE = FINETUNE_SOURCE_ROOT
DEFAULT_OUTPUT = DATA_ROOT
DEFAULT_EXCLUDE_LIST = DATA_ROOT / "finetune_exclude.txt"


@dataclass(frozen=True)
class ImageRecord:
    path: Path
    sha256: str
    dhash: int
    exact_duplicates: tuple[Path, ...]


@dataclass(frozen=True)
class ClassSplitPlan:
    records: list[ImageRecord]
    groups: list[list[int]]
    split_by_group: dict[int, str]
    summary: dict[str, object]


class DisjointSet:
    def __init__(self, size: int) -> None:
        self.parent = list(range(size))
        self.rank = [0] * size

    def find(self, item: int) -> int:
        while self.parent[item] != item:
            self.parent[item] = self.parent[self.parent[item]]
            item = self.parent[item]
        return item

    def union(self, left: int, right: int) -> None:
        left_root = self.find(left)
        right_root = self.find(right)
        if left_root == right_root:
            return

        if self.rank[left_root] < self.rank[right_root]:
            left_root, right_root = right_root, left_root
        self.parent[right_root] = left_root
        if self.rank[left_root] == self.rank[right_root]:
            self.rank[left_root] += 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Create a deterministic, class-stratified fine-tuning split while "
            "removing exact duplicates and keeping near-duplicates together."
        )
    )
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--exclude-list",
        type=Path,
        default=DEFAULT_EXCLUDE_LIST,
        help="Optional source-relative paths to exclude, one per line.",
    )
    parser.add_argument("--val-ratio", type=float, default=0.15)
    parser.add_argument("--test-ratio", type=float, default=0.15)
    parser.add_argument(
        "--calib-per-class",
        type=int,
        default=64,
        help="Training images copied into calib for each class.",
    )
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument(
        "--near-duplicate-distance",
        type=int,
        default=2,
        help="Maximum 64-bit dHash Hamming distance for grouping similar images.",
    )
    parser.add_argument(
        "--replace-generated",
        action="store_true",
        help="Replace only output train/val/test/calib and split_manifest.json.",
    )
    return parser.parse_args()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def difference_hash(path: Path, hash_size: int = 8) -> int:
    with Image.open(path) as image:
        grayscale = image.convert("L").resize(
            (hash_size + 1, hash_size),
            Image.Resampling.LANCZOS,
        )
        pixels = list(grayscale.tobytes())

    result = 0
    row_width = hash_size + 1
    for row in range(hash_size):
        row_start = row * row_width
        for column in range(hash_size):
            result <<= 1
            result |= pixels[row_start + column] > pixels[row_start + column + 1]
    return result


def collect_class_images(class_root: Path) -> list[Path]:
    return [
        path
        for path in sorted(class_root.rglob("*"))
        if path.is_file() and path.suffix.lower() in SUPPORTED_IMAGE_SUFFIXES
    ]


def deduplicate(paths: list[Path]) -> list[ImageRecord]:
    paths_by_digest: dict[str, list[Path]] = {}
    for path in paths:
        paths_by_digest.setdefault(sha256_file(path), []).append(path)

    records: list[ImageRecord] = []
    for digest, matching_paths in sorted(paths_by_digest.items()):
        ordered_paths = sorted(matching_paths)
        representative = ordered_paths[0]
        records.append(
            ImageRecord(
                path=representative,
                sha256=digest,
                dhash=difference_hash(representative),
                exact_duplicates=tuple(ordered_paths[1:]),
            )
        )
    return sorted(records, key=lambda record: record.path.as_posix())


def group_near_duplicates(
    records: list[ImageRecord],
    maximum_distance: int,
) -> list[list[int]]:
    groups = DisjointSet(len(records))
    for left in range(len(records)):
        for right in range(left + 1, len(records)):
            if (records[left].dhash ^ records[right].dhash).bit_count() <= maximum_distance:
                groups.union(left, right)

    members_by_root: dict[int, list[int]] = {}
    for index in range(len(records)):
        members_by_root.setdefault(groups.find(index), []).append(index)
    return sorted(members_by_root.values(), key=lambda members: members[0])


def choose_groups_for_split(
    groups: list[list[int]],
    candidate_group_indices: list[int],
    target_count: int,
    minimum_group_count: int,
    maximum_group_count: int,
    rng: random.Random,
) -> set[int]:
    group_indices = list(candidate_group_indices)
    rng.shuffle(group_indices)

    selections: dict[tuple[int, int], tuple[int, ...]] = {(0, 0): ()}
    for group_index in group_indices:
        group_size = len(groups[group_index])
        additions: dict[tuple[int, int], tuple[int, ...]] = {}
        for (current_count, current_group_count), selected_groups in selections.items():
            next_key = (current_count + group_size, current_group_count + 1)
            if next_key not in selections and next_key not in additions:
                additions[next_key] = selected_groups + (group_index,)
        selections.update(additions)

    valid_keys = [
        key
        for key in selections
        if minimum_group_count <= key[1] <= maximum_group_count
    ]
    if not valid_keys:
        raise RuntimeError(
            "Could not select a valid set of near-duplicate groups for a split."
        )

    best_key = min(
        valid_keys,
        key=lambda key: (
            abs(key[0] - target_count),
            key[0] > target_count,
            key[0],
            key[1],
        ),
    )
    return set(selections[best_key])


def prepare_output(output: Path, replace_generated: bool) -> None:
    generated_directories = ("train", "val", "test", "calib")
    occupied = [
        output / name
        for name in generated_directories
        if (output / name).exists()
        and any(
            candidate.name != ".gitkeep"
            for candidate in (output / name).rglob("*")
            if candidate.is_file() or candidate.is_symlink()
        )
    ]
    manifest_path = output / "split_manifest.json"
    if manifest_path.exists():
        occupied.append(manifest_path)
    if occupied and not replace_generated:
        locations = ", ".join(str(path) for path in occupied)
        raise FileExistsError(
            "Generated dataset paths are not empty: "
            f"{locations}. Move or remove them explicitly before regenerating."
        )
    output.mkdir(parents=True, exist_ok=True)
    if replace_generated:
        for name in generated_directories:
            generated_path = output / name
            if generated_path.is_symlink():
                raise ValueError(
                    f"Refusing to replace symlinked generated path: {generated_path}"
                )
            if generated_path.exists():
                shutil.rmtree(generated_path)
        if manifest_path.exists():
            manifest_path.unlink()

    placeholders = {
        Path("calib/.gitkeep"): "# Add calibration images here. Nested folders are allowed.\n",
        Path("train/human/.gitkeep"): "# Add human training images here.\n",
        Path("train/no_human/.gitkeep"): "# Add no_human training images here.\n",
        Path("val/human/.gitkeep"): "# Add human validation images here.\n",
        Path("val/no_human/.gitkeep"): "# Add no_human validation images here.\n",
    }
    for relative_path, content in placeholders.items():
        placeholder = output / relative_path
        placeholder.parent.mkdir(parents=True, exist_ok=True)
        placeholder.write_text(content, encoding="utf-8")


def load_exclusions(
    exclude_list: Path,
    source: Path,
    labels: list[str],
) -> set[Path]:
    if not exclude_list.exists():
        return set()

    exclusions: set[Path] = set()
    for line_number, raw_line in enumerate(
        exclude_list.read_text(encoding="utf-8").splitlines(),
        start=1,
    ):
        entry = raw_line.strip()
        if not entry or entry.startswith("#"):
            continue
        relative_path = Path(entry)
        if (
            relative_path.is_absolute()
            or ".." in relative_path.parts
            or len(relative_path.parts) < 2
            or relative_path.parts[0] not in labels
        ):
            raise ValueError(
                f"Unsafe exclusion at {exclude_list}:{line_number}: {entry}"
            )
        source_path = source / relative_path
        if not source_path.is_file():
            raise FileNotFoundError(
                f"Excluded source image does not exist: {source_path}"
            )
        exclusions.add(source_path.resolve())
    return exclusions


def relative_name(path: Path, root: Path) -> str:
    return path.resolve().relative_to(root.resolve()).as_posix()


def validate_created_split(
    output: Path,
    manifest: dict[str, object],
    labels: list[str],
    near_duplicate_distance: int,
) -> None:
    entries = manifest["images"]
    if not isinstance(entries, list):
        raise TypeError("Split manifest images must be a list.")

    valid_splits = {"train", "val", "test"}
    group_splits: dict[tuple[str, str], set[str]] = {}
    digest_splits: dict[str, set[str]] = {}
    entries_by_label: dict[str, list[dict[str, object]]] = {
        label: []
        for label in labels
    }
    counts = {
        label: {split: 0 for split in valid_splits}
        for label in labels
    }
    expected_files: set[Path] = set()

    for entry in entries:
        if not isinstance(entry, dict):
            raise TypeError("Every split manifest image entry must be an object.")
        label = entry["class"]
        split = entry["split"]
        group = entry["group"]
        digest = entry["sha256"]
        if label not in labels or split not in valid_splits:
            raise RuntimeError(f"Invalid split manifest entry: {entry}")

        group_splits.setdefault((label, group), set()).add(split)
        digest_splits.setdefault(digest, set()).add(split)
        entries_by_label[label].append(entry)
        counts[label][split] += 1

        destination = output / split / Path(entry["source"])
        if not destination.is_file():
            raise FileNotFoundError(
                f"Split manifest references a missing image: {destination}"
            )
        if sha256_file(destination) != digest:
            raise RuntimeError(f"Copied image checksum mismatch: {destination}")
        expected_files.add(destination.resolve())

    leaking_groups = {
        key: splits
        for key, splits in group_splits.items()
        if len(splits) != 1
    }
    leaking_digests = {
        digest: splits
        for digest, splits in digest_splits.items()
        if len(splits) != 1
    }
    if leaking_groups or leaking_digests:
        raise RuntimeError(
            "Split leakage detected: "
            f"groups={leaking_groups}, sha256={leaking_digests}"
        )

    for label, label_entries in entries_by_label.items():
        for left in range(len(label_entries)):
            left_entry = label_entries[left]
            left_hash = int(left_entry["dhash"], 16)
            for right in range(left + 1, len(label_entries)):
                right_entry = label_entries[right]
                right_hash = int(right_entry["dhash"], 16)
                if (
                    (left_hash ^ right_hash).bit_count()
                    <= near_duplicate_distance
                    and left_entry["split"] != right_entry["split"]
                ):
                    raise RuntimeError(
                        "Near-duplicate leakage detected between "
                        f"{left_entry['source']} and {right_entry['source']}."
                    )

    actual_files = {
        path.resolve()
        for split in valid_splits
        for path in collect_class_images(output / split)
    }
    if actual_files != expected_files:
        raise RuntimeError(
            "Split files do not match the manifest: "
            f"extra={sorted(actual_files - expected_files)}, "
            f"missing={sorted(expected_files - actual_files)}"
        )

    class_summaries = manifest["classes"]
    for label in labels:
        for split in valid_splits:
            if class_summaries[label][split] != counts[label][split]:
                raise RuntimeError(
                    f"Manifest count mismatch for {label}/{split}."
                )


def main() -> None:
    args = parse_args()
    if not 0.0 < args.val_ratio < 1.0:
        raise ValueError("--val-ratio must be between 0 and 1.")
    if not 0.0 < args.test_ratio < 1.0:
        raise ValueError("--test-ratio must be between 0 and 1.")
    if args.val_ratio + args.test_ratio >= 1.0:
        raise ValueError("--val-ratio + --test-ratio must be less than 1.")
    if args.calib_per_class <= 0:
        raise ValueError("--calib-per-class must be positive.")
    if not 0 <= args.near_duplicate_distance <= 64:
        raise ValueError("--near-duplicate-distance must be between 0 and 64.")

    source_resolved = args.source.resolve()
    output_resolved = args.output.resolve()
    generated_roots = [
        (output_resolved / name).resolve()
        for name in ("train", "val", "test", "calib")
    ]
    if any(
        source_resolved == generated_root
        or source_resolved in generated_root.parents
        or generated_root in source_resolved.parents
        for generated_root in generated_roots
    ):
        raise ValueError(
            "--source must not overlap the generated train/val/test/calib directories."
        )

    labels = load_labels()
    missing = [label for label in labels if not (args.source / label).is_dir()]
    if missing:
        raise FileNotFoundError(
            f"Missing class directories under {args.source}: {', '.join(missing)}"
        )

    exclusions = load_exclusions(args.exclude_list, args.source, labels)
    class_paths = {
        label: [
            path
            for path in collect_class_images(args.source / label)
            if path.resolve() not in exclusions
        ]
        for label in labels
    }
    if any(not paths for paths in class_paths.values()):
        raise RuntimeError("Every class must contain at least one image.")

    digest_labels: dict[str, set[str]] = {}
    for label, paths in class_paths.items():
        for path in paths:
            digest_labels.setdefault(sha256_file(path), set()).add(label)
    conflicts = {
        digest: conflict_labels
        for digest, conflict_labels in digest_labels.items()
        if len(conflict_labels) > 1
    }
    if conflicts:
        raise RuntimeError(
            f"Found {len(conflicts)} exact image(s) assigned to multiple classes."
        )

    records_by_label = {
        label: deduplicate(class_paths[label])
        for label in labels
    }
    cross_class_near_duplicates: list[tuple[str, Path, str, Path]] = []
    for left_index, left_label in enumerate(labels):
        for right_label in labels[left_index + 1 :]:
            for left_record in records_by_label[left_label]:
                for right_record in records_by_label[right_label]:
                    distance = (
                        left_record.dhash ^ right_record.dhash
                    ).bit_count()
                    if distance <= args.near_duplicate_distance:
                        cross_class_near_duplicates.append(
                            (
                                left_label,
                                left_record.path,
                                right_label,
                                right_record.path,
                            )
                        )
    if cross_class_near_duplicates:
        examples = "; ".join(
            f"{left_label}:{left_path.name} <-> {right_label}:{right_path.name}"
            for left_label, left_path, right_label, right_path
            in cross_class_near_duplicates[:5]
        )
        raise RuntimeError(
            f"Found {len(cross_class_near_duplicates)} near-duplicate pair(s) "
            f"assigned to different classes: {examples}"
        )

    rng = random.Random(args.seed)
    plans: dict[str, ClassSplitPlan] = {}
    for label in labels:
        records = records_by_label[label]
        groups = group_near_duplicates(records, args.near_duplicate_distance)
        if len(records) < 3 or len(groups) < 3:
            raise RuntimeError(
                f"Class {label} needs at least three independent image groups to split."
            )

        all_group_indices = list(range(len(groups)))
        target_holdout_count = round(
            len(records) * (args.val_ratio + args.test_ratio)
        )
        holdout_groups = choose_groups_for_split(
            groups,
            all_group_indices,
            target_holdout_count,
            minimum_group_count=2,
            maximum_group_count=len(all_group_indices) - 1,
            rng=rng,
        )
        target_test_count = round(len(records) * args.test_ratio)
        test_groups = choose_groups_for_split(
            groups,
            sorted(holdout_groups),
            target_test_count,
            minimum_group_count=1,
            maximum_group_count=len(holdout_groups) - 1,
            rng=rng,
        )
        validation_groups = holdout_groups - test_groups
        training_groups = set(all_group_indices) - holdout_groups
        if not test_groups or not validation_groups or not training_groups:
            raise RuntimeError(
                f"Could not create non-empty train, val, and test splits for class {label}."
            )

        split_by_group = {
            group_index: (
                "test"
                if group_index in test_groups
                else "val"
                if group_index in validation_groups
                else "train"
            )
            for group_index in all_group_indices
        }
        split_counts = {
            split: sum(
                len(groups[group_index])
                for group_index, assigned_split in split_by_group.items()
                if assigned_split == split
            )
            for split in ("train", "val", "test")
        }
        requested_counts = {
            "train": len(records) - target_holdout_count,
            "val": target_holdout_count - target_test_count,
            "test": target_test_count,
        }
        duplicate_count = sum(
            len(record.exact_duplicates)
            for record in records
        )
        summary = {
            "source": len(class_paths[label]),
            "unique": len(records),
            "discarded_exact_duplicates": duplicate_count,
            "near_duplicate_groups": sum(len(group) > 1 for group in groups),
            "requested_counts": requested_counts,
            "actual_ratios": {
                split: split_counts[split] / len(records)
                for split in ("train", "val", "test")
            },
            **split_counts,
        }
        plans[label] = ClassSplitPlan(
            records=records,
            groups=groups,
            split_by_group=split_by_group,
            summary=summary,
        )

    for label, plan in plans.items():
        training_count = int(plan.summary["train"])
        if training_count < args.calib_per_class:
            raise RuntimeError(
                f"Class {label} would have only {training_count} training images, "
                f"fewer than --calib-per-class={args.calib_per_class}."
            )

    prepare_output(args.output, args.replace_generated)
    manifest: dict[str, object] = {
        "format_version": 2,
        "source": str(args.source.resolve()),
        "output": str(args.output.resolve()),
        "excluded": sorted(
            relative_name(path, args.source)
            for path in exclusions
        ),
        "seed": args.seed,
        "ratios": {
            "train": 1.0 - args.val_ratio - args.test_ratio,
            "val": args.val_ratio,
            "test": args.test_ratio,
        },
        "labels": labels,
        "near_duplicate_hash": "64-bit dHash",
        "near_duplicate_distance": args.near_duplicate_distance,
        "calibration": {
            "source_split": "train",
            "requested_per_class": args.calib_per_class,
            "images": {},
        },
        "classes": {},
        "images": [],
    }
    summary: dict[str, dict[str, object]] = {}

    for label in labels:
        plan = plans[label]
        for group_index, member_indices in enumerate(plan.groups):
            split = plan.split_by_group[group_index]
            group_id = f"{label}-{group_index:04d}"
            destination_root = args.output / split / label

            for record_index in member_indices:
                record = plan.records[record_index]
                class_relative_path = record.path.relative_to(args.source / label)
                destination = destination_root / class_relative_path
                if destination.exists():
                    raise FileExistsError(f"Duplicate output filename: {destination}")
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(record.path, destination)

                manifest["images"].append(
                    {
                        "class": label,
                        "split": split,
                        "group": group_id,
                        "source": relative_name(record.path, args.source),
                        "sha256": record.sha256,
                        "dhash": f"{record.dhash:016x}",
                        "discarded_exact_duplicates": [
                            relative_name(path, args.source)
                            for path in record.exact_duplicates
                        ],
                    }
                )

        summary[label] = plan.summary
        manifest["classes"][label] = summary[label]

    validate_created_split(
        args.output,
        manifest,
        labels,
        args.near_duplicate_distance,
    )

    calibration_rng = random.Random(args.seed)
    for label in labels:
        training_root = args.output / "train" / label
        calibration_root = args.output / "calib" / label
        candidates = collect_class_images(training_root)
        if len(candidates) < args.calib_per_class:
            raise RuntimeError(
                f"Class {label} has only {len(candidates)} training images, "
                f"fewer than --calib-per-class={args.calib_per_class}."
            )
        calibration_rng.shuffle(candidates)
        selected = candidates[: args.calib_per_class]
        manifest["calibration"]["images"][label] = []
        for source_path in selected:
            relative_path = source_path.relative_to(training_root)
            destination = calibration_root / relative_path
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source_path, destination)
            manifest["calibration"]["images"][label].append(
                {
                    "source": relative_name(source_path, args.output),
                    "sha256": sha256_file(source_path),
                }
            )

    manifest_path = args.output / "split_manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    validate_finetune_split(args.output, manifest_path, labels)

    print(f"Fine-tuning split created under {args.output}")
    for label in labels:
        stats = summary[label]
        print(
            f"{label}: source={stats['source']}, unique={stats['unique']}, "
            f"train={stats['train']}, val={stats['val']}, test={stats['test']}, "
            f"exact_duplicates_removed={stats['discarded_exact_duplicates']}, "
            f"near_duplicate_groups={stats['near_duplicate_groups']}"
        )
    print(f"Manifest: {manifest_path}")
    print(
        "Calibration: "
        f"{args.calib_per_class} images/class under {args.output / 'calib'}"
    )


if __name__ == "__main__":
    main()
