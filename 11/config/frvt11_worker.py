#!/usr/bin/env python3
"""Python inference worker for the FRVT 1:1 C++ bridge.

The protocol on stdout/stdin is binary. All logs must go to stderr.
"""

import argparse
import contextlib
import io
import logging
import os
import struct
import sys
import traceback


READY = b"FRVTPY1\n"
REQ_MAGIC = b"FRQ1"
RSP_MAGIC = b"FRS1"

CMD_CREATE_SINGLE_PERSON = 1
CMD_CREATE_MULTI_PERSON = 2
CMD_QUIT = 99

STATUS_OK = 0
STATUS_ERROR = 1

TEMPLATE_OK = 0
TEMPLATE_FAILED = 1


def read_exact(stream, size):
    data = stream.read(size)
    if data == b"" and size > 0:
        return None
    if data is None or len(data) != size:
        raise EOFError("unexpected EOF while reading worker request")
    return data


def write_response(templates, eyes, status=STATUS_OK, message=""):
    message_bytes = message.encode("utf-8", errors="replace")
    out = bytearray()
    out += RSP_MAGIC
    out += struct.pack("<IIII", status, len(templates), len(eyes), len(message_bytes))
    out += message_bytes

    for template_status, embedding in templates:
        dim = 0 if embedding is None else int(len(embedding))
        out += struct.pack("<II", int(template_status), dim)
        if dim:
            out += struct.pack("<%sf" % dim, *[float(v) for v in embedding])

    for left_ok, right_ok, xleft, yleft, xright, yright in eyes:
        out += struct.pack(
            "<IIffff",
            1 if left_ok else 0,
            1 if right_ok else 0,
            float(xleft),
            float(yleft),
            float(xright),
            float(yright),
        )

    sys.stdout.buffer.write(out)
    sys.stdout.buffer.flush()


