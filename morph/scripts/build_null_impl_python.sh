#!/bin/bash

root=$(pwd)

echo -n "Checking installation of required packages "
for package in python3-pip g++ cmake python3-pybind11 python3-numpy python3-opencv 
do
    if [ $(dpkg-query -W -f='${Status}' $package 2>/dev/null | grep -c "ok installed") -eq 0 ];
then
        sudo apt-get install -y $package;
    fi
done
echo "[SUCCESS]"

echo "Attempting to build null implementation" 
cd src/nullImpl_python
rm -rf build; mkdir -p build; cd build
cmake ../ > /dev/null; make
cd ../
cp morph_detector.py ../../lib
cd $root
