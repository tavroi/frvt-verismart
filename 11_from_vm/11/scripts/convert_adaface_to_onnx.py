#!/usr/bin/env python3
"""Convert r100_AdaFace_glint360k.h5 to ONNX and verify numerical fidelity.

Run from the 11/ directory:
    python scripts/convert_adaface_to_onnx.py

Requirements (install in a throwaway venv — not the submission runtime):
    pip install "tensorflow-cpu>=2.12,<2.16" "tf2onnx>=1.16" "onnxruntime>=1.17" numpy

Output:
    config/r100_AdaFace_glint360k.onnx
"""

import argparse
import os
import sys

# Suppress TF/CUDA noise before importing tensorflow
os.environ.setdefault("CUDA_VISIBLE_DEVICES", "-1")
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")
os.environ.setdefault("TF_ENABLE_ONEDNN_OPTS", "0")

import numpy as np

# ---------------------------------------------------------------------------
# Locate config/ relative to this script so it works from any CWD.
# ---------------------------------------------------------------------------
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(SCRIPT_DIR)
CONFIG_DIR = os.path.join(ROOT_DIR, "config")

if not os.path.isdir(CONFIG_DIR):
    sys.exit(f"[ERROR] config/ not found at {CONFIG_DIR} — run from the 11/ root or check path")

sys.path.insert(0, CONFIG_DIR)

H5_PATH = os.path.join(CONFIG_DIR, "r100_AdaFace_glint360k.h5")
ONNX_PATH = os.path.join(CONFIG_DIR, "r100_AdaFace_glint360k.onnx")


def load_tf_model():
    import tensorflow as tf  # noqa: import-outside-toplevel

    # Suppress any remaining Keras chatter on stdout
    import contextlib, io  # noqa: import-outside-toplevel
    from resnet100 import build_adaface  # noqa: import-outside-toplevel

    print("[1/4] Building AdaFace ResNet100 model and loading weights ...")
    with contextlib.redirect_stdout(io.StringIO()):
        model = build_adaface()
    model.trainable = False
    print(f"      Input  : {model.input.shape}  name={model.input.name}")
    print(f"      Output : {model.output.shape}  name={model.output.name}")
    return model


def convert(model):
    import tensorflow as tf  # noqa: import-outside-toplevel
    import tf2onnx  # noqa: import-outside-toplevel

    print("[2/4] Converting to ONNX (opset 17, dynamic batch) ...")

    input_signature = [
        tf.TensorSpec(shape=(None, 112, 112, 3), dtype=tf.float32, name="input_1")
    ]

    model_proto, _ = tf2onnx.convert.from_keras(
        model,
        input_signature=input_signature,
        opset=17,
        output_path=ONNX_PATH,
    )

    size_mb = os.path.getsize(ONNX_PATH) / (1024 * 1024)
    print(f"      Saved  : {ONNX_PATH}  ({size_mb:.1f} MB)")

    # Print the exact input/output node names — the C++ code needs these.
    graph = model_proto.graph
    print("\n      ONNX graph I/O (copy these for the C++ code):")
    for inp in graph.input:
        print(f"        input  name='{inp.name}'")
    for out in graph.output:
        print(f"        output name='{out.name}'")


def verify():
    import onnxruntime as ort  # noqa: import-outside-toplevel

    print("[3/4] Verifying ONNX output vs TensorFlow on random inputs ...")

    # Load TF model fresh (same weights, inference-only)
    import contextlib, io  # noqa: import-outside-toplevel
    from resnet100 import build_adaface  # noqa: import-outside-toplevel
    import tensorflow as tf  # noqa: import-outside-toplevel

    with contextlib.redirect_stdout(io.StringIO()):
        tf_model = build_adaface()
    tf_model.trainable = False

    sess = ort.InferenceSession(ONNX_PATH, providers=["CPUExecutionProvider"])
    ort_input_name = sess.get_inputs()[0].name
    ort_output_name = sess.get_outputs()[0].name

    np.random.seed(42)
    passed = True
    for trial, batch_size in enumerate([1, 2, 4]):
        x = np.random.randn(batch_size, 112, 112, 3).astype(np.float32)

        tf_out = tf_model(x, training=False).numpy()
        ort_out = sess.run([ort_output_name], {ort_input_name: x})[0]

        max_diff = float(np.max(np.abs(tf_out - ort_out)))
        mean_diff = float(np.mean(np.abs(tf_out - ort_out)))

        status = "OK" if max_diff < 1e-4 else "FAIL"
        if status == "FAIL":
            passed = False
        print(f"      batch={batch_size}  max_abs_diff={max_diff:.2e}  mean_abs_diff={mean_diff:.2e}  [{status}]")

    if not passed:
        sys.exit(
            "\n[ERROR] Numerical fidelity check failed (max diff >= 1e-4).\n"
            "        Check tf2onnx version and PReLU export. Do NOT use this .onnx file."
        )

    print(f"\n      ORT input  name : '{ort_input_name}'")
    print(f"      ORT output name : '{ort_output_name}'")
    return ort_input_name, ort_output_name


def check_l2_norm():
    """Confirm the model outputs are NOT pre-normalized (we L2-normalize in C++)."""
    import onnxruntime as ort  # noqa: import-outside-toplevel

    sess = ort.InferenceSession(ONNX_PATH, providers=["CPUExecutionProvider"])
    inp_name = sess.get_inputs()[0].name
    out_name = sess.get_outputs()[0].name

    np.random.seed(0)
    x = np.random.randn(1, 112, 112, 3).astype(np.float32)
    emb = sess.run([out_name], {inp_name: x})[0][0]
    norm = float(np.linalg.norm(emb))
    print(f"[4/4] Output L2 norm on random face: {norm:.4f}")
    if abs(norm - 1.0) < 0.01:
        print("      Embeddings are pre-normalized by the model.")
        print("      NOTE: C++ matchTemplates still normalizes before dot product — safe either way.")
    else:
        print("      Embeddings are NOT pre-normalized. C++ bridge will L2-normalize before storing.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()

    if not os.path.exists(H5_PATH):
        sys.exit(f"[ERROR] Weights not found: {H5_PATH}")

    model = load_tf_model()
    convert(model)
    inp_name, out_name = verify()
    check_l2_norm()

    print("\n[DONE] Conversion successful.")
    print(f"       ONNX model  : {ONNX_PATH}")
    print(f"       Input name  : '{inp_name}'   <-- use in C++")
    print(f"       Output name : '{out_name}'   <-- use in C++")
    print("\nNext step: copy this output into nullimplfrvt11.cpp constants.")


if __name__ == "__main__":
    main()
