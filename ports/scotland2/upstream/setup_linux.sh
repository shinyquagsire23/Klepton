#!/bin/sh
cmake -G "Ninja" -DBUILD_FOR=LINUX -DCMAKE_BUILD_TYPE="Debug" -B build_linux 
cmake --build ./build_linux
