#!/bin/bash
set -euo pipefail

root=$(pwd)

mkdir -p "$root/config" "$root/doc" "$root/lib"

cp -f "$root/src/nullImpl/frvt11_worker.py" "$root/config/frvt11_worker.py"
cp -f "$root/assets/resnet100.py" "$root/config/resnet100.py"
cp -f "$root/assets/scrfd.onnx" "$root/config/scrfd.onnx"
cp -f "$root/assets/r100_AdaFace_glint360k.h5" "$root/config/r100_AdaFace_glint360k.h5"

if [ ! -s "$root/doc/version.txt" ]; then
    cat > "$root/doc/version.txt" <<'EOF'
FRVT 1:1 face submission integration
Implementation: C++ FRVT API bridge with child-local Python inference worker
Recognition model: AdaFace ResNet100, r100_AdaFace_glint360k.h5
Detection model: SCRFD, scrfd.onnx
Template format: FRVT11E1 v1, 512 float32 L2-normalized embedding
EOF
fi

echo "[SUCCESS] Staged config/, lib/, and doc/ submission directories."
