"""保存复现所需的白名单输入；版本探测同样受进程所有权约束。"""

from dataclasses import asdict
import hashlib
import json
from pathlib import Path
import sys

from .runner import Step, execute_step
from .reports import identity


CONFIGS = {
    "windows_build_dir": ("CMakeCache.txt",),
    "web_dir": ("package.json", "pnpm-lock.yaml"),
    "java_home": ("release",),
    "android_sdk": ("source.properties",),
    "opencv_root": ("CMakeLists.txt", "modules/core/include/opencv2/core/version.hpp"),
    "opencv_contrib_root": ("modules/face/CMakeLists.txt",),
    "android_ncnn_dir": tuple(
        f"{abi}/lib/cmake/ncnn/{name}.cmake"
        for abi in ("armeabi-v7a", "arm64-v8a")
        for name in ("ncnnConfig", "ncnnConfigVersion")
    ),
    "precision_tools_dir": ("ncnn_precision_test.exe",),
    "ncnn2table": ("",),
    "ncnn2int8": ("",),
    "public_cascade": ("",),
}


def plan_snapshot(root, suites, options):
    paths = {key: getattr(options, key, None) for key in CONFIGS}
    for suite in suites:
        for step in suite["steps"]:
            if suite["id"] in ("web", "web-e2e"):
                paths["web_dir"] = step.cwd
            if suite["id"] == "windows" and "--build" in step.command:
                paths["windows_build_dir"] = step.command[
                    step.command.index("--build") + 1
                ]
            for key, envkey in (
                ("java_home", "JAVA_HOME"),
                ("android_sdk", "ANDROID_HOME"),
                ("android_ncnn_dir", "NCNN_DIR"),
            ):
                if envkey in step.env:
                    paths[key] = step.env[envkey]
            for arg in step.command:
                for key in ("opencv_root", "opencv_contrib_root"):
                    prefix = "-P" + key.upper() + "="
                    if str(arg).startswith(prefix):
                        paths[key] = str(arg)[len(prefix) :]
    inputs = {}
    for key, value in paths.items():
        if not value:
            continue
        path = Path(value).resolve()
        files = {}
        for name in CONFIGS[key]:
            source = path / name if name else path
            if source.is_file() and not source.is_symlink():
                digest = hashlib.sha256()
                with source.open("rb") as stream:
                    while chunk := stream.read(1024 * 1024):
                        digest.update(chunk)
                files[name or source.name] = dict(
                    path=str(source), sha256=digest.hexdigest()
                )
        inputs[key] = dict(
            path=str(path), scope="选定配置文件，非整个目录身份", files=files
        )
    if any(s["id"].startswith("android-") for s in suites):
        wrapper = Path(root) / "gradle/wrapper/gradle-wrapper.properties"
        if wrapper.is_file():
            inputs["gradle_wrapper"] = dict(path=str(wrapper), **identity(wrapper))
    planned = [
        dict(
            id=s["id"],
            errors=s["errors"],
            tools=s.get("tools", {}),
            steps=[
                dict(
                    **{k: v for k, v in asdict(step).items() if k != "env"},
                    environment_keys=sorted(step.env),
                )
                for step in s["steps"]
            ],
        )
        for s in suites
    ]
    return json.loads(
        json.dumps(dict(schema_version=1, suites=planned, inputs=inputs), default=str)
    )


def collect_versions(suites, output, records):
    from .catalog import native

    records["python"] = dict(
        status="passed", executable=sys.executable, version=sys.version.split()[0]
    )
    seen = set()
    for suite in suites:
        if suite["errors"]:
            continue
        for key, executable in suite.get("tools", {}).items():
            if (key, executable) in seen:
                continue
            seen.add((key, executable))
            label = (
                key
                if key not in records
                else key + "-" + hashlib.sha256(executable.encode()).hexdigest()[:8]
            )
            args = (
                ("-version",)
                if key == "java"
                else (
                    ("-NoProfile", "-Command", "$PSVersionTable.PSVersion.ToString()")
                    if key == "pwsh"
                    else ("--version",)
                )
            )
            item = dict(status="unverified", executable=executable)
            records[label] = item
            step = Step(
                "version-" + key,
                native(executable, *args),
                suite["steps"][0].cwd,
                timeout=15,
                env=dict(suite["steps"][0].env, COREPACK_ENABLE_NETWORK="0", CI="true"),
            )
            try:
                result = execute_step(step, Path(output) / label)
            except KeyboardInterrupt:
                item.update(status="cancelled", reason="CANCELLED")
                raise
            item.update(result)
            if result["status"] == "cancelled":
                raise KeyboardInterrupt
            if result["status"] != "passed":
                raise RuntimeError("TOOL_VERSION_UNVERIFIED")
            log = Path(output) / label / "log.txt"
            try:
                with log.open(encoding="utf-8") as stream:
                    version = stream.read(4097).strip()
                evidence = dict(path=str(log), **identity(log))
            except (OSError, ValueError):
                item.update(status="unverified", reason="VERSION_EVIDENCE_UNAVAILABLE")
                raise RuntimeError("TOOL_VERSION_UNVERIFIED") from None
            if not version or len(version) > 4096:
                item.update(status="unverified", reason="INVALID_VERSION_OUTPUT")
                raise RuntimeError("TOOL_VERSION_UNVERIFIED")
            item.update(version=version, evidence=evidence)
