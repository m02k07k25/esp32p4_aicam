from __future__ import annotations

import argparse
from pathlib import Path

from model_utils import CHECKPOINT_PATH, IMAGE_SIZE, NUM_CLASSES, ONNX_PATH, build_model, checkpoint_state_dict, ensure_parent_dir, load_labels


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Export the best MobileNetV2 checkpoint to a fixed-shape ONNX graph.")
    parser.add_argument("--checkpoint", type=Path, default=CHECKPOINT_PATH, help="Checkpoint path from train_model.py.")
    parser.add_argument("--output", type=Path, default=ONNX_PATH, help="Output ONNX file.")
    parser.add_argument("--opset", type=int, default=17, help="ONNX opset version.")
    parser.add_argument("--device", default="cpu", help='Torch device string. Use "cpu" unless you know you need another target.')
    parser.add_argument("--skip-verify", action="store_true", help="Skip ONNX shape verification after export.")
    return parser.parse_args()


def verify_onnx_shape(onnx_path: Path) -> None:
    import onnx

    model = onnx.load(str(onnx_path))
    input_tensor = model.graph.input[0]
    dims = [dim.dim_value for dim in input_tensor.type.tensor_type.shape.dim]
    if dims != [1, 3, IMAGE_SIZE, IMAGE_SIZE]:
        raise ValueError(f"Unexpected input shape {dims}; expected [1, 3, {IMAGE_SIZE}, {IMAGE_SIZE}].")


def main() -> None:
    args = parse_args()

    import torch

    labels = load_labels()
    checkpoint = torch.load(args.checkpoint, map_location="cpu")
    checkpoint_labels = checkpoint.get("labels", labels)
    if checkpoint_labels != labels:
        raise ValueError(f"Checkpoint labels {checkpoint_labels} do not match {labels}.")

    model = build_model(num_classes=NUM_CLASSES, pretrained=False)
    model.load_state_dict(checkpoint_state_dict(checkpoint))
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
    )

    if not args.skip_verify:
        verify_onnx_shape(args.output)

    print(f"Exported ONNX model to {args.output}")


if __name__ == "__main__":
    main()
