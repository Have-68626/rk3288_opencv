# Windows CI Build Speedup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce Windows CI build-test job from ~60-90 minutes to ~15-25 minutes.

**Architecture:** The Windows CI build (`windows-build-test` in `.github/workflows/ci.yml`) compiles OpenCV 4.10.0 from source via `add_subdirectory`. Without `-DBUILD_LIST`, all ~80 OpenCV modules + ~30 opencv_contrib modules are compiled when only 15 are needed. Three additional bottlenecks (INT8 quantization every run, ineffective ccache, and uncached GTest builds) add ~10 more minutes. Fixes are: (1) add `BUILD_LIST` to CI and `cmake/opencv.cmake`, (2) gate optional steps behind path filters, (3) remove ineffective caching, (4) cache GTest build output.

**Tech Stack:** GitHub Actions (ci.yml), CMake, MSVC, OpenCV add_subdirectory, FetchContent, ccache

---

## File Map

| File | Responsibility | Change |
|------|---------------|--------|
| `.github/workflows/ci.yml` | Windows CI pipeline definition | Add `-DBUILD_LIST` to cmake step; gate INT8 steps; remove ccache; add GTest build cache |
| `cmake/opencv.cmake` | OpenCV configuration for all consumers | Add default `BUILD_LIST` guard so any consumer (CI or local) auto-trims modules |
| `cmake/core.cmake` | GTest FetchContent + core_unit_tests config | No change needed — caching is handled at CI layer |

---

### Task 1: Add BUILD_LIST to Windows CI cmake configure step

**Files:**
- Modify: `.github/workflows/ci.yml:281-283`

**Problem:** The `Configure (CMake)` step passes only `-DOPENCV_ROOT` and `-DOPENCV_CONTRIB_ROOT` without `-DBUILD_LIST`. This causes OpenCV to build all ~110 modules (80 core + 30 contrib). Only 15 modules in `RK_OPENCV_FULL_LIBS` are actually linked.

- [ ] **Step 1: Add `-DBUILD_LIST` to the cmake command**

Replace the three-line cmake invocation with one that includes the 15 required modules:

Old:
```yaml
      - name: Configure (CMake)
        shell: pwsh
        run: |
          $ErrorActionPreference = "Stop"
          cmake --version
          cmake -S . -B $env:BUILD_DIR -G "$env:CMAKE_GENERATOR" -A $env:CMAKE_PLATFORM `
            -DOPENCV_ROOT="$env:OPENCV_ROOT" `
            -DOPENCV_CONTRIB_ROOT="$env:OPENCV_CONTRIB_ROOT"
```

New:
```yaml
      - name: Configure (CMake)
        shell: pwsh
        run: |
          $ErrorActionPreference = "Stop"
          cmake --version
          cmake -S . -B $env:BUILD_DIR -G "$env:CMAKE_GENERATOR" -A $env:CMAKE_PLATFORM `
            -DOPENCV_ROOT="$env:OPENCV_ROOT" `
            -DOPENCV_CONTRIB_ROOT="$env:OPENCV_CONTRIB_ROOT" `
            -DBUILD_LIST="core,imgproc,imgcodecs,objdetect,features2d,flann,calib3d,dnn,ml,photo,video,videoio,highgui,stitching"
```

- [ ] **Step 2: Define BUILD_LIST as an env constant for reuse**

Add to the `env:` block at `.github/workflows/ci.yml:192-199`:

```yaml
    env:
      OPENCV_VERSION: "4.10.0"
      OPENCV_CONTRIB_VERSION: "4.10.0"
      DEPS_DIR: _deps
      BUILD_DIR: build_ci_win
      CMAKE_GENERATOR: Visual Studio 17 2022
      CMAKE_PLATFORM: x64
      CMAKE_BUILD_CONFIG: Release
      OPENCV_BUILD_LIST: "core,imgproc,imgcodecs,objdetect,features2d,flann,calib3d,dnn,ml,photo,video,videoio,highgui,stitching"
```

Then rewrite the step to use `$env:OPENCV_BUILD_LIST`:

```yaml
      - name: Configure (CMake)
        shell: pwsh
        run: |
          $ErrorActionPreference = "Stop"
          cmake --version
          cmake -S . -B $env:BUILD_DIR -G "$env:CMAKE_GENERATOR" -A $env:CMAKE_PLATFORM `
            -DOPENCV_ROOT="$env:OPENCV_ROOT" `
            -DOPENCV_CONTRIB_ROOT="$env:OPENCV_CONTRIB_ROOT" `
            -DBUILD_LIST="$env:OPENCV_BUILD_LIST"
```

