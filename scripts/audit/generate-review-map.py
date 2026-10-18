"""生成启发式审查分派，不代表文件已审查或测试覆盖率。"""
import csv
import io
from pathlib import Path, PurePosixPath
import re

from _common import Parser, run_cli, write_text

HIGH = (
    "engine", "facesearch", "facedatabase", "wincrypto", "httpfacesserver",
    "httpfacesserverpath", "jsonendpoint", "endpointregistry", "streamsession",
    "facemodelmanager", "enrollment", "featuretemplate", "nativelog",
    "structuredlogger", "winjsonconfig", "jnimethodregistry", "mainactivity",
    "faceruntimeprofile", "faceprofilerequest", "modelregistry", "jni/",
    "path", "crypto", "auth", "redaction", "poster",
)

MED = (
    "framepipeline", "runtimebootstrap", "resultpublisher", "perfreporter",
    "realtimefacepipeline", "trackcoordinator", "videomanager", "eventmanager",
    "frameinputchannel", "scrfdecoder", "arcface", "mobilefacenet", "yoloface",
    "adapters", "recognizer", "camera", "device", "throttle", "permission",
)

def owner_class(p):
    if p.startswith("src/win/third_party/"):
        return "third-party"
    if p.startswith("src/win/app/webroot/"):
        return "generated"
    if p.startswith("_deps/") or p.startswith("deps/"):
        return "third-party"
    if p.startswith("docs/"):
        return "doc"
    if p.startswith("tests/"):
        return "test"
    if p.startswith("scripts/"):
        return "script"
    if p.startswith(".github/") or p.startswith(".claude/"):
        return "infra"
    return "first-party"

def module(p):
    m = [
        ("src/cpp/", "cpp-core"), ("src/win/third_party/", "third-party"),
        ("src/win/", "win"), ("src/java/", "android"), ("app/", "android"),
        ("web/", "web"), ("tests/cpp/", "test-cpp"), ("tests/win/", "test-win"),
        ("tests/unit/", "test-android"), ("tests/scripts/", "test-scripts"),
        ("tests/fixtures/", "test-fixtures"), ("tests/", "test-other"),
        ("docs/", "docs"), ("scripts/", "scripts"), ("cmake/", "build"),
        (".github/", "ci"), (".claude/", "infra"), ("models/", "models"),
        ("deps/", "deps"), ("_deps/", "deps"), ("gradle/", "build"),
    ]
    for pre, name in m:
        if p.startswith(pre):
            return name
    return "root"

def risk(p, oc):
    if oc in ("third-party", "generated"):
        return "medium"
    if oc == "doc":
        return "low"
    if oc == "test":
        return "low"
    if oc == "infra":
        return "medium"
    low = p.lower()
    if any(k in low for k in HIGH):
        return "high"
    if any(k in low for k in MED):
        return "medium"
    if oc == "script":
        return "medium"
    return "medium"

def task(p, oc, mod):
    low = p.lower()
    if p.startswith("src/cpp/jni/") or "native_face_bridge" in low or "nativebridge" in low:
        return "T07b"
    if mod == "cpp-core":
        if "nativelog" in low or "crypto" in low:
            return "T05"
        if any(k in low for k in ("modelregistry", "adapter", "arcface", "mobilefacenet",
                                  "yoloface", "scrfd", "profile", "quantiz", "int8")):
            return "T03"
        if any(k in low for k in ("videomanager", "eventmanager", "frameinputchannel",
                                  "perfreporter", "resource", "mpp", "mpi")):
            return "T06"
        return "T02"
    if mod == "win":
        if any(k in low for k in ("facedatabase", "wincrypto", "winjsonconfig", "winconfig")):
            return "T04"
        if any(k in low for k in ("httpfaces", "endpoint", "jsonendpoint", "streamsession",
                                  "poster", "structuredlogger", "path")):
            return "T05"
        if any(k in low for k in ("framepipeline", "runtimebootstrap", "mfcamera",
                                  "streamsession")):
            return "T06"
        return "T07a"
    if mod == "android":
        if any(k in low for k in ("enrollment", "featuretemplate", "facemodelmanager",
                                  "androidenrollmentrepository")):
            return "T04"
        return "T07b"
    if mod == "web":
        return "T07c"
    if mod == "models":
        return "T03"
    if mod == "deps":
        return "T08"
    if mod == "docs":
        return "T08"
    if mod == "scripts":
        return "T08"
    if mod.startswith("test-"):
        return "T09"
    if mod == "ci":
        return "T09"
    if mod in ("build", "root"):
        return "T08"
    if mod == "infra":
        return "T08"
    return "T01"

def depth(oc, rk, tk):
    if oc in ("third-party", "generated"):
        return "boundary-scan"
    if tk in ("T08", "T09") and oc in ("doc", "script", "infra"):
        return "scan"
    if rk == "high":
        return "full"
    if rk == "low":
        return "scan"
    return "standard"


def main():
    parser = Parser(description=__doc__)
    parser.add_argument('--files', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    paths = Path(args.files).read_text(encoding='utf-8-sig').splitlines()
    if not paths or len(set(paths)) != len(paths):
        raise ValueError('EMPTY_OR_DUPLICATE_PATHS')
    rows = []
    for path in paths:
        parts = path.split('/')
        if not path or PurePosixPath(path).is_absolute() or re.match(r'^[A-Za-z]:', path) or any(part in {'', '.', '..'} for part in parts) or '\\' in path or any(ord(c) < 32 for c in path):
            raise ValueError('INVALID_RELATIVE_PATH')
        owner = owner_class(path)
        mod = module(path)
        priority = risk(path, owner)
        assignment = task(path, owner, mod)
        rows.append(dict(path=path, owner_class=owner, module=mod, risk=priority,
                         task=assignment, review_depth=depth(owner, priority, assignment),
                         review_state='assigned', evidence_ids='',
                         exclusion_reason='仅审上游版本、许可和集成边界' if owner == 'third-party' else '仅审源码映射和可重建性' if owner == 'generated' else ''))
    stream = io.StringIO(newline='')
    writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(rows)
    write_text(args.output, stream.getvalue())
    print('REVIEW_MAP_WRITTEN heuristic=true reviewed=false')


if __name__ == '__main__':
    raise SystemExit(run_cli(main))
