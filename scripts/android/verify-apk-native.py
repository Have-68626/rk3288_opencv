"""离线检查双ABI、依赖及AAR代码；静态检查不证明设备后端生效。"""
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import zipfile

from _tool_support import Parser, ToolError, check_output, load_json, run_cli, write_report

ABIS = {"armeabi-v7a": (1, 40), "arm64-v8a": (2, 183)}


def elf_info(data):
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4] not in (1, 2) or data[5:7] != b"\x01\x01":
        raise ToolError("INVALID_ELF")
    bits = data[4]
    if bits == 2 and len(data) < 64:
        raise ToolError("INVALID_ELF")
    if struct.unpack_from("<H", data, 16)[0] != 3:
        raise ToolError("ELF_NOT_SHARED_LIBRARY")
    machine = struct.unpack_from("<H", data, 18)[0]
    offset = struct.unpack_from("<I" if bits == 1 else "<Q", data, 32 if bits == 1 else 40)[0]
    stride, count, names_index = struct.unpack_from("<HHH", data, 46 if bits == 1 else 58)
    fmt = "<IIIIIIIIII" if bits == 1 else "<IIQQQQIIQQ"
    if stride < struct.calcsize(fmt) or not count or names_index >= count or offset + stride * count > len(data):
        raise ToolError("ELF_SECTION_BOUNDS")
    sections = [struct.unpack_from(fmt, data, offset + i * stride) for i in range(count)]
    def content(section):
        if section[4] + section[5] > len(data):
            raise ToolError("ELF_SECTION_BOUNDS")
        return data[section[4]:section[4] + section[5]]
    def cstring(strings, index):
        if index >= len(strings) or b"\0" not in strings[index:]:
            raise ToolError("ELF_STRING_BOUNDS")
        return strings[index:].split(b"\0", 1)[0]
    names = content(sections[names_index])
    text_hash, needed, textrel = None, [], False
    for section in sections:
        if section[1] != 8:
            content(section)
        name = cstring(names, section[0])
        if name == b".text" and section[5]:
            text_hash = hashlib.sha256(content(section)).hexdigest()
        if section[1] != 6:
            continue
        if section[6] >= count:
            raise ToolError("ELF_DYNAMIC_BOUNDS")
        strings = content(sections[section[6]])
        dynamic_fmt = "<iI" if bits == 1 else "<qQ"
        dynamic_stride = section[9] or struct.calcsize(dynamic_fmt)
        payload = content(section)
        if dynamic_stride < struct.calcsize(dynamic_fmt) or len(payload) % dynamic_stride:
            raise ToolError("ELF_DYNAMIC_BOUNDS")
        terminated = False
        for index in range(0, len(payload), dynamic_stride):
            kind, value = struct.unpack_from(dynamic_fmt, payload, index)
            if kind == 0:
                terminated = True
                break
            if kind == 1:
                value = cstring(strings, value)
                if not re.fullmatch(rb"[A-Za-z0-9_+.-]+\.so(?:\.\d+)*", value):
                    raise ToolError("ELF_DEPENDENCY_NAME")
                needed.append(value.decode("ascii"))
            if kind == 22 or (kind == 30 and value & 4):
                textrel = True
        if not terminated:
            raise ToolError("ELF_DYNAMIC_UNTERMINATED")
    if text_hash is None:
        raise ToolError("ELF_TEXT_MISSING")
    return {"elf_class": bits, "machine": machine, "text_sha256": text_hash,
            "needed": needed, "has_textrel": textrel}


def zip_names(archive):
    names = archive.namelist()
    if len(names) != len(set(names)):
        raise ToolError("DUPLICATE_ZIP_ENTRY")
    return set(names)


def inspect_apk(apk_path, aar_path, platform_libraries, binding=None):
    try:
        return _inspect_apk(apk_path, aar_path, platform_libraries, binding)
    except (zipfile.BadZipFile, struct.error, UnicodeError):
        raise ToolError("INVALID_APK") from None


