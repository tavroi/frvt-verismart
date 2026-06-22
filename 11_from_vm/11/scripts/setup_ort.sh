#!/bin/bash
# Download and stage ONNX Runtime 1.18.1 headers + library.
# Run once from the 11/ directory on Ubuntu.
set -euo pipefail

ORT_VERSION="1.18.1"
ORT_PKG="onnxruntime-linux-x64-${ORT_VERSION}"
ORT_URL="https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_PKG}.tgz"
TMP=$(mktemp -d)

echo "Downloading ONNX Runtime ${ORT_VERSION} ..."
curl -L "$ORT_URL" -o "$TMP/${ORT_PKG}.tgz"

echo "Extracting ..."
tar -xzf "$TMP/${ORT_PKG}.tgz" -C "$TMP"

echo "Staging headers → include/onnxruntime/"
mkdir -p include/onnxruntime
cp "$TMP/${ORT_PKG}/include/"* include/onnxruntime/

echo "Staging library → lib/"
mkdir -p lib
cp "$TMP/${ORT_PKG}/lib/libonnxruntime.so.${ORT_VERSION}" lib/
ln -sf "libonnxruntime.so.${ORT_VERSION}" lib/libonnxruntime.so

rm -rf "$TMP"
echo "Done. lib/ now contains:"
ls -lh lib/libonnxruntime*
