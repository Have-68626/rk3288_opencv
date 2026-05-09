sed -i 's|"src/win/src/JsonLite.cpp"|"src/win/src/JsonLite.cpp"\n        "src/win/src/WinJsonConfig.cpp"|g' CMakeLists.txt
cmake -S . -B build_smoke -DRK_SKIP_OPENCV=ON -DRK_BUILD_CORE_UNIT_TESTS=ON
cmake --build build_smoke --config Release --target core_unit_tests
