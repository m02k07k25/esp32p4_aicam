from __future__ import annotations

import argparse
import importlib.util
import itertools
import json
import random
from pathlib import Path

from model_utils import (
    CALIB_ROOT,
    ESPDL_PATH,
    FIVE_CROP_AGGREGATION,
    FIVE_CROP_HUMAN_THRESHOLD,
    FixedOrderImageDataset,
    IMAGE_SIZE,
    INPUT_MEAN,
    INPUT_STD,
    MODEL_ARCHITECTURE,
    MODEL_WIDTH_MULT,
    NUM_CLASSES,
    ONNX_PATH,
    PREPROCESS_PROFILE,
    SPLIT_MANIFEST_PATH,
    build_transforms,
    collect_images,
    ensure_parent_dir,
    file_sha256,
    load_labels,
    make_five_crops,
    validate_finetune_split,
    validate_calibration_set,
    validate_onnx_metadata,
)

MIXED_INT16_OPERATIONS = (
    "/features/features.1/conv/conv.0/conv.0.0/Conv",
    "/features/features.1/conv/conv.0/conv.0.2/Clip",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Quantize classifier_224.onnx into an ESP-DL .espdl model.")
    parser.add_argument("--onnx", type=Path, default=ONNX_PATH, help="Input ONNX model.")
    parser.add_argument(
        "--calib-dir",
        type=Path,
        default=CALIB_ROOT,
        help="Calibration images (defaults to model/data/calib).",
    )
    parser.add_argument("--output", type=Path, default=ESPDL_PATH, help="Output .espdl file.")
    parser.add_argument("--target", default="esp32p4", help="ESP-PPQ target platform.")
    parser.add_argument(
        "--bits",
        type=int,
        default=8,
        choices=(8, 16),
        help="ESP-DL quantization bits.",
    )
    parser.add_argument(
        "--calibration-algorithm",
        choices=("kl", "percentile", "minmax", "mse"),
        default="kl",
        help="Activation calibration algorithm.",
    )
    parser.add_argument(
        "--pure-int8",
        action="store_true",
        help="Disable the default INT16 first-bottleneck override (diagnostic only; it severely reduced validation accuracy).",
    )
    parser.add_argument("--batch-size", type=int, default=1, help="Calibration batch size. Use 1 for the fixed-shape ONNX export.")
    parser.add_argument(
        "--calib-steps",
        type=int,
        default=128,
        help="Maximum calibration steps (default gives 64 samples per class).",
    )
    parser.add_argument("--seed", type=int, default=42, help="Calibration image shuffle seed.")
    parser.add_argument("--workers", type=int, default=0, help="DataLoader worker processes.")
    parser.add_argument("--device", default="cpu", help='Torch device string. Keep this at "cpu" for a CPU-only .venv.')
    parser.add_argument("--skip-export-test-values", action="store_true", help="Disable test-value export in the .espdl file.")
    parser.add_argument("--verbose", type=int, default=1, help="ESP-PPQ verbosity.")
    parser.add_argument(
        "--error-report",
        action="store_true",
        help="Run ESP-PPQ graphwise and layerwise quantization error analysis.",
    )
    parser.add_argument(
        "--evaluate-split",
        choices=("val", "test", "both"),
        default=None,
        help="Evaluate the returned PPQ INT8 graph on this split.",
    )
    parser.add_argument(
        "--human-threshold",
        type=float,
        default=None,
        help=(
            "Optional five-crop human-score threshold. With val evaluation, omitting "
            "this recalibrates on val; otherwise the checked-in val-derived default is used."
        ),
    )
    parser.add_argument(
        "--split-manifest",
        type=Path,
        default=SPLIT_MANIFEST_PATH,
    )
    parser.add_argument(
        "--allow-unverified-split",
        action="store_true",
        help=(
            "Use an ONNX model whose checkpoint split is unverified. The current "
            "repository split is still used for calibration and optional diagnostics."
        ),
    )
    return parser.parse_args()


def import_ppq():
    if importlib.util.find_spec("ppq") is not None:
        from ppq import QuantizationSettingFactory, TargetPlatform, TorchExecutor
        from ppq.api import espdl_quantize_onnx

        return (
            QuantizationSettingFactory,
            espdl_quantize_onnx,
            TorchExecutor,
            TargetPlatform,
        )

    if importlib.util.find_spec("esp_ppq") is not None:
        from esp_ppq import QuantizationSettingFactory, TargetPlatform, TorchExecutor
        from esp_ppq.api import espdl_quantize_onnx

        return (
            QuantizationSettingFactory,
            espdl_quantize_onnx,
            TorchExecutor,
            TargetPlatform,
        )

    raise SystemExit(
        "ESP-PPQ is not installed in the active interpreter. "
        "Install it inside this project's .venv before running quantize_espdl.py."
    )


def collect_balanced_calibration_images(root: Path, seed: int) -> list[Path]:
    labels = load_labels()
    label_directories = [root / label for label in labels]
    rng = random.Random(seed)

    missing_directories = [
        directory for directory in label_directories if not directory.is_dir()
    ]
    if missing_directories:
        raise FileNotFoundError(
            "Calibration data must use one directory per label; missing: "
            + ", ".join(str(directory) for directory in missing_directories)
        )

    paths_by_label = [collect_images(directory) for directory in label_directories]
    empty_labels = [
        label
        for label, paths in zip(labels, paths_by_label)
        if not paths
    ]
    if empty_labels:
        raise RuntimeError(
            f"Calibration classes have no images under {root}: {empty_labels}"
        )
    for paths in paths_by_label:
        rng.shuffle(paths)

    return [
        path
        for row in itertools.zip_longest(*paths_by_label)
        for path in row
        if path is not None
    ]


def write_espdl_manifest(
    espdl_path: Path,
    onnx_path: Path,
    *,
    target: str,
    bits: int,
    precision_profile: str,
    calibration_algorithm: str,
    mixed_int16_operations: tuple[str, ...],
    human_threshold: float,
    threshold_source: str,
) -> Path:
    onnx_metadata = validate_onnx_metadata(onnx_path)
    if not espdl_path.is_file():
        raise FileNotFoundError(
            f"ESP-PPQ did not create the expected output: {espdl_path}"
        )

    manifest_path = espdl_path.with_suffix(espdl_path.suffix + ".json")
    manifest = {
        "format_version": 2,
        "architecture": MODEL_ARCHITECTURE,
        "width_mult": str(MODEL_WIDTH_MULT),
        "image_size": IMAGE_SIZE,
        "preprocess_profile": PREPROCESS_PROFILE,
        "input_mean": list(INPUT_MEAN),
        "input_std": list(INPUT_STD),
        "labels": load_labels(),
        "inference": {
            "aggregation": FIVE_CROP_AGGREGATION,
            "human_class": "human",
            "human_threshold": format(human_threshold, ".17g"),
            "decision_rule": "gte",
            "threshold_source": threshold_source,
        },
        "source_checkpoint_sha256": onnx_metadata[
            "source_checkpoint_sha256"
        ],
        "dataset_split_manifest_sha256": onnx_metadata[
            "dataset_split_manifest_sha256"
        ],
        "dataset_split_verified": onnx_metadata.get(
            "dataset_split_verified", "true"
        ) == "true",
        "dataset_split_provenance": onnx_metadata.get(
            "dataset_split_provenance",
            "checkpoint references a content-addressed split manifest",
        ),
        "quantization": {
            "target": target,
            "bits": bits,
            "profile": precision_profile,
            "calibration_algorithm": calibration_algorithm,
            "mixed_int16_operations": list(mixed_int16_operations),
        },
        "source_onnx": onnx_path.name,
        "source_onnx_sha256": file_sha256(onnx_path),
        "espdl_sha256": file_sha256(espdl_path),
    }
    ensure_parent_dir(manifest_path)
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    return manifest_path


def print_confusion_metrics(
    name: str,
    confusion,
    labels: list[str],
    precision_profile: str,
) -> None:
    total = confusion.sum().item()
    correct = confusion.diag().sum().item()
    recalls = [
        confusion[index, index].item() / confusion[index].sum().item()
        for index in range(NUM_CLASSES)
    ]
    print(
        f"PPQ {precision_profile} simulated {name}: "
        f"accuracy={correct / total:.4f} "
        f"({correct}/{total}) balanced_accuracy={sum(recalls) / NUM_CLASSES:.4f}"
    )
    print("Confusion matrix (rows=true, columns=predicted):")
    print(" " * 14 + " ".join(f"{label:>10s}" for label in labels))
    for class_index, label in enumerate(labels):
        values = " ".join(
            f"{confusion[class_index, predicted_index].item():10d}"
            for predicted_index in range(NUM_CLASSES)
        )
        print(f"{label:>12s}: {values}")
    for class_index, label in enumerate(labels):
        support = confusion[class_index].sum().item()
        predicted_count = confusion[:, class_index].sum().item()
        true_positive = confusion[class_index, class_index].item()
        recall = recalls[class_index]
        precision = (
            true_positive / predicted_count
            if predicted_count
            else 0.0
        )
        f1_score = (
            2.0 * precision * recall / (precision + recall)
            if precision + recall
            else 0.0
        )
        print(
            f"  {label}: precision={precision:.4f} recall={recall:.4f} "
            f"f1={f1_score:.4f} support={support}"
        )


def confusion_from_human_scores(
    scores: list[float],
    targets: list[int],
    human_index: int,
    threshold: float,
):
    import torch

    confusion = torch.zeros(
        (NUM_CLASSES, NUM_CLASSES),
        dtype=torch.int64,
    )
    no_human_index = 1 - human_index
    for score, target in zip(scores, targets):
        prediction = human_index if score >= threshold else no_human_index
        confusion[target, prediction] += 1
    return confusion


def calibrate_human_threshold(
    scores: list[float],
    targets: list[int],
    human_index: int,
) -> tuple[float, object]:
    import numpy as np

    unique_scores = np.unique(np.asarray(scores, dtype=np.float32))
    candidate_values = {
        np.float32(0.0),
        np.float32(1.0),
        *unique_scores,
    }
    candidate_values.update(
        np.float32((float(left) + float(right)) / 2.0)
        for left, right in zip(unique_scores, unique_scores[1:])
    )
    candidates = sorted(float(value) for value in candidate_values)

    best_threshold = 0.5
    best_confusion = None
    best_key = None
    for threshold in candidates:
        confusion = confusion_from_human_scores(
            scores,
            targets,
            human_index,
            threshold,
        )
        recalls = [
            confusion[index, index].item()
            / confusion[index].sum().item()
            for index in range(NUM_CLASSES)
        ]
        balanced_accuracy = sum(recalls) / NUM_CLASSES
        accuracy = confusion.diag().sum().item() / confusion.sum().item()
        score_margin = min(abs(threshold - float(score)) for score in unique_scores)
        key = (
            balanced_accuracy,
            accuracy,
            score_margin,
            -abs(threshold - 0.5),
        )
        if best_key is None or key > best_key:
            best_key = key
            best_threshold = threshold
            best_confusion = confusion
    assert best_confusion is not None
    # Every candidate is already an exactly representable float32 value. The
    # margin tie-break selects the most stable point between observed scores.
    # Keep the round-trip value so Python and C++ make the same >= decision.
    return best_threshold, best_confusion


def evaluate_quantized_graph(
    graph,
    executor_type,
    args,
    split_name: str,
    decision_threshold: float | None = None,
) -> float | None:
    import numpy as np
    import onnxruntime as ort
    import torch
    from PIL import Image

    labels = load_labels()
    split_root = args.split_manifest.parent
    evaluation_dir = split_root / split_name
    dataset_split = validate_finetune_split(
        split_root,
        args.split_manifest,
        labels,
    )
    onnx_metadata = validate_onnx_metadata(args.onnx)
    if (
        onnx_metadata["dataset_split_manifest_sha256"]
        != dataset_split["manifest_sha256"]
    ):
        if not args.allow_unverified_split or onnx_metadata.get(
            "dataset_split_verified", "true"
        ) != "false":
            raise ValueError("ONNX model and test split manifest do not match.")
        print(
            "WARNING: checkpoint split provenance is unverified; evaluation "
            "against the repository split is diagnostic only."
        )

    dataset = FixedOrderImageDataset(evaluation_dir, labels, transform=None)
    transform = build_transforms(train=False)
    executor = executor_type(graph=graph, device=args.device)
    output_names = list(graph.outputs)
    if len(output_names) != 1:
        raise ValueError(
            f"Expected one quantized graph output, found {output_names}."
        )

    human_index = labels.index("human")
    single_confusion = torch.zeros(
        (NUM_CLASSES, NUM_CLASSES),
        dtype=torch.int64,
    )
    five_crop_confusion = torch.zeros_like(single_confusion)
    quantized_logits: list[torch.Tensor] = []
    reference_logits: list[torch.Tensor] = []
    five_crop_human_scores: list[float] = []
    evaluation_targets: list[int] = []
    reference_session = ort.InferenceSession(
        str(args.onnx),
        providers=["CPUExecutionProvider"],
    )
    reference_input_name = reference_session.get_inputs()[0].name

    def infer(tensor):
        outputs = executor.forward(
            tensor.unsqueeze(0).to(args.device),
            output_names=output_names,
        )
        if len(outputs) != 1:
            raise RuntimeError(
                f"Quantized executor returned {len(outputs)} outputs."
            )
        return outputs[0].detach().cpu()

    with torch.inference_mode():
        for image_path, target in dataset.samples:
            with Image.open(image_path) as source_image:
                image = source_image.convert("RGB")
                full_tensor = transform(image)
                full_logits = infer(full_tensor)
                fp32_logits = torch.from_numpy(
                    reference_session.run(
                        None,
                        {
                            reference_input_name: np.ascontiguousarray(
                                full_tensor.unsqueeze(0).numpy()
                            )
                        },
                    )[0]
                )
                crop_logits = torch.cat(
                    [
                        infer(transform(crop))
                        for _, crop in make_five_crops(image)
                    ],
                    dim=0,
                )

            single_prediction = full_logits.argmax(dim=1).item()
            quantized_logits.append(full_logits)
            reference_logits.append(fp32_logits)
            crop_probabilities = torch.softmax(crop_logits, dim=1)
            selected_crop = crop_probabilities[
                :, human_index
            ].argmax().item()
            five_crop_human_scores.append(
                crop_probabilities[selected_crop, human_index].item()
            )
            evaluation_targets.append(target)
            five_crop_prediction = crop_logits[
                selected_crop
            ].argmax().item()
            single_confusion[target, single_prediction] += 1
            five_crop_confusion[target, five_crop_prediction] += 1

    print_confusion_metrics(
        f"single-image {split_name}",
        single_confusion,
        labels,
        (
            "mixed INT8/INT16"
            if args.bits == 8 and not args.pure_int8
            else f"INT{args.bits}"
        ),
    )
    print_confusion_metrics(
        f"five-crop {split_name} at threshold 0.5",
        five_crop_confusion,
        labels,
        (
            "mixed INT8/INT16"
            if args.bits == 8 and not args.pure_int8
            else f"INT{args.bits}"
        ),
    )
    quantized_values = torch.cat(quantized_logits, dim=0)
    reference_values = torch.cat(reference_logits, dim=0)
    print(
        "PPQ/ONNX logit diagnostics: "
        f"mae={(quantized_values - reference_values).abs().mean().item():.6f}, "
        f"ppq_range=[{quantized_values.min().item():.4f}, "
        f"{quantized_values.max().item():.4f}], "
        f"onnx_range=[{reference_values.min().item():.4f}, "
        f"{reference_values.max().item():.4f}]"
    )
    for class_index, label in enumerate(labels):
        targets = torch.tensor(dataset.targets)
        mask = targets == class_index
        print(
            f"  true={label}: ppq_mean="
            f"{quantized_values[mask].mean(dim=0).tolist()} "
            f"onnx_mean={reference_values[mask].mean(dim=0).tolist()}"
        )

    if decision_threshold is None and split_name == "val":
        decision_threshold, threshold_confusion = calibrate_human_threshold(
            five_crop_human_scores,
            evaluation_targets,
            human_index,
        )
        print(
            f"Calibrated five-crop human threshold from val: "
            f"{decision_threshold:.8f} "
            f"(float32 round-trip={decision_threshold:.17g})"
        )
        print_confusion_metrics(
            f"five-crop val at calibrated threshold {decision_threshold:.8f}",
            threshold_confusion,
            labels,
            (
                "mixed INT8/INT16"
                if args.bits == 8 and not args.pure_int8
                else f"INT{args.bits}"
            ),
        )
    elif decision_threshold is not None:
        threshold_confusion = confusion_from_human_scores(
            five_crop_human_scores,
            evaluation_targets,
            human_index,
            decision_threshold,
        )
        print_confusion_metrics(
            f"five-crop {split_name} at threshold {decision_threshold:.8f}",
            threshold_confusion,
            labels,
            (
                "mixed INT8/INT16"
                if args.bits == 8 and not args.pure_int8
                else f"INT{args.bits}"
            ),
        )
    return decision_threshold


def main() -> None:
    args = parse_args()
    onnx_metadata = validate_onnx_metadata(args.onnx)
    labels = load_labels()
    split_root = args.split_manifest.parent
    expected_calib_dir = (split_root / "calib").resolve()
    if args.calib_dir.resolve() != expected_calib_dir:
        raise ValueError(
            f"--calib-dir must be {expected_calib_dir} for "
            f"--split-manifest {args.split_manifest}."
        )
    dataset_split = validate_finetune_split(
        split_root,
        args.split_manifest,
        labels,
    )
    validate_calibration_set(split_root, args.split_manifest, labels)
    if (
        onnx_metadata["dataset_split_manifest_sha256"]
        != dataset_split["manifest_sha256"]
    ):
        if not args.allow_unverified_split or onnx_metadata.get(
            "dataset_split_verified", "true"
        ) != "false":
            raise ValueError(
                "ONNX model and calibration split manifest do not match. "
                "Pass --allow-unverified-split only for a checkpoint explicitly "
                "marked with unverified split provenance."
            )
        print(
            "WARNING: checkpoint split provenance is unverified; using the "
            "repository calibration split for quantization."
        )
    (
        QuantizationSettingFactory,
        espdl_quantize_onnx,
        TorchExecutor,
        TargetPlatform,
    ) = import_ppq()

    if args.batch_size != 1:
        raise ValueError("This quantization flow expects a fixed-shape ONNX input of [1, 3, 224, 224], so --batch-size must be 1.")
    if args.pure_int8 and args.bits != 8:
        raise ValueError("--pure-int8 is only valid with --bits 8.")
    if args.human_threshold is not None and not (
        0.0 <= args.human_threshold <= 1.0
    ):
        raise ValueError("--human-threshold must be between 0 and 1.")

    import torch
    from PIL import Image
    from torch.utils.data import DataLoader, Dataset

    image_paths = collect_balanced_calibration_images(args.calib_dir, args.seed)
    if not image_paths:
        raise RuntimeError(f"No calibration images found under {args.calib_dir}")

    transform = build_transforms(train=False)

    class CalibrationImageDataset(Dataset):
        def __init__(self, paths: list[Path]):
            self._paths = paths

        def __len__(self) -> int:
            return len(self._paths)

        def __getitem__(self, index: int):
            with Image.open(self._paths[index]) as image:
                return transform(image.convert("RGB"))

    dataset = CalibrationImageDataset(image_paths)
    dataloader = DataLoader(
        dataset,
        batch_size=args.batch_size,
        shuffle=False,
        num_workers=args.workers,
        pin_memory=False,
    )
    calib_steps = min(args.calib_steps, len(dataloader))
    if calib_steps == 0:
        raise RuntimeError("Calibration DataLoader produced zero batches.")

    quant_setting = QuantizationSettingFactory.espdl_setting()
    quant_setting.quantize_activation_setting.calib_algorithm = (
        args.calibration_algorithm
    )
    mixed_int16_operations = (
        MIXED_INT16_OPERATIONS
        if args.bits == 8 and not args.pure_int8
        else ()
    )
    precision_profile = (
        "mixed_int8_int16"
        if mixed_int16_operations
        else f"int{args.bits}"
    )
    for operation_name in mixed_int16_operations:
        quant_setting.dispatching_table.append(
            operation=operation_name,
            platform=TargetPlatform.ESPDL_INT16,
        )
    ensure_parent_dir(args.output)
    quantized_graph = espdl_quantize_onnx(
        onnx_import_file=str(args.onnx),
        espdl_export_file=str(args.output),
        calib_dataloader=dataloader,
        calib_steps=calib_steps,
        input_shape=[1, 3, IMAGE_SIZE, IMAGE_SIZE],
        target=args.target,
        num_of_bits=args.bits,
        collate_fn=lambda batch: batch.to(args.device),
        setting=quant_setting,
        device=args.device,
        error_report=args.error_report,
        skip_export=False,
        export_test_values=not args.skip_export_test_values,
        verbose=args.verbose,
    )

    print(f"Exported ESP-DL model to {args.output}")

    human_threshold = (
        args.human_threshold
        if args.human_threshold is not None
        else FIVE_CROP_HUMAN_THRESHOLD
    )
    threshold_source = (
        "command_line_override"
        if args.human_threshold is not None
        else "validation_balanced_accuracy"
    )
    if args.evaluate_split is not None:
        if args.evaluate_split == "both":
            if args.human_threshold is None:
                calibrated_threshold = evaluate_quantized_graph(
                    quantized_graph,
                    TorchExecutor,
                    args,
                    "val",
                )
                if calibrated_threshold is None:
                    raise RuntimeError(
                        "Validation evaluation did not produce a human threshold."
                    )
                human_threshold = calibrated_threshold
            else:
                evaluate_quantized_graph(
                    quantized_graph,
                    TorchExecutor,
                    args,
                    "val",
                    human_threshold,
                )
            evaluate_quantized_graph(
                quantized_graph,
                TorchExecutor,
                args,
                "test",
                human_threshold,
            )
        elif args.evaluate_split == "val" and args.human_threshold is None:
            calibrated_threshold = evaluate_quantized_graph(
                quantized_graph,
                TorchExecutor,
                args,
                "val",
            )
            if calibrated_threshold is None:
                raise RuntimeError(
                    "Validation evaluation did not produce a human threshold."
                )
            human_threshold = calibrated_threshold
        else:
            evaluate_quantized_graph(
                quantized_graph,
                TorchExecutor,
                args,
                args.evaluate_split,
                human_threshold,
            )

    manifest_path = write_espdl_manifest(
        args.output,
        args.onnx,
        target=args.target,
        bits=args.bits,
        precision_profile=precision_profile,
        calibration_algorithm=args.calibration_algorithm,
        mixed_int16_operations=mixed_int16_operations,
        human_threshold=human_threshold,
        threshold_source=threshold_source,
    )
    print(
        f"Wrote compatibility manifest to {manifest_path} "
        f"(five-crop human threshold={human_threshold:.8f}, "
        f"float32 round-trip={human_threshold:.17g})"
    )


if __name__ == "__main__":
    main()
