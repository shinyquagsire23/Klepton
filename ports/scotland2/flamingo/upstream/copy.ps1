& cmake -G "Ninja" -DCMAKE_BUILD_TYPE="Debug" -DTEST_BUILD=1 -B build
& cmake --build ./build

# & adb push 