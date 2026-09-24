#include "rk_win/WinConfig.h"
#include "AccelerationContract.h"

#ifdef _WIN32
#include <windows.h>
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace rk_win {
namespace {

constexpr int kInferenceIntervalDefaultMs = 150;
constexpr int kInferenceIntervalMinMs = 80;
constexpr int kInferenceIntervalMaxMs = 500;

// FACE-ARCWIN-010 / FACE-CONFIG-017 的边界常量（kIdentifyThresholdMin/Max、kLogBudgetBytes、
// kLogMaxFileBytesCap）已提到 WinConfig.h：WinJsonConfig.cpp 的规范化告警文本也要用同一份数值，
// 就地复制会漂移（头文件内含推导与取舍说明）。

std::wstring readIniW(const std::filesystem::path& iniPath, const wchar_t* section, const wchar_t* key, const wchar_t* def) {
#ifdef _WIN32
    wchar_t out[32768];
    DWORD n = GetPrivateProfileStringW(section, key, def, out, static_cast<DWORD>(std::size(out)), iniPath.wstring().c_str());
    // 若 n == size-1，值可能被截断；日志记录以便排查
    if (n >= static_cast<DWORD>(std::size(out)) - 1) {
        std::cerr << "WinConfig: readIniW 缓冲区可能不足, key=" << (key ? "" : "null")
                  << " n=" << n << " max=" << (std::size(out) - 1) << std::endl;
    }
    return std::wstring(out, out + n);
#else
    (void)iniPath;
    (void)section;
    (void)key;
    return def ? std::wstring(def) : L"";
#endif
}

int readIniInt(const std::filesystem::path& iniPath, const wchar_t* section, const wchar_t* key, int def) {
#ifdef _WIN32
    return static_cast<int>(GetPrivateProfileIntW(section, key, def, iniPath.wstring().c_str()));
#else
    (void)iniPath;
    (void)section;
    (void)key;
    return def;
#endif
}

bool readIniBool(const std::filesystem::path& iniPath, const wchar_t* section, const wchar_t* key, bool def) {
    return readIniInt(iniPath, section, key, def ? 1 : 0) != 0;
}

std::uint64_t readIniU64(const std::filesystem::path& iniPath, const wchar_t* section, const wchar_t* key, std::uint64_t def) {
    const std::wstring s = readIniW(iniPath, section, key, L"");
    if (s.empty()) return def;
    try {
        return static_cast<std::uint64_t>(std::stoull(s));
    } catch (...) {
        std::wcerr << L"WinConfig: readIniU64 解析失败 value='" << s << L"'" << std::endl;
        return def;
    }
}

double readIniDouble(const std::filesystem::path& iniPath, const wchar_t* section, const wchar_t* key, double def) {
    const std::wstring s = readIniW(iniPath, section, key, L"");
    if (s.empty()) return def;
    try {
        return std::stod(s);
    } catch (...) {
        std::wcerr << L"WinConfig: readIniDouble 解析失败 value='" << s << L"'" << std::endl;
        return def;
    }
}

std::filesystem::path toPath(const std::wstring& ws) {
    if (ws.empty()) return {};
    return std::filesystem::path(ws);
}

bool writeIniW(const std::filesystem::path& iniPath, const wchar_t* section, const wchar_t* key, const std::wstring& value) {
#ifdef _WIN32
    return WritePrivateProfileStringW(section, key, value.c_str(), iniPath.wstring().c_str()) == TRUE;
#else
    (void)iniPath;
    (void)section;
    (void)key;
    (void)value;
    return false;
#endif
}

std::wstring toWStringInt(std::int64_t v) {
    return std::to_wstring(v);
}

std::wstring toWStringDouble(double v) {
    wchar_t buf[64];
#ifdef _WIN32
    swprintf_s(buf, L"%.6f", v);
#else
    std::swprintf(buf, 64, L"%.6f", v);
#endif
    return buf;
}

}  // namespace

// ─── 共享工具函数实现 ──────────────────────────────────────────

std::wstring getEnvW(const wchar_t* name) {
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return L"";
    return std::wstring(buf, buf + n);
#else
    (void)name;
    return L"";
#endif
}

std::string utf8FromWide(const std::wstring& ws) {
    if (ws.empty()) return {};
#ifdef _WIN32
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), out.data(), n, nullptr, nullptr);
    return out;
#else
    return std::string(ws.begin(), ws.end());
#endif
}