def _inspect_apk(apk_path, aar_path, platform_libraries, binding):
    if not isinstance(platform_libraries, dict) or any(not isinstance(platform_libraries.get(abi), list) or
        not platform_libraries[abi] or any(not isinstance(n, str) or not re.fullmatch(r"[A-Za-z0-9_+.-]+\.so", n)
                                          for n in platform_libraries[abi]) for abi in ABIS):
        raise ToolError("INVALID_PLATFORM_LIBRARIES")
    report = {"schema_version": 1, "static_checks_passed": False, "runtime_verified": False,
              "ncnn_build_binding_verified": False, "signature_checked": False, "abis": {},
              "scope": "静态ABI/依赖/代码引用检查；桩检查为启发式，不能替代构建绑定或设备精度验收"}
    with zipfile.ZipFile(apk_path) as apk, zipfile.ZipFile(aar_path) as aar:
        names, aar_names = zip_names(apk), zip_names(aar)
        if any(n.startswith("assets/face_models/") for n in names):
            raise ToolError("APK_CONTAINS_FACE_MODELS")
        packaged_abis = {n.split("/")[1] for n in names if n.startswith("lib/") and n.endswith(".so")}
        if packaged_abis != set(ABIS):
            raise ToolError("APK_ABIS_MISMATCH")
        if binding is not None and (not isinstance(binding, dict) or set(binding) != set(ABIS)):
            raise ToolError("INVALID_BUILD_BINDING")
        for abi, expected in ABIS.items():
            libraries = {n.rsplit("/", 1)[1]: n for n in names if re.fullmatch(r"lib/" + re.escape(abi) + r"/[^/]+\.so", n)}
            core_name = f"lib/{abi}/libnative-lib.so"
            if core_name not in names:
                raise ToolError("CORE_MISSING")
            core = apk.read(core_name)
            if len(core) <= 512 * 1024 or b"nativeGetRtmpFrame" not in core:
                raise ToolError("CORE_STUB_SUSPECTED")
            infos = {}
            for name, member in libraries.items():
                info = elf_info(apk.read(member))
                if (info["elf_class"], info["machine"]) != expected or info["has_textrel"]:
                    raise ToolError("APK_ABI_OR_TEXTREL")
                if set(info["needed"]) - set(libraries) - set(platform_libraries[abi]):
                    raise ToolError("NATIVE_DEPENDENCY_MISSING")
                infos[name] = info
            references = {n for n in aar_names if n.startswith(f"jni/{abi}/") and n.endswith(".so")}
            if not references or f"jni/{abi}/libffmpegkit.so" not in references:
                raise ToolError("AAR_NATIVE_MISSING")
            for member in references:
                info = elf_info(aar.read(member))
                name = member.rsplit("/", 1)[1]
                if name not in infos or infos[name]["text_sha256"] != info["text_sha256"] or (info["elf_class"], info["machine"]) != expected:
                    raise ToolError("AAR_CODE_MISMATCH")
            core_info = infos["libnative-lib.so"]
            if binding is not None:
                paths = binding[abi]
                if not isinstance(paths, dict) or any(not isinstance(paths.get(k), str) for k in ["core_library", "cmake_cache", "build_ninja"]):
                    raise ToolError("INVALID_BUILD_BINDING")
                compiled = elf_info(Path(paths["core_library"]).read_bytes())
                cmake = Path(paths["cmake_cache"]).read_text(encoding="utf8")
                ninja = Path(paths["build_ninja"]).read_text(encoding="utf8")
                if ((compiled["elf_class"], compiled["machine"]) != expected or compiled["text_sha256"] != core_info["text_sha256"]
                    or not re.search(r"^RK_SKIP_OPENCV:BOOL=OFF$", cmake, re.M)
                    or not re.search(r"^RK_ENABLE_NCNN:BOOL=ON$", cmake, re.M)
                    or not any("-Dnative_lib_EXPORTS" in line and "-DRK_HAVE_NCNN=1" in line for line in ninja.splitlines())):
                    raise ToolError("BUILD_BINDING_MISMATCH")
            report["abis"][abi] = {"native_libraries": len(libraries), "aar_libraries": len(references),
                                   "core_sha256": hashlib.sha256(core).hexdigest(), "core_text_sha256": core_info["text_sha256"],
                                   "aar_code_matches": True, "native_dependencies_closed": True}
        descriptors = [b"Lcom/arthenica/ffmpegkit/FFmpegKit;", b"Lcom/arthenica/smartexception/AbstractExceptions;"]
        found = set()
        for name in names:
            if re.fullmatch(r"classes\d*\.dex", name):
                dex = apk.read(name)
                found.update(index for index, marker in enumerate(descriptors) if marker in dex)
        if len(found) != len(descriptors):
            raise ToolError("DEX_REFERENCE_MISSING")
    report.update(static_checks_passed=True, ncnn_build_binding_verified=binding is not None,
                  apk_sha256=hashlib.sha256(Path(apk_path).read_bytes()).hexdigest(),
                  aar_sha256=hashlib.sha256(Path(aar_path).read_bytes()).hexdigest())
    return report


def verify_signature(java, jar, apk):
    try:
        result = subprocess.run([java, "-jar", jar, "verify", "--print-certs", str(apk)],
                                capture_output=True, timeout=45)
    except (OSError, subprocess.TimeoutExpired):
        raise ToolError("SIGNATURE_TOOL_FAILED") from None
    certs = re.findall(rb"Signer #\d+ certificate SHA-256 digest:\s*([a-fA-F0-9]{64})", result.stdout)
    if result.returncode != 0 or not certs:
        raise ToolError("SIGNATURE_UNVERIFIED")
    return [c.decode("ascii").lower() for c in certs]


def main():
    parser = Parser(description=__doc__)
    for key in ["apk", "aar", "platform-libraries", "output"]:
        parser.add_argument("--" + key, required=True)
    parser.add_argument("--build-binding")
    parser.add_argument("--java")
    parser.add_argument("--apksigner-jar")
    args = parser.parse_args()
    check_output(args.output)
    if bool(args.java) != bool(args.apksigner_jar):
        raise ToolError("SIGNATURE_ARGUMENTS_INCOMPLETE")
    report = inspect_apk(args.apk, args.aar, load_json(args.platform_libraries),
                         load_json(args.build_binding) if args.build_binding else None)
    if args.java:
        report["signature_certificate_sha256"] = verify_signature(args.java, args.apksigner_jar, args.apk)
        report["signature_checked"] = True
    write_report(args.output, report)
    print("STATIC_CHECKS_PASS=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(run_cli(main))
