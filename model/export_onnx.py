from __future__ import annotations

import argparse
import json
from pathlib import Path

from model_utils import (
    CHECKPOINT_PATH,
    IMAGE_SIZE,
    INPUT_MEAN,
    INPUT_STD,
    MODEL_ARCHITECTURE,
    MODEL_WIDTH_MULT,
    NUM_CLASSES,
    ONNX_PATH,
    PREPROCESS_PROFILE,
    build_model,
    checkpoint_split_provenance,
    checkpoint_state_dict,
    ensure_parent_dir,
    file_sha256,
    load_labels,
    validate_checkpoint_compatibility,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Export the best MobileNetV2 0.35-width checkpoint to a fixed-shape ONNX graph.")
    parser.add_argument("--checkpoint", type=Path, default=CHECKPOINT_PATH, help="Checkpoint path from train_model.py.")
    parser.add_argument("--output", type=Path, default=ONNX_PATH, help="Output ONNX file.")
    parser.add_argument("--opset", type=int, default=18, help="ONNX opset version.")
    parser.add_argument("--device", default="cpu", help='Torch device string. Use "cpu" unless you know you need another target.')
    parser.add_argument("--skip-verify", action="store_true", help="Skip ONNX shape verification after export.")
    parser.add_argument(
        "--allow-unverified-split",
        action="store_true",
        help=(
            "Export a checkpoint whose training split is not content-addressed. "
            "The ONNX metadata will mark the split as unverified."
        ),
    )
    return parser.parse_args()


def verify_onnx_shape(onnx_path: Path) -> None:
    import onnx

    model = onnx.load(str(onnx_path))
    input_tensor = model.graph.input[0]
    dims = [dim.dim_value for dim in input_tensor.type.tensor_type.shape.dim]
    if dims != [1, 3, IMAGE_SIZE, IMAGE_SIZE]:
        raise ValueError(f"Unexpected input shape {dims}; expected [1, 3, {IMAGE_SIZE}, {IMAGE_SIZE}].")


def add_onnx_metadata(
    onnx_path: Path,
    labels: list[str],
    checkpoint_path: Path,
    split_provenance: dict[str, object],
) -> None:
    import onnx

    model = onnx.load(str(onnx_path))
    metadata = {
        "architecture": MODEL_ARCHITECTURE,
        "width_mult": str(MODEL_WIDTH_MULT),
        "image_size": str(IMAGE_SIZE),
        "preprocess_profile": PREPROCESS_PROFILE,
        "input_mean": json.dumps(list(INPUT_MEAN)),
        "input_std": json.dumps(list(INPUT_STD)),
        "labels": json.dumps(labels),
        "source_checkpoint_sha256": file_sha256(checkpoint_path),
        "dataset_split_manifest_sha256": str(split_provenance["digest"]),
        "dataset_split_verified": "true" if split_provenance["verified"] else "false",
        "dataset_split_provenance": str(split_provenance["description"]),
    }
    del model.metadata_props[:]
    for key, value in metadata.items():
        entry = model.metadata_props.add()
        entry.key = key
        entry.value = value
    onnx.save(model, str(onnx_path))


def main() -> None:
    args = parse_args()

    import torch

    labels = load_labels()
    checkpoint = torch.load(
        args.checkpoint,
        map_location="cpu",
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
    split_provenance = checkpoint_split_provenance(
        checkpoint,
        args.checkpoint,
        allow_unverified=args.allow_unverified_split,
    )

    model = build_model(num_classes=NUM_CLASSES, pretrained=False)
    try:
        model.load_state_dict(checkpoint_state_dict(checkpoint))
    except RuntimeError as exc:
        raise RuntimeError(
            f"Failed to load checkpoint into MobileNetV2 width_mult={MODEL_WIDTH_MULT}. "
            "Retrain the classifier before exporting; older checkpoints may be width_mult=1.0."
        ) from exc
    model.eval()
    model.to(torch.device(args.device))

    dummy_input = torch.randn(1, 3, IMAGE_SIZE, IMAGE_SIZE, device=torch.device(args.device))
    ensure_parent_dir(args.output)
    torch.onnx.export(
        model,
        dummy_input,
        str(args.output),
        export_params=True,
        do_constant_folding=True,
        opset_version=args.opset,
        input_names=["input"],
        output_names=["logits"],
        dynamic_axes=None,
        dynamo=False,
    )
    add_onnx_metadata(
        args.output,
        labels,
        args.checkpoint,
        split_provenance,
    )

    if not args.skip_verify:
        verify_onnx_shape(args.output)

    print(f"Exported ONNX model to {args.output}")


if __name__ == "__main__":
    main()
