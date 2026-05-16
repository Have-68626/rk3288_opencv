#!/bin/bash
set -e

# Note: It is highly recommended to modify CMakeLists.txt directly instead of using sed.
# This command adds both WinCrypto and WinJsonConfig to targets containing JsonLite.
sed -i 's|"src/win/src/JsonLite.cpp"|"src/win/src/JsonLite.cpp"\n        "src/win/src/WinCrypto.cpp"\n        "src/win/src/WinJsonConfig.cpp"|g' CMakeLists.txt

cmake -S . -B build_smoke -DRK_SKIP_OPENCV=ON -DRK_BUILD_CORE_UNIT_TESTS=ON
cmake --build build_smoke --config Release --target core_unit_tests
ctest --test-dir build_smoke -C Release --output-on-failure
