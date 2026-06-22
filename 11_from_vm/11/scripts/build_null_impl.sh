#!/bin/bash

root=$(pwd)
FRVT_PROVIDER=${FRVT_PROVIDER:-nxtgn}
FRVT_SEQUENCE=${FRVT_SEQUENCE:-001}

echo "Attempting to build FRVT 1:1 implementation libfrvt_11_${FRVT_PROVIDER}_${FRVT_SEQUENCE}.so"
cd src/nullImpl
rm -rf build; mkdir -p build; cd build
cmake -DFRVT_PROVIDER="$FRVT_PROVIDER" -DFRVT_SEQUENCE="$FRVT_SEQUENCE" ../ > /dev/null; make
cd $root
