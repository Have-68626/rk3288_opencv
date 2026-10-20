"""固定套件与前置条件；不读取历史cache命令或执行任意配置。"""

from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import socket
import importlib.util
import json
import hashlib
from .runner import Step

SUITES = (
    "docs",
    "tooling",
    "windows",
    "android-mock",
    "android-real",
    "web",
    "web-e2e",
)


def installed_pnpm(text):
    try:
        data = json.loads(text)
        return data.get("packageManager") if isinstance(data, dict) else None
    except ValueError:
        match = re.search(
            r"(?m)^packageManager:\s*['\"]?(pnpm@[0-9.]+)['\"]?\s*$", text
        )
        return match.group(1) if match else None


def select(names):
    if not names or any(name not in SUITES for name in names):
        raise ValueError("INVALID_SUITE")
    return [name for name in SUITES if name in names]


def native(executable, *arguments):
    command = list(map(str, [executable, *arguments]))
    if Path(str(executable)).suffix.lower() in (".cmd", ".bat"):
        # call避免首个带空格路径的cmd引号歧义；拒绝二次展开及shell操作符。
        if any(re.search(r"[\r\n&|<>^%!\"]", arg) for arg in command):
            raise ValueError("UNSAFE_BATCH_ARGUMENT")
        return [os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", "call", *command]
    return command


def make_suites(root, names, options, out):
    root = Path(root)
    out = Path(out)
    result = []
    for name in select(names):
        errors = []
        steps = []
        base = out / name
        env = {}

        def tool(key, default):
            value = getattr(options, key, None) or default
            resolved = shutil.which(str(value))
            if not resolved:
                errors.append("MISSING_" + key.upper())
                return str(value)
            return resolved

        node = (
            tool("node", "node")
            if name in ("docs", "tooling", "web", "web-e2e")
            else None
        )
        if node:
            env["PATH"] = (
                str(Path(node).resolve().parent)
                + os.pathsep
                + os.environ.get("PATH", "")
            )

        def add(
            label, command, *, cwd=root, junit=(), unit="case", audit=None, timeout=600
        ):
            steps.append(
                Step(
                    label,
                    command,
                    Path(cwd),
                    getattr(options, "timeout", None) or timeout,
                    tuple(map(str, junit)),
                    unit,
                    dict(
                        env,
                        TEMP=str(
                            root
                            / "cache/reports/.tmp"
                            / hashlib.sha256(out.name.encode()).hexdigest()[:12]
                        ),
                        TMP=str(
                            root
                            / "cache/reports/.tmp"
                            / hashlib.sha256(out.name.encode()).hexdigest()[:12]
                        ),
                    ),
                    audit,
                )
            )

        if name == "docs":
            add(
                "docs-tests",
                [
                    node,
                    "--test",
                    "--test-reporter=junit",
                    "--test-reporter-destination",
                    str(base / "docs-tests/junit.xml"),
                    "tests/scripts/docs-sync-audit.test.js",
                    "tests/scripts/docs-sync-audit-gate.test.js",
                ],
                junit=[base / "docs-tests/junit.xml"],
            )
            add(
                "docs-audit",
                [
                    node,
                    "scripts/docs-sync-audit.js",
                    "--skip-links",
                    "--out-dir",
                    str(base / "docs-audit/report"),
                ],
                audit=base / "docs-audit/report",
            )
        elif name == "tooling":
            if importlib.util.find_spec("pytest") is None:
                errors.append("MISSING_PYTEST")
            for label, command in [
                (
                    "hygiene",
                    [
                        node,
                        "scripts/clean-repo-junk.js",
                        "scan",
                        "--ci",
                        "--output-dir",
                        str(base / "hygiene/report"),
                    ],
                ),
                ("jni", [node, "scripts/jni-sync-audit.js"]),
                ("pins", [node, "scripts/check-workflow-pins.js"]),
            ]:
                add(label, command)
            add(
                "node-tools",
                [
                    node,
                    "--test",
                    "--test-reporter=junit",
                    "--test-reporter-destination",
                    str(base / "node-tools/junit.xml"),
                    *[
                        str(p.relative_to(root))
                        for p in sorted((root / "tests/scripts").glob("*.test.js"))
                    ],
                ],
                junit=[base / "node-tools/junit.xml"],
                timeout=1200,
            )
            for flag, key in [
                ("precision_tools_dir", "RK_PRECISION_TOOLS_DIR"),
                ("ncnn2table", "RK_PRECISION_TOOL_NCNN2TABLE"),
                ("ncnn2int8", "RK_PRECISION_TOOL_NCNN2INT8"),
                ("public_cascade", "RK_PUBLIC_CASCADE_PATH"),
            ]:
                value = getattr(options, flag, None)
                if value:
                    if not Path(value).exists():
                        errors.append("MISSING_" + flag.upper())
                    env[key] = str(Path(value).resolve())
            add(
                "python-tools",
                [
                    sys.executable,
                    "-m",
                    "pytest",
                    "tests/scripts/",
                    "-q",
                    "--basetemp",
                    str(out / "p"),
                    "--junitxml",
                    str(base / "python-tools/junit.xml"),
                ],
                junit=[base / "python-tools/junit.xml"],
                timeout=1200,
            )
        elif name == "windows":
            cmake = tool("cmake", "cmake")
            ctest = tool("ctest", "ctest")
            directory = getattr(options, "windows_build_dir", None)
            build = (
                Path(directory).resolve()
                if directory
                else root / "cache/builds/windows-vs2022-x64"
            )
            if not (build / "CMakeCache.txt").is_file():
                errors.append("MISSING_WINDOWS_BUILD")
            else:
                source = re.search(
                    r"^CMAKE_HOME_DIRECTORY:INTERNAL=([^\r\n]+)$",
                    (build / "CMakeCache.txt").read_text(encoding="utf-8"),
                    re.M,
                )
                if not source or Path(source.group(1)).resolve() != root.resolve():
                    errors.append("WINDOWS_BUILD_SOURCE_MISMATCH")
            add(
                "windows-build",
                [
                    cmake,
                    "--build",
                    str(build),
                    "--config",
                    "Release",
                    "--parallel",
                    "2",
                    "--target",
                    "core_unit_tests",
                    "win_unit_tests",
                    "face_infer_unit_tests",
                    "core_gtest_tests",
                ],
                timeout=3600,
            )
            add(
                "windows-ctest",
                [
                    ctest,
                    "--test-dir",
                    str(build),
                    "-C",
                    "Release",
                    "-R",
                    "^(core_unit_tests|win_unit_tests|face_infer_unit_tests|core_gtest_tests|engine_run_reentry_behavior)$",
                    "--output-on-failure",
                    "--output-junit",
                    str(base / "windows-ctest/junit.xml"),
                ],
                junit=[base / "windows-ctest/junit.xml"],
                unit="target",
            )
        elif name.startswith("android-"):
            java = getattr(options, "java_home", None) or os.environ.get("JAVA_HOME")
            if not java or not (Path(java) / "bin/java.exe").is_file():
                errors.append("MISSING_JAVA_HOME")
            else:
                env.update(
                    JAVA_HOME=str(Path(java).resolve()),
                    PATH=str(Path(java) / "bin")
                    + os.pathsep
                    + os.environ.get("PATH", ""),
                )
                try:
                    version = subprocess.run(
                        [str(Path(java) / "bin/java.exe"), "-version"],
                        capture_output=True,
                        text=True,
                        encoding="utf-8",
                        errors="replace",
                        timeout=10,
                    )
                    if version.returncode or not re.search(
                        r"version \"21[.\"]", version.stdout + version.stderr
                    ):
                        errors.append("JAVA_VERSION_MISMATCH")
                except (OSError, subprocess.TimeoutExpired):
                    errors.append("JAVA_VERSION_UNAVAILABLE")
            sdk = (
                getattr(options, "android_sdk", None)
                or os.environ.get("ANDROID_SDK_ROOT")
                or os.environ.get("ANDROID_HOME")
            )
            if sdk:
                if not (Path(sdk) / "platforms").is_dir():
                    errors.append("MISSING_ANDROID_SDK")
                env.update(
                    ANDROID_HOME=str(Path(sdk).resolve()),
                    ANDROID_SDK_ROOT=str(Path(sdk).resolve()),
                )
            elif not (root / "local.properties").is_file():
                errors.append("MISSING_ANDROID_SDK")
            command = [str(root / "gradlew.bat"), "--no-daemon", "--max-workers=2"]
            if name == "android-mock":
                command += ["-PRK_SKIP_OPENCV=true"]
            else:
                for flag, envkey in [
                    ("opencv_root", "OPENCV_ROOT"),
                    ("opencv_contrib_root", "OPENCV_CONTRIB_ROOT"),
                ]:
                    value = getattr(options, flag, None) or os.environ.get(envkey)
                    if not value or not (
                        (Path(value) / "modules").is_dir()
                        if flag == "opencv_contrib_root"
                        else (Path(value) / "CMakeLists.txt").is_file()
                    ):
                        errors.append("MISSING_" + envkey)
                    else:
                        command += ["-P" + envkey + "=" + str(Path(value).resolve())]
                ncnn = getattr(options, "android_ncnn_dir", None) or os.environ.get(
                    "NCNN_DIR"
                )
                if not ncnn or not all(
                    (Path(ncnn) / abi / "lib/cmake/ncnn/ncnnConfig.cmake").is_file()
                    for abi in ("armeabi-v7a", "arm64-v8a")
                ):
                    errors.append("MISSING_ANDROID_NCNN_ABIS")
                else:
                    env["NCNN_DIR"] = str(Path(ncnn).resolve())
            command += [
                ":app:assembleDebug",
                ":app:checkApkFaceModels",
                ":app:testDebugUnitTest",
                "--rerun",
                ":app:lintDebug",
            ]
            add(
                name,
                native(*command),
                junit=[root / "app/build/test-results/testDebugUnitTest/TEST-*.xml"],
                timeout=3600,
            )
            if name == "android-real" and ncnn:
                steps[-1].android_ncnn = Path(ncnn)
        else:
            pwsh = tool("pwsh", "pwsh") if name == "web-e2e" else None
            pnpm = tool("pnpm", "pnpm")
            try:
                probe = subprocess.run(
                    native(pnpm, "--version"),
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    timeout=10,
                    env={
                        **os.environ,
                        **env,
                        "COREPACK_ENABLE_NETWORK": "0",
                        "CI": "true",
                    },
                )
                if probe.returncode or probe.stdout.strip() != "10.33.0":
                    errors.append("PNPM_VERSION_MISMATCH")
            except (OSError, subprocess.TimeoutExpired):
                errors.append("PNPM_VERSION_UNAVAILABLE")
            web = Path(getattr(options, "web_dir", None) or root / "web").resolve()
            if (
                not (web / "package.json").is_file()
                or not (web / "node_modules/.modules.yaml").is_file()
            ):
                errors.append("MISSING_WEB_DEPENDENCIES")
            else:
                modules = (web / "node_modules/.modules.yaml").read_text(
                    encoding="utf-8"
                )
                if installed_pnpm(modules) != "pnpm@10.33.0":
                    errors.append("WEB_DEPENDENCY_VERSION_MISMATCH")
                # 独立依赖树必须运行当前源码，不能把旧Web副本当本次验收。
                tracked = (
                    subprocess.check_output(
                        ["git", "ls-files", "-z", "--", "web"], cwd=root
                    )
                    .decode("utf-8")
                    .strip("\0")
                    .split("\0")
                )
                for relative in filter(None, tracked):
                    source = root / relative
                    target = web / source.relative_to(root / "web")
                    if (
                        not target.is_file()
                        or target.read_bytes() != source.read_bytes()
                    ):
                        errors.append("WEB_SOURCE_MISMATCH")
                        break
            if name == "web":
                for task in ("lint", "test", "build", "bundle:check"):
                    args = ["run", task]
                    if task == "test":
                        args += [
                            "--outputFile.junit=" + str(base / "web-test/junit.xml")
                        ]
                    add(
                        "web-" + task.replace(":", "-"),
                        native(pnpm, *args),
                        cwd=web,
                        junit=[base / "web-test/junit.xml"] if task == "test" else (),
                    )
            else:
                try:
                    with socket.socket() as probe:
                        probe.bind(("127.0.0.1", 5173))
                except OSError:
                    errors.append("E2E_PORT_BUSY")
                cypress = web / "node_modules/cypress/package.json"
                if cypress.is_file():
                    import json

                    version = json.loads(cypress.read_text(encoding="utf-8"))["version"]
                    cache = Path(
                        os.environ.get("CYPRESS_CACHE_FOLDER")
                        or Path(os.environ.get("LOCALAPPDATA", "")) / "Cypress/Cache"
                    )
                    if not (cache / version / "Cypress/Cypress.exe").is_file():
                        errors.append("MISSING_CYPRESS_BINARY")
                else:
                    errors.append("MISSING_CYPRESS_PACKAGE")
                add(
                    "web-e2e",
                    [
                        pwsh,
                        "-NoProfile",
                        "-File",
                        str(root / "scripts/run-web-e2e.ps1"),
                        "-WebDirectory",
                        str(web),
                        "-PnpmPath",
                        pnpm,
                        "-SkipInstall",
                        "-ReportDirectory",
                        str(base / "web-e2e"),
                    ],
                    junit=[base / "web-e2e/cypress-*.xml"],
                    timeout=1200,
                )
        result.append(dict(id=name, errors=sorted(set(errors)), steps=steps))
    return result