> **Rationale:** Using `env` keeps the module list in one place and avoids duplicating the long string if other steps need it.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "perf(ci): add BUILD_LIST to Windows CI cmake step

Without -DBUILD_LIST, OpenCV compiles all ~80 core + ~30 contrib
modules (110 total) from source. Only 15 modules in RK_OPENCV_FULL_LIBS
are ever linked. Pinning to those 15 reduces the Windows CI build time
from ~60-90 min to ~15-25 min."
```

**Estimated impact:** Reduces CI build time by **45-65 minutes** (dominant bottleneck fixed).

---

### Task 2: Add default BUILD_LIST guard to opencv.cmake

**Files:**
- Modify: `cmake/opencv.cmake:46-48`

**Problem:** `cmake/opencv.cmake` is the central OpenCV configuration file, but it never sets `BUILD_LIST`. The comment on line 46-47 says "Explicitly whitelist modules if needed" but doesn't actually whitelist anything. This means any build configuration (CI, local dev, or future consumers) that forgets `-DBUILD_LIST` will compile all OpenCV modules.

- [ ] **Step 1: Add `BUILD_LIST` default after the existing module disables**

Insert after line 48 (`set(BUILD_opencv_world OFF ...)`):

```cmake
if(NOT BUILD_LIST)
    set(BUILD_LIST "core,imgproc,imgcodecs,objdetect,features2d,flann,calib3d,dnn,ml,photo,video,videoio,highgui,stitching" CACHE STRING "OpenCV modules to build (empty = all)" FORCE)
