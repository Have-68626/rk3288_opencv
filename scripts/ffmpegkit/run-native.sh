#!/bin/bash
# WSL仅为Android交叉编译宿主；最小环境避免上游脚本继承凭据。
set -euo pipefail
source_dir=''
sdk_root=''
ndk_root=''
while [ "$#" -gt 0 ]; do
    case "$1" in
        --source-dir) source_dir="$2"; shift 2 ;;
        --sdk-root) sdk_root="$2"; shift 2 ;;
        --ndk-root) ndk_root="$2"; shift 2 ;;
        *) printf 'INVALID_ARGUMENTS\n' >&2; exit 2 ;;
    esac
done
[ -n "$source_dir" ] && [ -n "$sdk_root" ] && [ -n "$ndk_root" ] || { printf 'BUILD_INPUTS_REQUIRED\n' >&2; exit 2; }
script_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
python3 "$script_dir/prepare-native.py" --verify-only --source-dir "$source_dir"
grep -Eq '^Pkg.Revision[[:space:]]*=[[:space:]]*25\.2\.9519653[[:space:]]*$' "$ndk_root/source.properties" || { printf 'NDK_VERSION_MISMATCH\n' >&2; exit 2; }
cd -- "$source_dir"
env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 \
    ANDROID_SDK_ROOT="$sdk_root" ANDROID_NDK_ROOT="$ndk_root" \
    GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 GIT_ALLOW_PROTOCOL=file \
    bash ./android.sh --lts --api-level=21 --enable-gpl --enable-x264 --enable-openssl \
    --enable-android-zlib --disable-x86 --disable-x86-64 --no-archive --no-link-time-optimization \
    2>&1 | python3 "$script_dir/_common.py" --redact-stdin