class FaceRuntime:
    def __init__(self, config_dir, strategy):
        self.config_dir = config_dir
        self.strategy = strategy
        self.loaded = False

    def load(self):
        if self.loaded:
            return

        os.environ.setdefault("OMP_NUM_THREADS", "1")
        os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
        os.environ.setdefault("MKL_NUM_THREADS", "1")
        os.environ.setdefault("NUMEXPR_NUM_THREADS", "1")
        os.environ.setdefault("TF_NUM_INTRAOP_THREADS", "1")
        os.environ.setdefault("TF_NUM_INTEROP_THREADS", "1")
        os.environ.setdefault("ORT_INTRA_OP_NUM_THREADS", "1")
        os.environ.setdefault("ORT_INTER_OP_NUM_THREADS", "1")
        os.environ.setdefault("TF_DETERMINISTIC_OPS", "1")

        for path in (
            self.config_dir,
            os.path.join(self.config_dir, "python"),
            os.path.join(self.config_dir, "python", "site-packages"),
        ):
            if os.path.isdir(path) and path not in sys.path:
                sys.path.insert(0, path)

        import cv2
        import numpy as np
        import onnxruntime as ort
        import tensorflow as tf
        from insightface.model_zoo.scrfd import SCRFD
        from insightface.utils.face_align import norm_crop
        from resnet100 import build_adaface

        self.cv2 = cv2
        self.np = np
        self.tf = tf
        self.norm_crop = norm_crop

        try:
            tf.config.threading.set_intra_op_parallelism_threads(1)
            tf.config.threading.set_inter_op_parallelism_threads(1)
        except RuntimeError:
            pass

        scrfd_path = os.path.join(self.config_dir, "scrfd.onnx")
        if not os.path.exists(scrfd_path):
            raise FileNotFoundError(scrfd_path)

        requested = ["CPUExecutionProvider"]
        if os.getenv("FRVT_USE_CUDA", "0").strip().lower() in {"1", "true", "yes"}:
            requested = ["CUDAExecutionProvider", "CPUExecutionProvider"]
        available = set(ort.get_available_providers())
        providers = [provider for provider in requested if provider in available]
        if not providers:
            providers = ["CPUExecutionProvider"]

        ort.set_default_logger_severity(3)
        session = ort.InferenceSession(scrfd_path, providers=providers)
        detector = SCRFD(scrfd_path)
        detector.session = session
        ctx_id = 0 if "CUDAExecutionProvider" in session.get_providers() else -1
        detector.prepare(ctx_id, input_size=(320, 320))
        self.detector = detector

        weight_path = os.path.join(self.config_dir, "r100_AdaFace_glint360k.h5")
        if not os.path.exists(weight_path):
            raise FileNotFoundError(weight_path)

        # resnet100.py contains diagnostic print calls in model construction paths.
        # Suppress stdout so the binary protocol cannot be corrupted.
        with contextlib.redirect_stdout(io.StringIO()):
            model = build_adaface()
        model.trainable = False
        self.model = model
        self.loaded = True
        logging.info("Python face runtime loaded with ONNX providers: %s", session.get_providers())

    def image_from_request(self, width, height, depth, data):
        self.load()
        np = self.np
        cv2 = self.cv2

        if width <= 0 or height <= 0:
            raise ValueError("invalid image dimensions")

        if depth == 24:
            expected = width * height * 3
            if len(data) != expected:
                raise ValueError("unexpected RGB image byte count")
            return np.frombuffer(data, dtype=np.uint8).reshape((height, width, 3)).copy()

        if depth == 8:
            expected = width * height
            if len(data) != expected:
                raise ValueError("unexpected grayscale image byte count")
            gray = np.frombuffer(data, dtype=np.uint8).reshape((height, width)).copy()
            return cv2.cvtColor(gray, cv2.COLOR_GRAY2RGB)

        raise ValueError("unsupported FRVT image depth: %s" % depth)

    def detect_faces(self, image_rgb):
        self.load()
        bboxes, kpss = self.detector.detect(image_rgb)
        if bboxes is None or len(bboxes) == 0:
            return []

        np = self.np
        areas = (bboxes[:, 2] - bboxes[:, 0]) * (bboxes[:, 3] - bboxes[:, 1])
        order = np.argsort(-areas, kind="mergesort")
        detections = []
        for idx in order:
            bbox = bboxes[idx]
            kps = kpss[idx].reshape(5, 2)
            score = float(bbox[4]) if bboxes.shape[1] >= 5 else 1.0
            detections.append((bbox, kps, score))
        return detections

    def align_face(self, image_rgb, kps):
        face = self.norm_crop(image_rgb, kps.reshape(5, 2))
        face = self.cv2.resize(face, (112, 112))
        return face.astype(self.np.float32) / 127.5 - 1.0

    def eye_from_kps(self, kps, width, height):
        def point_ok(point):
            x, y = float(point[0]), float(point[1])
            return self.np.isfinite(x) and self.np.isfinite(y) and 0 <= x < width and 0 <= y < height

        left = kps[0]
        right = kps[1]
        left_ok = bool(point_ok(left))
        right_ok = bool(point_ok(right))
        return (
            left_ok,
            right_ok,
            float(left[0]) if left_ok else 0.0,
            float(left[1]) if left_ok else 0.0,
            float(right[0]) if right_ok else 0.0,
            float(right[1]) if right_ok else 0.0,
        )

    def embed_faces(self, aligned_faces):
        self.load()
        if not aligned_faces:
            return []
        batch = self.np.stack(aligned_faces).astype(self.np.float32)
        emb = self.model(batch, training=False)
        emb = self.tf.linalg.l2_normalize(emb, axis=1).numpy().astype(self.np.float32)
        return [row for row in emb]

    def aggregate(self, embeddings, qualities):
        np = self.np
        if not embeddings:
            return None

        embs = np.asarray(embeddings, dtype=np.float32)
        if self.strategy == "score_weighted":
            weights = np.asarray(qualities, dtype=np.float32)
        else:
            weights = np.ones((len(embeddings),), dtype=np.float32)

        weight_sum = float(weights.sum())
        if weight_sum <= 0.0:
            return None

        template = (embs * weights[:, None]).sum(axis=0) / max(weight_sum, 1.0e-12)
        norm = float(np.linalg.norm(template))
        if norm <= 0.0:
            return None
        return (template / norm).astype(np.float32)

    def create_single_person(self, images):
        aligned_faces = []
        qualities = []
        eyes = []

        for width, height, depth, data in images:
            image_rgb = self.image_from_request(width, height, depth, data)
            detections = self.detect_faces(image_rgb)
            if not detections:
                eyes.append((False, False, 0.0, 0.0, 0.0, 0.0))
                continue

            _bbox, kps, score = detections[0]
            aligned_faces.append(self.align_face(image_rgb, kps))
            qualities.append(float(score))
            eyes.append(self.eye_from_kps(kps, width, height))

        embeddings = self.embed_faces(aligned_faces)
        template = self.aggregate(embeddings, qualities)
        if template is None:
            return [(TEMPLATE_FAILED, None)], eyes

        return [(TEMPLATE_OK, template)], eyes

    def create_multi_person(self, images):
        if len(images) != 1:
            raise ValueError("multi-person request expects exactly one image")

        width, height, depth, data = images[0]
        image_rgb = self.image_from_request(width, height, depth, data)
        detections = self.detect_faces(image_rgb)
        if not detections:
            return [(TEMPLATE_FAILED, None)], [(False, False, 0.0, 0.0, 0.0, 0.0)]

        aligned_faces = [self.align_face(image_rgb, kps) for _bbox, kps, _score in detections]
        embeddings = self.embed_faces(aligned_faces)
        templates = [(TEMPLATE_OK, emb) for emb in embeddings]
        eyes = [self.eye_from_kps(kps, width, height) for _bbox, kps, _score in detections]
        return templates, eyes


