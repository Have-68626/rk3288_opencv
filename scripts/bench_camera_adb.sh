#!/usr/bin/env bash
set -euo pipefail

package_name="${1:-}"
activity_name="${2:-}"
iterations="${3:-50}"
timeout_sec="${4:-45}"
serial="${5:-}"
log_tag="${6:-BENCH_CAMERA}"
output_dir="${7:-tests/bench}"

if [[ -z "${package_name}" || -z "${activity_name}" ]]; then
  echo "用法: $0 <PackageName> <ActivityName> [Iterations] [TimeoutSec] [Serial] [LogTag] [OutputDir]" 1>&2
  exit 2
fi

adb_bin="adb"
if [[ -n "${ANDROID_SDK_ROOT:-}" && -x "${ANDROID_SDK_ROOT}/platform-tools/adb" ]]; then
  adb_bin="${ANDROID_SDK_ROOT}/platform-tools/adb"
elif [[ -n "${ANDROID_HOME:-}" && -x "${ANDROID_HOME}/platform-tools/adb" ]]; then
  adb_bin="${ANDROID_HOME}/platform-tools/adb"
fi

mapfile -t devices < <("${adb_bin}" devices | awk 'NR>1 && $2=="device" {print $1}')
if [[ "${#devices[@]}" -eq 0 ]]; then
  echo "未检测到已连接设备，请先通过 adb 连接目标设备。" 1>&2
  exit 1
fi

if [[ -z "${serial}" ]]; then
  if [[ "${#devices[@]}" -eq 1 ]]; then
    serial="${devices[0]}"
  else
    echo "检测到多个设备，请通过参数指定 Serial：${devices[*]}" 1>&2
    exit 1
  fi
fi

adb_args=(-s "${serial}")
mkdir -p "${output_dir}"

timestamp="$(date +%Y%m%d_%H%M%S)"
out_csv="${output_dir}/bench_camera_${timestamp}.csv"
printf "timestamp,serial,iteration,package,activity,ttff_ms,capture_ok,capture_fail,camera_service_restart,log_tag\n" > "${out_csv}"

wait_for_lines() {
  local deadline
  deadline=$((SECONDS + timeout_sec))
  while (( SECONDS < deadline )); do
    local dump
    dump="$("${adb_bin}" "${adb_args[@]}" logcat -d -v brief -s "${log_tag}:I" 2>/dev/null || true)"
    if [[ -n "${dump}" ]]; then
      printf "%s\n" "${dump}"
      return 0
    fi
    sleep 1
  done
  return 0
}

extract_int() {
  local key="$1"
  awk -v k="${key}" 'match($0, k"=([0-9]+)", a) {print a[1]; exit 0}' || true
}

for ((i=1; i<=iterations; i++)); do
  "${adb_bin}" "${adb_args[@]}" logcat -c >/dev/null 2>&1 || true
  "${adb_bin}" "${adb_args[@]}" shell am force-stop "${package_name}" >/dev/null 2>&1 || true
  "${adb_bin}" "${adb_args[@]}" shell am start -n "${package_name}/${activity_name}" --ez BENCH true --ei BENCH_ITERATION "${i}" >/dev/null 2>&1 || true

  lines="$(wait_for_lines)"
  ttff="$(printf "%s\n" "${lines}" | extract_int "TTFF_MS")"
  capture_ok="$(printf "%s\n" "${lines}" | extract_int "CAPTURE_OK")"
  capture_fail="$(printf "%s\n" "${lines}" | extract_int "CAPTURE_FAIL")"
  camera_service_restart="$(printf "%s\n" "${lines}" | extract_int "CAMERA_SERVICE_RESTART")"

  printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
    "$(date -Iseconds)" \
    "${serial}" \
    "${i}" \
    "${package_name}" \
    "${activity_name}" \
    "${ttff}" \
    "${capture_ok}" \
    "${capture_fail}" \
    "${camera_service_restart}" \
    "${log_tag}" \
    >> "${out_csv}"
done

echo "已生成基准结果：${out_csv}"
