#!/usr/bin/env python3
"""Linux 服务冒烟：启动 -> GET /api/v1/health -> SIGTERM -> 校验优雅退出。

用法：python3 linux_service_smoke.py <二进制路径> [端口]
端口被占用时以 77 退出（CTest 记为 SKIP，不当作通过）。
"""
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

SKIP_RC = 77


def port_available(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("127.0.0.1", port))
        except OSError:
            return False
    return True


def wait_health(port: int, deadline_s: float = 20.0):
    url = f"http://127.0.0.1:{port}/api/v1/health"
    end = time.monotonic() + deadline_s
    last = None
    while time.monotonic() < end:
        try:
            with urllib.request.urlopen(url, timeout=2) as resp:
                if resp.status == 200:
                    body = resp.read().decode("utf-8", "replace")
                    return json.loads(body)
        except Exception as exc:  # 启动期连接失败属预期，继续轮询
            last = exc
        time.sleep(0.2)
    raise RuntimeError(f"health 未在 {deadline_s}s 内就绪: {last}")


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: linux_service_smoke.py <binary> [port]", file=sys.stderr)
        return 2
    binary = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
    if not port_available(port):
        print(f"PORT_BUSY {port}", file=sys.stderr)
        return SKIP_RC

    with tempfile.TemporaryDirectory(prefix="rk-svc-smoke-") as tmp:
        env = dict(os.environ)
        env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
        env["XDG_CONFIG_HOME"] = tmp
        env["HOME"] = tmp
        env.pop("RK_WCFR_CONFIG", None)
        proc = subprocess.Popen([binary], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        try:
            health = wait_health(port)
            status = health.get("data", {}).get("status")
            if status not in ("ok", "degraded"):
                print(f"BAD_STATUS {status}", file=sys.stderr)
                return 1
            proc.send_signal(signal.SIGTERM)
            try:
                rc = proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
                print("SIGTERM_TIMEOUT", file=sys.stderr)
                return 1
            if rc != 0:
                print(f"BAD_EXIT {rc}", file=sys.stderr)
                return 1
            print(f"linux_service_smoke OK status={status} exit={rc}")
            return 0
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