def read_request():
    header = read_exact(sys.stdin.buffer, 12)
    if header is None:
        return None

    magic, command, image_count = struct.unpack("<4sII", header)
    if magic != REQ_MAGIC:
        raise ValueError("invalid request magic")
    if image_count > 64:
        raise ValueError("too many images in request")

    images = []
    for _ in range(image_count):
        image_header = read_exact(sys.stdin.buffer, 20)
        width, height, depth, byte_count = struct.unpack("<IIIQ", image_header)
        if byte_count > 1024 * 1024 * 1024:
            raise ValueError("image payload is unreasonably large")
        data = read_exact(sys.stdin.buffer, byte_count) if byte_count else b""
        images.append((int(width), int(height), int(depth), data))

    return command, images


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True)
    args = parser.parse_args()

    logging.basicConfig(
        level=os.getenv("FRVT_WORKER_LOG_LEVEL", "WARNING").upper(),
        stream=sys.stderr,
        format="%(asctime)s %(levelname)s [frvt11_worker] %(message)s",
    )

    config_dir = os.path.abspath(args.config)
    strategy = os.getenv("FRVT_AGGREGATION_STRATEGY", "score_weighted").strip().lower()
    if strategy not in {"mean", "score_weighted"}:
        strategy = "score_weighted"

    runtime = FaceRuntime(config_dir, strategy)
    sys.stdout.buffer.write(READY)
    sys.stdout.buffer.flush()

    while True:
        try:
            request = read_request()
            if request is None:
                return

            command, images = request
            if command == CMD_QUIT:
                return
            if command == CMD_CREATE_SINGLE_PERSON:
                templates, eyes = runtime.create_single_person(images)
                write_response(templates, eyes)
            elif command == CMD_CREATE_MULTI_PERSON:
                templates, eyes = runtime.create_multi_person(images)
                write_response(templates, eyes)
            else:
                write_response([], [], status=STATUS_ERROR, message="unknown command: %s" % command)
        except Exception as exc:
            logging.error("worker request failed: %s\n%s", exc, traceback.format_exc())
            write_response([], [], status=STATUS_ERROR, message=str(exc))


if __name__ == "__main__":
    main()