std::wstring wideFromUtf8(const std::string& s) {
    if (s.empty()) return L"";
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

std::string asciiLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string normalizeInferenceThrottleMode(std::string s) {
    s = asciiLower(std::move(s));
    if (s == "auto" || s == "manual" || s == "off") return s;
    return "auto";
}

int clampInferenceIntervalMs(int v) {
    return std::clamp(v, 80, 500);
}

double clampIdentifyThreshold(double v) {
    // 钳制到 [1, 64]：上界 64 见 kIdentifyThresholdMax 的推导；下界取 1.
    // 取舍登记（本批**刻意**选 1 而非 0）：选 0 会使「配置里写 0」被放行，但运行时 schema 的
    //   minimum 仍是 1 ⇒ 会造出「clamp 后仍不合法 ⇒ 启动校验被拒 ⇒ 配置被隔离」的死角，正是
    //   本函数要消灭的形态。要选 0 就必须同步把三方 minimum 放宽到 0，而 docs/ 侧不在本次改动
    //   射程内 ⇒ 保持与三方同值的 [1, 64]。
    // ⚠️ 射程订正（原注释此处曾称「既有 config.json 里 < 1 的值改动后改判为『钳到 1 并告警』，
    //   即放宽」——**不实**，本次修复按 WinJsonConfig.cpp 的 normalizeRecognitionKeys 内那两句注释
    //   （「只处理『超过物理上界』的值 ⇒ 不放宽下界的既有拒绝语义」）的正确表述改写）：钳制只在下界
    //   被打到时才生效，而 JSON 侧的调用判据是 `thr->n > kIdentifyThresholdMax`，**不覆盖 `< 1`**
    //   ⇒ `< 1` 的值在此**不被钳**，仍交回 schema 的 `minimum: 1.0` 判**拒** ⇒ 启动期仍是
    //   「拒绝 + 隔离坏配置 + 以默认值启动」，**不是**「钳到 1 并告警」。越界值被钳的只有 `> 64`
    //   这一侧（从「被接受」改判为「钳到 64 并告警」，属不让既有配置失效的收紧方向）。
    //   唯一会在 `< 1` 时真的钳到 1 的入口是 **INI 读入**（loadConfigFromIniOrDefault /
    //   saveConfigToIni 无条件调用本函数），那条路径不经过运行时 schema。
    return std::clamp(v, kIdentifyThresholdMin, kIdentifyThresholdMax);
}

std::uint64_t clampLogMaxFileBytes(std::uint64_t v) {
    // 只压上界（不抬到 1024）：下界属既有 schema 契约，放宽会改变「既有非法配置被拒」的语义。
    // 上界 = 单份文件集均分额的一半（kLogPerSetBudgetBytes / 2 = 89478485），见 kLogMaxFileBytesCap 的推导。
    return std::min(v, kLogMaxFileBytesCap);
}

int clampLogMaxRollFiles(std::uint64_t maxFileBytes, int v) {
    // 按**单份均分额**反推上界：maxFileBytes × (maxRollFiles + 1) ≤ kLogPerSetBudgetBytes
    //   ⇒ maxRollFiles ≤ 均分额 / maxFileBytes − 1（无符号整数除法即下取整）。
    // ⚠️ 分子必须用**钳后**的 maxFileBytes：用原值会算出比真实上界更小的值（例：原值 1 GiB 时
    //    均分额/原值 = 178956970/1073741824 = 0 ⇒ 上界退化为 1，钳不住均分额；这正是 maxFileBytes
    //    也要钳的原因）。
    const std::uint64_t effective = clampLogMaxFileBytes(maxFileBytes);
    const std::uint64_t ratio = (effective == 0) ? 1 : (kLogPerSetBudgetBytes / effective);
    // effective ≤ kLogMaxFileBytesCap = 均分额/2 ⇒ ratio ≥ 2 ⇒ cap ≥ 1，恒不与 schema 的 minimum 1 冲突。
    const int cap = static_cast<int>(ratio > 1 ? ratio - 1 : 1);
    // 只压上界（不抬到 1）：同上，下界属既有 schema 契约。
    return std::min(v, cap);
}

bool checkConfigPath(const std::filesystem::path& p, PathPolicy pol, std::string& reasonOut) {
    reasonOut.clear();
    // 空串 = 「未设置」，由调用方的默认值兜底，不属于本校验射程。
    if (p.empty()) return true;

    // ① 逐**段**判 `..`：必须走 path 的段级迭代，不能用字符串子串匹配——`a/../b`（及其反斜杠形态）、
    //    盘符相对形式 `C:foo`（既非 is_absolute 也无前导分隔符）都会被单纯的前后缀匹配漏判；
    //    段级迭代同时覆盖正/反斜杠两种分隔符（Windows 上 std::filesystem 二者皆识别）。
    for (const auto& seg : p) {
        if (seg == std::filesystem::path(L"..")) {
            reasonOut = std::string((pol == PathPolicy::Strict) ? "拒绝" : "告警") +
                        "：路径含 `..` 段（可越出预期目录）: " + p.string();
            return false;
        }
    }

    // ② 绝对路径：**刻意不判违规**——这就是本次选定的逃生口。
    //    硬约束（用户明确要求）：本机官方用法就是绝对路径（CLAUDE.md 记录的
    //      `D:\ProgramData\NCNN\...`），且默认配置本身由 resolvePathFromExeDir 落成绝对路径、
    //      PUT 全量回写既有配置（tests/win/test_win_json_config.cpp 的 roundtrip 用例即此形态）
    //      必然携带绝对路径 ⇒ 任一档若把绝对路径判为违规，既有合法部署会立刻被 PUT 400 拒绝或被
    //      误报，属迁移破坏。故 Strict 与 Warn 在**判据上完全相同**（这就是头文件里那句
    //      「两档判据相同」的落地形态），差别只在调用方处置（拒绝 vs 告警）。
    //    ⚠️ 如实登记（残留）：这意味着「写型字段用绝对路径」在本批**没有**任何约束力。
    //      要真正约束需引入「可写根白名单」——AppConfig 当前没有该字段，而该字段与配套判据的落点都在
    //      WinJsonConfig.h / AppConfig（不在本批允许改动的文件清单内）⇒ 本批不实现，登记为后续项，
    //      勿把本函数误读为「已禁绝对路径」。
    //    ⚠️ 如实登记（残留 · **UNC / 网络路径同样放行**，此前漏登记）：`\\host\share\...`（UNC）与
    //      `\\?\UNC\host\share\...` 这类**网络路径**在 Windows 上 `is_absolute()` **为真**、且
    //      **不含 `..` 段** ⇒ 本函数在 **Strict 与 Warn 两档都放行**（判据只有 ① 的 `..` 段 ⇒ 网络
    //      路径连**告警**都不会产生，warnUnsafeConfigPaths 只在返回 false 时才记警告）。
    //      `resolvePathFromExeDir` 对绝对路径原样返回（**不判是否本地卷**），运行时 schema 对
    //      `log.logDir` 只要求 `type: string`（**无 `pattern`**，与 docs/windows-web-spa/config.schema.json
    //      同形）⇒ 没有第二道闸门。
    //      后果类别（写型字段，以 `log.logDir` 为例；S4-SEC 独立推演）：
    //      · 越界**写**面不限于本机：`StructuredLogger` / `EventLogger` 的 open 都会对该目录调
    //        `create_directories`（见 StructuredLogger::open 与 EventLogger::open）⇒ 先触发
    //        **向远端主机的 SMB 认证**（NTLM 凭据泄漏面），随后把**含明文 `person_id`** 的日志
    //        写到远端共享；
    //      · 该值可由**本机任意进程**经 `PUT /api/v1/settings` 写入：本函数在 PUT 侧虽为
    //        `PathPolicy::Strict`，但对网络路径不判违规；而 HTTP 层默认**无 token**
    //        （`authToken_` 取自环境变量 `RK_WCFR_HTTP_AUTH_TOKEN`，为空即全匿名放行）⇒ 无兜底。
    //      与 ③ 的两类并列，属**此前未登记**的同形绕过；真正的修法同为「可写根白名单」
    //      （需附「必须是本地卷」判据）。**不在本批**：只如实登记，不改行为。

    // ③ 判据射程（如实登记，勿高估本函数的强度）：本函数**只拦字面 `..` 段**，且只拦这一种形态。
    //    · 等价形态**一律放行**：分量带尾随空格 / 尾随点的 `".. \\evil"`（Win32 路径解析会忽略
    //      尾随空格与点 ⇒ 它实际仍指向父目录）、经 junction / 符号链接中转到目标目录之外的相对
    //      路径（该字符串自身不含 `..` 段）、以及 **UNC / 网络路径**（绝对路径形态，见上方 ② 末的
    //      残留登记）。
    //    · 本函数**不**做 realpath / 词法规范化，也不判「拼接 exeDir 后是否越界」⇒ 它**不声称已
    //      收窄写型字段的越界写面**。当前真正的约束只有上方的绝对路径逃生口登记（绝对路径与 UNC
    //      网络路径都**不**判违规）+ 缺失的可写根白名单；本函数的价值是「可见化 + 拦住最粗的一类
    //      越界写法」。
    return true;
}

// ─── 路径解析 ─────────────────────────────────────────────────

std::filesystem::path getExeDir() {
#ifdef _WIN32
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::filesystem::current_path();
    std::filesystem::path p(path);
    return p.parent_path();
#else
    return std::filesystem::current_path();
#endif
}

std::filesystem::path resolvePathFromExeDir(const std::filesystem::path& p) {
    if (p.empty()) return p;
    if (p.is_absolute()) return p;
    return getExeDir() / p;
}

AppConfig loadConfigFromIniOrDefault() {
    AppConfig cfg;

    std::filesystem::path iniPath = L"config/windows_camera_face_recognition.ini";
    const std::wstring envPath = getEnvW(L"RK_WCFR_CONFIG");
    if (!envPath.empty()) iniPath = envPath;

    cfg.configPath = resolvePathFromExeDir(iniPath);

    cfg.camera.preferredDeviceId = readIniW(cfg.configPath, L"camera", L"preferred_device_id", L"");
    cfg.camera.width = readIniInt(cfg.configPath, L"camera", L"width", cfg.camera.width);
    cfg.camera.height = readIniInt(cfg.configPath, L"camera", L"height", cfg.camera.height);
    cfg.camera.fps = readIniInt(cfg.configPath, L"camera", L"fps", cfg.camera.fps);

    cfg.recognition.cascadePath = resolvePathFromExeDir(toPath(readIniW(cfg.configPath, L"recognition", L"cascade_path", L"assets/lbpcascade_frontalface.xml")));
    cfg.recognition.databasePath = resolvePathFromExeDir(toPath(readIniW(cfg.configPath, L"recognition", L"database_path", L"storage/win_face_db.yml")));
    cfg.recognition.arcFaceModelPath = resolvePathFromExeDir(toPath(readIniW(cfg.configPath, L"recognition", L"arcface_model_path", L"models/arcface_w600k_r50.onnx")));
    cfg.recognition.minFaceSizePx = readIniInt(cfg.configPath, L"recognition", L"min_face_size_px", cfg.recognition.minFaceSizePx);
    // INI 读入点（FACE-ARCWIN-010）：钳到 [1, 64] 的物理合法区间后再进 cfg。
    //   处置理由：INI 路径**不**经过运行时 schema 校验（它只在 JSON 侧生效），若在此放行越界值，
    //   cfg 会带着越界阈值被 RuntimeBootstrap 注入 LBPH 识别器（阈值 > 64 ⇒ 判定恒真、形同虚设）。
    //   与 clampInferenceIntervalMs 在本函数内的既有先例同形态：读入即钳，只钳不拒。
    cfg.recognition.identifyThreshold = clampIdentifyThreshold(
        readIniDouble(cfg.configPath, L"recognition", L"identify_threshold", cfg.recognition.identifyThreshold));
    // ArcFace 专用阈值（余弦相似度，FACE-ARCWIN-006/007）：独立键，缺省保留结构体默认 0.45。
    cfg.recognition.identifyThresholdArcFace = readIniDouble(cfg.configPath, L"recognition", L"identify_threshold_arcface", cfg.recognition.identifyThresholdArcFace);
    cfg.recognition.enrollSamples = readIniInt(cfg.configPath, L"recognition", L"enroll_samples", cfg.recognition.enrollSamples);
    // ArcFace 节点名（FACE-ARCWIN-001）：键缺省为空串 ⇒ 自动（OpenCV 默认输入 / 首个输出）。
    cfg.recognition.arcFaceInputName = utf8FromWide(readIniW(cfg.configPath, L"recognition", L"arcface_input_name", L""));
    cfg.recognition.arcFaceOutputName = utf8FromWide(readIniW(cfg.configPath, L"recognition", L"arcface_output_name", L""));

    cfg.inference.throttleMode =
        normalizeInferenceThrottleMode(utf8FromWide(readIniW(cfg.configPath, L"inference", L"throttle_mode", L"auto")));
    cfg.inference.intervalMs =
        clampInferenceIntervalMs(readIniInt(cfg.configPath, L"inference", L"interval_ms", kInferenceIntervalDefaultMs));

    cfg.dnn.enable = readIniBool(cfg.configPath, L"dnn", L"enable", cfg.dnn.enable);
    {
        std::filesystem::path mp = toPath(readIniW(cfg.configPath, L"dnn", L"model_path", L"storage/models/opencv_face_detector_uint8.pb"));
        std::filesystem::path cp = toPath(readIniW(cfg.configPath, L"dnn", L"config_path", L"storage/models/opencv_face_detector.pbtxt"));
        const std::wstring envModel = getEnvW(L"RK_WCFR_DNN_MODEL");
        const std::wstring envConfig = getEnvW(L"RK_WCFR_DNN_CONFIG");
        if (!envModel.empty()) mp = envModel;
        if (!envConfig.empty()) cp = envConfig;
        cfg.dnn.modelPath = resolvePathFromExeDir(mp);
        cfg.dnn.configPath = resolvePathFromExeDir(cp);
    }
    cfg.dnn.inputWidth = readIniInt(cfg.configPath, L"dnn", L"input_width", cfg.dnn.inputWidth);
    cfg.dnn.inputHeight = readIniInt(cfg.configPath, L"dnn", L"input_height", cfg.dnn.inputHeight);
    cfg.dnn.scale = readIniDouble(cfg.configPath, L"dnn", L"scale", cfg.dnn.scale);
    cfg.dnn.meanB = readIniInt(cfg.configPath, L"dnn", L"mean_b", cfg.dnn.meanB);
    cfg.dnn.meanG = readIniInt(cfg.configPath, L"dnn", L"mean_g", cfg.dnn.meanG);
    cfg.dnn.meanR = readIniInt(cfg.configPath, L"dnn", L"mean_r", cfg.dnn.meanR);
    cfg.dnn.swapRB = readIniBool(cfg.configPath, L"dnn", L"swap_rb", cfg.dnn.swapRB);
    cfg.dnn.confThreshold = readIniDouble(cfg.configPath, L"dnn", L"conf_threshold", cfg.dnn.confThreshold);
    cfg.dnn.backend = readIniInt(cfg.configPath, L"dnn", L"backend", cfg.dnn.backend);
    cfg.dnn.target = readIniInt(cfg.configPath, L"dnn", L"target", cfg.dnn.target);

    cfg.http.enable = readIniBool(cfg.configPath, L"http", L"enable", cfg.http.enable);
    cfg.http.port = readIniInt(cfg.configPath, L"http", L"port", cfg.http.port);
    {
        const std::wstring envPort = getEnvW(L"RK_WCFR_HTTP_PORT");
        if (!envPort.empty()) {
            try {
                const int p = std::stoi(envPort);
                if (p >= 1 && p <= 65535) cfg.http.port = p;
            } catch (...) {
            }
        }
    }

    cfg.poster.enable = readIniBool(cfg.configPath, L"poster", L"enable", cfg.poster.enable);
    cfg.poster.postUrl = utf8FromWide(readIniW(cfg.configPath, L"poster", L"post_url", L"http://localhost:8080/api/faces"));
    cfg.poster.throttleMs = readIniInt(cfg.configPath, L"poster", L"throttle_ms", cfg.poster.throttleMs);
    cfg.poster.backoffMinMs = readIniInt(cfg.configPath, L"poster", L"backoff_min_ms", cfg.poster.backoffMinMs);
    cfg.poster.backoffMaxMs = readIniInt(cfg.configPath, L"poster", L"backoff_max_ms", cfg.poster.backoffMaxMs);
    {
        const std::wstring envUrl = getEnvW(L"RK_WCFR_POST_URL");
        if (!envUrl.empty()) cfg.poster.postUrl = utf8FromWide(envUrl);
    }

    cfg.log.logDir = resolvePathFromExeDir(toPath(readIniW(cfg.configPath, L"log", L"log_dir", L"storage/win_logs")));
    // INI 读入点（FACE-CONFIG-017）：先钳 maxFileBytes，再用**钳后值**反推 maxRollFiles 的上界
    //   （传原值会算出偏小的上界，见 clampLogMaxRollFiles 内注释）。同 identifyThreshold 处同源的「读入即钳」。
    cfg.log.maxFileBytes = clampLogMaxFileBytes(readIniU64(cfg.configPath, L"log", L"max_file_bytes", cfg.log.maxFileBytes));
    cfg.log.maxRollFiles = clampLogMaxRollFiles(cfg.log.maxFileBytes,
                                                readIniInt(cfg.configPath, L"log", L"max_roll_files", cfg.log.maxRollFiles));

    cfg.ui.windowWidth = readIniInt(cfg.configPath, L"ui", L"window_width", cfg.ui.windowWidth);
    cfg.ui.windowHeight = readIniInt(cfg.configPath, L"ui", L"window_height", cfg.ui.windowHeight);
    cfg.ui.previewScaleMode = readIniInt(cfg.configPath, L"ui", L"preview_scale_mode", cfg.ui.previewScaleMode);

    cfg.display.outputIndex = readIniInt(cfg.configPath, L"display", L"output_index", cfg.display.outputIndex);
    cfg.display.width = readIniInt(cfg.configPath, L"display", L"width", cfg.display.width);
    cfg.display.height = readIniInt(cfg.configPath, L"display", L"height", cfg.display.height);
    cfg.display.refreshNumerator = static_cast<std::uint32_t>(readIniInt(cfg.configPath, L"display", L"refresh_numerator", static_cast<int>(cfg.display.refreshNumerator)));
    cfg.display.refreshDenominator = static_cast<std::uint32_t>(readIniInt(cfg.configPath, L"display", L"refresh_denominator", static_cast<int>(cfg.display.refreshDenominator)));
    if (cfg.display.refreshDenominator == 0) cfg.display.refreshDenominator = 1;

    cfg.display.vsync = readIniBool(cfg.configPath, L"display", L"vsync", cfg.display.vsync);
    cfg.display.swapchainBuffers = readIniInt(cfg.configPath, L"display", L"swapchain_buffers", cfg.display.swapchainBuffers);
    cfg.display.fullscreen = readIniBool(cfg.configPath, L"display", L"fullscreen", cfg.display.fullscreen);
    cfg.display.allowSystemModeSwitch = readIniBool(cfg.configPath, L"display", L"allow_system_mode_switch", cfg.display.allowSystemModeSwitch);

    cfg.display.enableSRGB = readIniBool(cfg.configPath, L"display", L"enable_srgb", cfg.display.enableSRGB);
    cfg.display.gamma = readIniDouble(cfg.configPath, L"display", L"gamma", cfg.display.gamma);
    cfg.display.colorTempK = readIniInt(cfg.configPath, L"display", L"color_temp_k", cfg.display.colorTempK);

    cfg.display.aaSamples = readIniInt(cfg.configPath, L"display", L"aa_samples", cfg.display.aaSamples);
    cfg.display.anisoLevel = readIniInt(cfg.configPath, L"display", L"aniso_level", cfg.display.anisoLevel);

    cfg.acceleration.enableOpenCL = readIniBool(cfg.configPath, L"acceleration", L"enable_opencl", cfg.acceleration.enableOpenCL);
    cfg.acceleration.enableLibyuv = readIniBool(cfg.configPath, L"acceleration", L"enable_libyuv", cfg.acceleration.enableLibyuv);
    cfg.acceleration.enableMpp = readIniBool(cfg.configPath, L"acceleration", L"enable_mpp", cfg.acceleration.enableMpp);
    cfg.acceleration.enableQualcomm = readIniBool(cfg.configPath, L"acceleration", L"enable_qualcomm", cfg.acceleration.enableQualcomm);

    cfg.model.detection = utf8FromWide(readIniW(cfg.configPath, L"model", L"detection", wideFromUtf8(cfg.model.detection).c_str()));
    cfg.model.recognition = utf8FromWide(readIniW(cfg.configPath, L"model", L"recognition", wideFromUtf8(cfg.model.recognition).c_str()));
    cfg.model.backend = rk_accel::normalizeBackendName(
        utf8FromWide(readIniW(cfg.configPath, L"model", L"backend", wideFromUtf8(cfg.model.backend).c_str())),
        cfg.model.backend);
    cfg.model.detectorBackend = rk_accel::normalizeBackendName(
        utf8FromWide(readIniW(cfg.configPath, L"model", L"detector_backend", wideFromUtf8(cfg.model.backend).c_str())),
        cfg.model.backend);
    cfg.model.recognitionBackend = rk_accel::normalizeBackendName(
        utf8FromWide(readIniW(cfg.configPath, L"model", L"recognition_backend", wideFromUtf8(cfg.model.backend).c_str())),
        cfg.model.backend);
    cfg.model.autoFallback = readIniBool(cfg.configPath, L"model", L"auto_fallback", cfg.model.autoFallback);
    cfg.model.int8Enabled = readIniBool(cfg.configPath, L"model", L"int8_enabled", cfg.model.int8Enabled);

    return cfg;
}

bool saveConfigToIni(const AppConfig& cfg) {
    if (cfg.configPath.empty()) return false;
    bool ok = true;

    ok = writeIniW(cfg.configPath, L"camera", L"preferred_device_id", cfg.camera.preferredDeviceId) && ok;
    ok = writeIniW(cfg.configPath, L"camera", L"width", toWStringInt(cfg.camera.width)) && ok;
    ok = writeIniW(cfg.configPath, L"camera", L"height", toWStringInt(cfg.camera.height)) && ok;
    ok = writeIniW(cfg.configPath, L"camera", L"fps", toWStringInt(cfg.camera.fps)) && ok;

    ok = writeIniW(cfg.configPath, L"recognition", L"cascade_path", cfg.recognition.cascadePath.wstring()) && ok;
    ok = writeIniW(cfg.configPath, L"recognition", L"database_path", cfg.recognition.databasePath.wstring()) && ok;
    ok = writeIniW(cfg.configPath, L"recognition", L"arcface_model_path", cfg.recognition.arcFaceModelPath.wstring()) && ok;
    ok = writeIniW(cfg.configPath, L"recognition", L"min_face_size_px", toWStringInt(cfg.recognition.minFaceSizePx)) && ok;
    // INI 写回点（FACE-ARCWIN-010）：与下方 interval_ms 的写回先例一致——写盘前再钳一次，
    //   使「磁盘上的 INI 值」与「内存 cfg 的合法区间」不会因调用方直接改 cfg 而漂移（钳是幂等的）。
    ok = writeIniW(cfg.configPath, L"recognition", L"identify_threshold", toWStringDouble(clampIdentifyThreshold(cfg.recognition.identifyThreshold))) && ok;
    ok = writeIniW(cfg.configPath, L"recognition", L"identify_threshold_arcface", toWStringDouble(cfg.recognition.identifyThresholdArcFace)) && ok;
    ok = writeIniW(cfg.configPath, L"recognition", L"enroll_samples", toWStringInt(cfg.recognition.enrollSamples)) && ok;
    ok = writeIniW(cfg.configPath, L"recognition", L"arcface_input_name", wideFromUtf8(cfg.recognition.arcFaceInputName)) && ok;
    ok = writeIniW(cfg.configPath, L"recognition", L"arcface_output_name", wideFromUtf8(cfg.recognition.arcFaceOutputName)) && ok;

    ok = writeIniW(cfg.configPath, L"inference", L"throttle_mode", wideFromUtf8(normalizeInferenceThrottleMode(cfg.inference.throttleMode))) && ok;
    ok = writeIniW(cfg.configPath, L"inference", L"interval_ms", toWStringInt(clampInferenceIntervalMs(cfg.inference.intervalMs))) && ok;

    ok = writeIniW(cfg.configPath, L"dnn", L"enable", toWStringInt(cfg.dnn.enable ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"model_path", cfg.dnn.modelPath.wstring()) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"config_path", cfg.dnn.configPath.wstring()) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"input_width", toWStringInt(cfg.dnn.inputWidth)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"input_height", toWStringInt(cfg.dnn.inputHeight)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"scale", toWStringDouble(cfg.dnn.scale)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"mean_b", toWStringInt(cfg.dnn.meanB)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"mean_g", toWStringInt(cfg.dnn.meanG)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"mean_r", toWStringInt(cfg.dnn.meanR)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"swap_rb", toWStringInt(cfg.dnn.swapRB ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"conf_threshold", toWStringDouble(cfg.dnn.confThreshold)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"backend", toWStringInt(cfg.dnn.backend)) && ok;
    ok = writeIniW(cfg.configPath, L"dnn", L"target", toWStringInt(cfg.dnn.target)) && ok;

    ok = writeIniW(cfg.configPath, L"http", L"enable", toWStringInt(cfg.http.enable ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"http", L"port", toWStringInt(cfg.http.port)) && ok;

    ok = writeIniW(cfg.configPath, L"poster", L"enable", toWStringInt(cfg.poster.enable ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"poster", L"post_url", wideFromUtf8(cfg.poster.postUrl)) && ok;
    ok = writeIniW(cfg.configPath, L"poster", L"throttle_ms", toWStringInt(cfg.poster.throttleMs)) && ok;
    ok = writeIniW(cfg.configPath, L"poster", L"backoff_min_ms", toWStringInt(cfg.poster.backoffMinMs)) && ok;
    ok = writeIniW(cfg.configPath, L"poster", L"backoff_max_ms", toWStringInt(cfg.poster.backoffMaxMs)) && ok;

    ok = writeIniW(cfg.configPath, L"log", L"log_dir", cfg.log.logDir.wstring()) && ok;
    // INI 写回点（FACE-CONFIG-017）：同读入点的形态——先钳 maxFileBytes，再用**钳后值**反推
    //   maxRollFiles 上界，保证写盘产物满足总预算的**单份均分额**（⇔ 单份落盘量 ≤ 总预算；
    //   钳幂等，重复写不会持续变化）。
    {
        const std::uint64_t iniMaxFileBytes = clampLogMaxFileBytes(cfg.log.maxFileBytes);
        ok = writeIniW(cfg.configPath, L"log", L"max_file_bytes", toWStringInt(static_cast<std::int64_t>(iniMaxFileBytes))) && ok;
        ok = writeIniW(cfg.configPath, L"log", L"max_roll_files",
                       toWStringInt(clampLogMaxRollFiles(iniMaxFileBytes, cfg.log.maxRollFiles))) && ok;
    }

    ok = writeIniW(cfg.configPath, L"ui", L"window_width", toWStringInt(cfg.ui.windowWidth)) && ok;
    ok = writeIniW(cfg.configPath, L"ui", L"window_height", toWStringInt(cfg.ui.windowHeight)) && ok;
    ok = writeIniW(cfg.configPath, L"ui", L"preview_scale_mode", toWStringInt(cfg.ui.previewScaleMode)) && ok;

    ok = writeIniW(cfg.configPath, L"display", L"output_index", toWStringInt(cfg.display.outputIndex)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"width", toWStringInt(cfg.display.width)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"height", toWStringInt(cfg.display.height)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"refresh_numerator", toWStringInt(cfg.display.refreshNumerator)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"refresh_denominator", toWStringInt(cfg.display.refreshDenominator)) && ok;

    ok = writeIniW(cfg.configPath, L"display", L"vsync", toWStringInt(cfg.display.vsync ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"swapchain_buffers", toWStringInt(cfg.display.swapchainBuffers)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"fullscreen", toWStringInt(cfg.display.fullscreen ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"allow_system_mode_switch", toWStringInt(cfg.display.allowSystemModeSwitch ? 1 : 0)) && ok;

    ok = writeIniW(cfg.configPath, L"display", L"enable_srgb", toWStringInt(cfg.display.enableSRGB ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"gamma", toWStringDouble(cfg.display.gamma)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"color_temp_k", toWStringInt(cfg.display.colorTempK)) && ok;

    ok = writeIniW(cfg.configPath, L"display", L"aa_samples", toWStringInt(cfg.display.aaSamples)) && ok;
    ok = writeIniW(cfg.configPath, L"display", L"aniso_level", toWStringInt(cfg.display.anisoLevel)) && ok;

    ok = writeIniW(cfg.configPath, L"acceleration", L"enable_opencl", toWStringInt(cfg.acceleration.enableOpenCL ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"acceleration", L"enable_libyuv", toWStringInt(cfg.acceleration.enableLibyuv ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"acceleration", L"enable_mpp", toWStringInt(cfg.acceleration.enableMpp ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"acceleration", L"enable_qualcomm", toWStringInt(cfg.acceleration.enableQualcomm ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"model", L"detection", wideFromUtf8(cfg.model.detection)) && ok;
    ok = writeIniW(cfg.configPath, L"model", L"recognition", wideFromUtf8(cfg.model.recognition)) && ok;
    ok = writeIniW(cfg.configPath, L"model", L"backend", wideFromUtf8(rk_accel::normalizeBackendName(cfg.model.backend, "opencv_dnn"))) && ok;
    ok = writeIniW(cfg.configPath, L"model", L"detector_backend", wideFromUtf8(rk_accel::normalizeBackendName(cfg.model.detectorBackend, "opencv_dnn"))) && ok;
    ok = writeIniW(cfg.configPath, L"model", L"recognition_backend", wideFromUtf8(rk_accel::normalizeBackendName(cfg.model.recognitionBackend, "opencv_dnn"))) && ok;
    ok = writeIniW(cfg.configPath, L"model", L"auto_fallback", toWStringInt(cfg.model.autoFallback ? 1 : 0)) && ok;
    ok = writeIniW(cfg.configPath, L"model", L"int8_enabled", toWStringInt(cfg.model.int8Enabled ? 1 : 0)) && ok;

    return ok;
}

}  // namespace rk_win