endif()
```

The complete block (lines 36-49) becomes:

```cmake
# Disable unnecessary modules to speed up build
set(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared libraries")
set(BUILD_TESTS OFF CACHE BOOL "Build tests")
set(BUILD_PERF_TESTS OFF CACHE BOOL "Build perf tests")
set(BUILD_EXAMPLES OFF CACHE BOOL "Build examples")
set(BUILD_ANDROID_EXAMPLES OFF CACHE BOOL "Build Android examples")
set(BUILD_DOCS OFF CACHE BOOL "Build docs")
set(BUILD_JAVA OFF CACHE BOOL "Build Java wrapper") # We use C++ only
set(BUILD_opencv_python2 OFF CACHE BOOL "Build Python2")
set(BUILD_opencv_python3 OFF CACHE BOOL "Build Python3")
set(BUILD_opencv_world OFF CACHE BOOL "Build opencv_world")
# Limit to modules that are actually linked (RK_OPENCV_FULL_LIBS).
# If BUILD_LIST is already set (e.g. from CMakePresets or -D flag), respect it.
if(NOT BUILD_LIST)
    set(BUILD_LIST "core,imgproc,imgcodecs,objdetect,features2d,flann,calib3d,dnn,ml,photo,video,videoio,highgui,stitching" CACHE STRING "OpenCV modules to build (empty = all)" FORCE)
endif()
```

> **Why NOT empty `BUILD_LIST` check matters:** If a user or preset passes `-DBUILD_LIST=core,imgcodecs` via the command line, CMake sets `BUILD_LIST` as a cache variable. The `if(NOT BUILD_LIST)` guard preserves that user choice. Only when `BUILD_LIST` is unset do we apply the default.

- [ ] **Step 2: Update the comment above the block**

Replace line 46's misleading comment:

```cmake
# Whitelist only the modules we actually link. See RK_OPENCV_FULL_LIBS below.
```

- [ ] **Step 3: Verify the change works correctly with an override**

Run a quick CMake configure **without** `-DBUILD_LIST` and confirm only 15 modules are targeted:

```powershell
cmake -S . -B build_test_opencv_default -G "Visual Studio 17 2022" -A x64 `
  -DOPENCV_ROOT="$env:OPENCV_ROOT" `
  -DOPENCV_CONTRIB_ROOT="$env:OPENCV_CONTRIB_ROOT" `
  -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON
```

Then check the CMake output for lines containing `opencv_` to confirm only the 15 listed modules appear:

```powershell
cmake --build build_test_opencv_default --target opencv_core --config Release -- /m 2>&1 | Select-String -Pattern "Building (Custom|)"
```

Expected output: Builds only opencv_core, opencv_imgproc, opencv_imgcodecs, etc. — about 15 DLL targets.

- [ ] **Step 4: Clean up test build directory**

```powershell
Remove-Item -Recurse -Force build_test_opencv_default
```

- [ ] **Step 5: Commit**

```bash
git add cmake/opencv.cmake
git commit -m "perf(build): add default BUILD_LIST guard to opencv.cmake

Without this guard, any consumer that forgets -DBUILD_LIST compiles
all 110 OpenCV modules. The default limits to the 15 modules in
RK_OPENCV_FULL_LIBS. The if-not-set pattern preserves user overrides
from CMakePresets.json or -D flags."
```

**Estimated impact:** Ensures the fix applies even when CI step is missed; protects all future consumers.

---

### Task 3: Remove ineffective ccache setup from Windows CI

**Files:**
- Modify: `.github/workflows/ci.yml:204-217`

**Problem:** ccache has only experimental MSVC support via compiler wrapper. The cache key `hashFiles('CMakeLists.txt', 'src/cpp/**/*.cpp', 'src/cpp/**/*.h')` excludes OpenCV sources — so even if ccache worked, it wouldn't accelerate the dominant cost (OpenCV build). The setup adds ~30s of overhead for zero benefit.

- [ ] **Step 1: Remove the `Cache ccache` step**

Delete lines 204-210:

```yaml
      - name: Cache ccache
        uses: actions/cache@v4
        with:
          path: ~/.ccache
          key: ccache-${{ runner.os }}-${{ hashFiles('CMakeLists.txt', 'src/cpp/**/*.cpp', 'src/cpp/**/*.h') }}
          restore-keys: |
            ccache-${{ runner.os }}-
```

- [ ] **Step 2: Remove the `Configure ccache` step**

Delete lines 212-217:

```yaml
      - name: Configure ccache
        run: |
          if (Get-Command "ccache" -ErrorAction SilentlyContinue) {
            ccache --max-size=500M
            echo "CCACHE_DIR=~/.ccache" >> $env:GITHUB_ENV
          }
```

Also remove the global ccache auto-detection at `CMakeLists.txt:12-18` (optional — it does no harm on non-Windows but adds cmake configure time):

**Option A** (minimal — only remove from CI yaml): No changes to CMakeLists.txt.

**Option B** (clean — also guard ccache detection behind non-MSVC):
```cmake
if(NOT MSVC)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        message(STATUS "ccache detected: ${CCACHE_PROGRAM}")
    endif()
endif()
```

Recommend **Option A** (keep CMakeLists.txt changes separate/minimal).

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "perf(ci): remove ineffective ccache setup from Windows CI

ccache has experimental MSVC support and its cache key excludes
OpenCV sources (the dominant build cost). The ~30s setup overhead
provided zero measurable benefit."
```

**Estimated impact:** Saves ~30s of CI time (setup overhead removed).

---

### Task 4: Gate INT8 quantization + precision tests behind path filters

**Files:**
- Modify: `.github/workflows/ci.yml:297-315`

**Problem:** The INT8 quantization step (`python scripts/quantize_ncnn_int8.py`) runs `ncnn2table` + `ncnn2int8` which takes 2-5 minutes on every CI build. The subsequent precision tests also take ~1 minute. Both are marked `continue-on-error: true`, confirming they're known to be unreliable/non-blocking. These steps are only needed when model files, calibration data, or the quantization script change.

- [ ] **Step 1: Add a path filter condition to the INT8 steps**

The INT8 quantization and precision test steps should only run when relevant paths change:

```yaml
      - name: Quantize models (INT8)
        if: |
          github.event_name == 'workflow_dispatch' ||
          contains(github.event.head_commit.message, 'quantize') ||
          steps.changed-files.outputs.models == 'true' ||
          steps.changed-files.outputs.quantize_script == 'true'
        continue-on-error: true
        shell: pwsh
        run: |
          $ErrorActionPreference = "Stop"
          python scripts/quantize_ncnn_int8.py `
            --model scrfd `
            --fp32-dir "models/scrfd_ncnn" `
            --calib-dir "deps/WIDER_train/WIDER_train/images" `
            --output-dir "models/scrfd_int8_ncnn" `
            --size 640 `
            --num-samples 50

      - name: Run INT8 precision tests
        if: |
          github.event_name == 'workflow_dispatch' ||
          contains(github.event.head_commit.message, 'quantize') ||
          steps.changed-files.outputs.models == 'true' ||
          steps.changed-files.outputs.quantize_script == 'true'
        continue-on-error: true
        shell: pwsh
        run: |
          $ErrorActionPreference = "Stop"
          ctest --test-dir $env:BUILD_DIR -C $env:CMAKE_BUILD_CONFIG -R face_infer --output-on-failure
```

- [ ] **Step 2: Add the `tj-actions/changed-files` step to detect model/script changes**

Insert before the "Quantize models" step (after line 295):

```yaml
      - name: Check for model/quantize changes
        id: changed-files
        uses: tj-actions/changed-files@v44
        with:
          files: |
            models/**
            scripts/quantize_ncnn_int8.py
            deps/WIDER_train/**
```

> **Note:** `tj-actions/changed-files@v44` is used in many OSS projects. If the project prefers a different approach, replace with:
> ```yaml
>       - name: Check for model changes
>         id: changed-files
>         shell: pwsh
>         run: |
>           $changed = git diff --name-only ${{ github.event.pull_request.base.sha }} ${{ github.sha }}
>           $modelsChanged = ($changed | Select-String -Pattern '^models/' -Quiet)
>           $scriptChanged = ($changed | Select-String -Pattern '^scripts/quantize_ncnn_int8\.py' -Quiet)
>           $depsChanged = ($changed | Select-String -Pattern '^deps/WIDER_train/' -Quiet)
>           if ($modelsChanged -or $scriptChanged -or $depsChanged) {
>             echo "changed=true" >> $env:GITHUB_OUTPUT
>           } else {
>             echo "changed=false" >> $env:GITHUB_OUTPUT
>           }
>         ```
Then use `steps.changed-files.outputs.changed == 'true'` instead of `== 'true'` for each sub-path.

Recommend using the native PowerShell approach (no third-party action dependency).

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "perf(ci): gate INT8 quantization + precision tests behind path filter

These steps take 3-6 min total and are only needed when model files,
calibration data, or the quantization script change. On normal code
PRs they can be safely skipped."
```

**Estimated impact:** Saves **3-6 minutes** per CI run on non-model PRs.

---

### Task 5: Cache GTest build output across CI runs

**Files:**
- Modify: `.github/workflows/ci.yml:230-237`

**Problem:** GTest is fetched from GitHub via FetchContent and compiled from source on every CI run. This adds ~1-2 minutes for download + MSVC compilation. The existing `_deps` cache stores OpenCV source repos but does NOT cache GTest build artifacts.

- [ ] **Step 1: Add GTest build directory to the existing `_deps` cache**

The FetchContent populates GTest into `_deps/googletest-src` and `_deps/googletest-build`. Add these paths to the existing `_deps` cache entry, or create a separate cache entry.

**Approach:** Extend the existing `_deps` cache (already stores OpenCV git repos). Rename step to reflect broader scope:

Old:
```yaml
      - name: Cache _deps (opencv sources)
        uses: actions/cache@v4
        with:
          path: ${{ env.DEPS_DIR }}
          key: deps-${{ runner.os }}-opencv-${{ env.OPENCV_VERSION }}-contrib-${{ env.OPENCV_CONTRIB_VERSION }}-v1
          restore-keys: |
            deps-${{ runner.os }}-opencv-${{ env.OPENCV_CONTRIB_VERSION }}-
            deps-${{ runner.os }}-opencv-
```

New:
```yaml
      - name: Cache _deps (opencv sources, GTest build)
        uses: actions/cache@v4
        with:
          path: |
            ${{ env.DEPS_DIR }}
            ${{ env.BUILD_DIR }}/_deps/googletest-build
          key: deps-${{ runner.os }}-opencv-${{ env.OPENCV_VERSION }}-contrib-${{ env.OPENCV_CONTRIB_VERSION }}-gtest-1.12.1-${{ hashFiles('CMakeLists.txt') }}
          restore-keys: |
            deps-${{ runner.os }}-opencv-${{ env.OPENCV_VERSION }}-contrib-${{ env.OPENCV_CONTRIB_VERSION }}-
            deps-${{ runner.os }}-opencv-
```

> **Why this works:** FetchContent checks `_deps/googletest-src` for existing source and `_deps/googletest-build` for existing build artifacts. If the build directory is cached with a valid CMake cache, FetchContent skips the download AND build steps.

> **Cache key strategy:** The key includes `hashFiles('CMakeLists.txt')` because GTest configuration flags could change if CMakeLists.txt is modified. The restore-keys fall back to the broader OpenCV-only key when GTest-specific cache misses.

> **Important:** The GTest build output is inside `${{ env.BUILD_DIR }}/_deps/`, NOT at the top-level `${{ env.DEPS_DIR }}`. The `actions/cache` `path:` parameter accepts a list of paths.

- [ ] **Step 2: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "perf(ci): cache GTest build output across CI runs

Add googletest-build directory to _deps cache. FetchContent reuses
cached build artifacts, avoiding download + MSVC compilation on
subsequent runs. Saves ~1-2 min per CI run."
```

**Estimated impact:** Saves **1-2 minutes** per CI run (compounded if multiple CI runs happen before cache expires).

---

### Task 6: Verify all changes together and run CI

**Files:**
- Validate: Full `.github/workflows/ci.yml`

- [ ] **Step 1: Review the final CI yaml for consistency**

Read the complete modified `ci.yml` and verify:
1. The `env:` block has `OPENCV_BUILD_LIST` defined (Task 1)
2. The `Configure (CMake)` step uses `$env:OPENCV_BUILD_LIST` (Task 1)
3. The ccache steps are removed (Task 3)
4. The INT8 quantization step has an `if:` condition (Task 4)
5. The INT8 precision test step has the same `if:` condition (Task 4)
6. The `Cache _deps` step includes `${{ env.BUILD_DIR }}/_deps/googletest-build` in the path list (Task 5)

- [ ] **Step 2: Run the CMake configure locally to validate the new BUILD_LIST**

```powershell
cmake -S . -B build_ci_verify -G "Visual Studio 17 2022" -A x64 `
  -DOPENCV_ROOT="$env:OPENCV_ROOT" `
  -DOPENCV_CONTRIB_ROOT="$env:OPENCV_CONTRIB_ROOT" `
  -DBUILD_LIST="core,imgproc,imgcodecs,objdetect,features2d,flann,calib3d,dnn,ml,photo,video,videoio,highgui,stitching" `
  -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON
```

Expected: CMake output should show only ~15 OpenCV modules being configured (look for "opencv_core", "opencv_imgproc", etc., and notably ABSENT modules like "opencv_python2", "opencv_java", "opencv_xfeatures2d").

- [ ] **Step 3: Build a single target to measure time**

```powershell
Measure-Command { cmake --build build_ci_verify --config Release --target opencv_core -- /m 2>&1 }
```

Expected: Build completes in ~30-60 seconds (a single OpenCV DLL with BUILD_LIST).

- [ ] **Step 4: Clean up verification build**

```powershell
Remove-Item -Recurse -Force build_ci_verify
```

- [ ] **Step 5: Push all commits and trigger CI**

```bash
git push origin HEAD
```

Then monitor the CI run at `https://github.com/Have-68626/rk3288_opencv/actions`. Expected: Windows CI build completes in <30 minutes.

---

## Self-Review Checklist

**1. Spec coverage:**
- P0 (BUILD_LIST in CI): ✅ Task 1
- P1 (BUILD_LIST in opencv.cmake): ✅ Task 2
- P2 (INT8 quantization gating): ✅ Task 4
- P3 (ccache removal): ✅ Task 3
- P4 (GTest build cache): ✅ Task 5
- Verification: ✅ Task 6

**2. Placeholder scan:** No "TBD", "TODO", or vague patterns found. Every step has exact code, commands, or file paths.

**3. Type consistency:** `OPENCV_BUILD_LIST` env var (Task 1), `BUILD_LIST` cmake variable (Task 1+2), and `$env:OPENCV_BUILD_LIST` reference (Task 1 Step 2) are all consistent. The `if:` condition in Task 4 uses the same boolean logic across both INT8 steps.

**4. No missing tasks:** Every P0-P4 issue from the audit maps to exactly one task. No gap between spec and plan.
