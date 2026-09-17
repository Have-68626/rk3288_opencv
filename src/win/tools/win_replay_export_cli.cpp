// win_replay_export_cli — 把真实录像导出为「检测序列」文本，供 tests/cpp/test_track_replay.cpp
// 的离线回放载体消费（rk3288_opencv 任务 C1 的最后一环：视频 → 逐帧检测序列）。
//
// 链路：VideoCapture 解码 → 逐帧检测（可选：在图库中识别身份）→ pipeline::writeReplayScript 落盘。
//
// 装配沿用 src/win/tools/win_face_eval_cli.cpp：rk_win::FaceRecognizer（Cascade 检测 +
// LBPH 嵌入 + FaceDatabase），图库按 <gallery>/<personId>/*.jpg 目录结构登记。
//
// 降级契约（缺模型/缺图库**不得崩溃**）：
//   * 未给 --gallery：正常检测，但全部检测 authenticated=0、身份写 '-'，并在 stderr 明说
//     「不产出身份」。
//   * Cascade 缺失导致 FaceRecognizer 初始化失败：仍逐帧推进与落盘（该帧无 D 行），
//     在 stderr 说明降级原因——序列化→落盘链路照常被验证。
#include "pipeline/ReplayScriptWriter.h"
#include "rk_win/FaceRecognizer.h"
#include "rk_win/WinConfig.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Args {
    std::string video;
    std::string out;
    int stride = 1;            // 每 N 帧取 1 帧（1 = 不抽稀）
    long long maxFrames = 0;   // 最多导出的帧数（0 = 不限）
    std::string gallery;       // 可选：图库目录
    int tsMsPerFrame = 0;      // 每帧时间戳步进（0 = 由视频 FPS 推导，不可得时回落 40ms）
    std::string cascade = "app/src/main/assets/lbpcascade_frontalface.xml";
    int minFaceSize = 60;
    double threshold = 55.0;
    bool help = false;
};

void printUsage() {
    std::cout
        << "用法: win_replay_export_cli --video <path> --out <path> [选项]\n"
        << "  把视频逐帧检测结果导出为检测序列文本（供 test_track_replay.cpp 回放）。\n\n"
        << "  --video <path>            输入视频（必填）\n"
        << "  --out <path>              输出脚本文件（必填）\n"
        << "  --stride N                每 N 帧取 1 帧（默认 1）\n"
        << "  --max-frames N            最多导出 N 帧（默认 0 = 不限）\n"
        << "  --gallery <dir>           图库目录（<dir>/<personId>/*.jpg）；缺省则不产出身份\n"
        << "  --ts-ms-per-frame N       每帧时间戳步进毫秒（默认 0 = 由 FPS 推导，回落 40）\n"
        << "  --cascade <path>          Cascade 模型路径（默认 app/src/main/assets/...）\n"
        << "  --min-face N              最小人脸像素（默认 60）\n"
        << "  --threshold X             LBPH 识别阈值（默认 55）\n"
        << "  --help                    显示本帮助\n";
}

bool parseArgs(int argc, char** argv, Args& a, std::string& err) {
    for (int i = 1; i < argc; i++) {
        const std::string k = argv[i];
        auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                err = std::string("参数 ") + name + " 缺少取值";
                return nullptr;
            }
            return argv[++i];
        };
        if (k == "--video") {
            const char* v = needValue("--video");
            if (!v) return false;
            a.video = v;
        } else if (k == "--out") {
            const char* v = needValue("--out");
            if (!v) return false;
            a.out = v;
        } else if (k == "--stride") {
            const char* v = needValue("--stride");
            if (!v) return false;
            a.stride = std::max(1, std::atoi(v));
        } else if (k == "--max-frames") {
            const char* v = needValue("--max-frames");
            if (!v) return false;
            a.maxFrames = std::max(0LL, std::atoll(v));
        } else if (k == "--gallery") {
            const char* v = needValue("--gallery");
            if (!v) return false;
            a.gallery = v;
        } else if (k == "--ts-ms-per-frame") {
            const char* v = needValue("--ts-ms-per-frame");
            if (!v) return false;
            a.tsMsPerFrame = std::max(0, std::atoi(v));
        } else if (k == "--cascade") {
            const char* v = needValue("--cascade");
            if (!v) return false;
            a.cascade = v;
        } else if (k == "--min-face") {
            const char* v = needValue("--min-face");
            if (!v) return false;
            a.minFaceSize = std::atoi(v);
        } else if (k == "--threshold") {
            const char* v = needValue("--threshold");
            if (!v) return false;
            a.threshold = std::atof(v);
        } else if (k == "--help" || k == "-h") {
            a.help = true;
        } else {
            err = "未知参数: " + k;
            return false;
        }
    }
    return true;
}

bool isImageFile(const std::filesystem::path& p) {
    const auto ext = p.extension().string();
    std::string e;
    e.resize(ext.size());
    std::transform(ext.begin(), ext.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".bmp";
}

void ensureParentDir(const std::filesystem::path& p) {
    std::error_code ec;
    auto parent = p.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
}

}  // namespace

int main(int argc, char** argv) {
    using namespace rk_win;

    Args args;
    std::string parseErr;
    if (!parseArgs(argc, argv, args, parseErr)) {
        std::cerr << "REPLAY_ERROR bad_args: " << parseErr << "\n";
        printUsage();
        return 4;
    }
    if (args.help) {
        printUsage();
        return 0;
    }
    if (args.video.empty() || args.out.empty()) {
        std::cerr << "REPLAY_ERROR missing_required: 必须同时提供 --video 与 --out\n";
        printUsage();
        return 4;
    }

    // ── 1. 打开视频 ──────────────────────────────────────────────────────────
    cv::VideoCapture cap(args.video, cv::CAP_ANY);
    if (!cap.isOpened()) {
        std::cerr << "REPLAY_ERROR video_open_failed path=" << args.video << "\n";
        return 2;
    }
    const double fps = cap.get(cv::CAP_PROP_FPS);
    long long stepMs = args.tsMsPerFrame;
    if (stepMs <= 0) {
        // 由 FPS 推导逐帧步进；FPS 不可得（<=0 或 NaN）时回落 40ms（25fps）。
        stepMs = (fps > 0.0 && std::isfinite(fps)) ? std::llround(1000.0 / fps) : 40;
        if (stepMs <= 0) stepMs = 40;
    }

    // ── 2. 装配识别器（单一实例；图库可选）───────────────────────────────────
    const auto cascadePath = resolvePathFromExeDir(args.cascade);
    const auto dbPath =
        resolvePathFromExeDir(std::filesystem::path("storage/win_replay_export_db.yml"));
    ensureParentDir(dbPath);

    // 无图库时也要能检测，故无论是否给 --gallery 都先尝试初始化识别器；
    // 失败则退化为「逐帧无 D 行」，但绝不崩溃（见文件头降级契约）。
    FaceRecognizer recognizer;
    bool recognizerReady = false;
    int enrollCount = 0;
    if (!recognizer.initialize(cascadePath.string(), dbPath, args.minFaceSize, args.threshold)) {
        std::cerr << "REPLAY_ERROR recognizer_init_failed cascade=" << cascadePath.string()
                  << " → 无法检测，导出将不含 D 行（序列化→落盘链路仍验证）\n";
    } else {
        recognizer.clearDb();
        recognizerReady = true;
        if (!args.gallery.empty()) {
            std::error_code gec;
            if (!std::filesystem::is_directory(args.gallery, gec)) {
                std::cerr << "REPLAY_WARN gallery_unavailable path=" << args.gallery
                          << "（目录不存在或不可读）→ 不产出身份，检测仍继续\n";
            } else {
                for (const auto& idEntry : std::filesystem::directory_iterator(args.gallery)) {
                    if (!idEntry.is_directory()) continue;
                    const std::string personId = idEntry.path().filename().string();
                    for (const auto& imgEntry :
                         std::filesystem::directory_iterator(idEntry.path())) {
                        if (!imgEntry.is_regular_file() || !isImageFile(imgEntry.path())) continue;
                        cv::Mat img = cv::imread(imgEntry.path().string(), cv::IMREAD_COLOR);
                        if (img.empty()) continue;
                        int taken = 0;
                        if (recognizer.enrollFromFrame(personId, img, 1, taken) && taken > 0) {
                            enrollCount += taken;
                        }
                    }
                }
                std::cout << "REPLAY_INFO gallery=" << args.gallery
                          << " enrolled=" << enrollCount << "\n";
            }
        } else {
            std::cerr << "REPLAY_WARN no_gallery: 未提供 --gallery，检测结果全部 authenticated=0、"
                         "身份写 '-'（不产出身份）\n";
        }
    }

    // ── 3. 逐帧解码 → 检测 → 序列化 ──────────────────────────────────────────
    pipeline::ReplayScriptData script;
    long long frameCounter = 0;   // 视频原始帧号
    long long emitted = 0;
    long long decoded = 0;

    while (true) {
        cv::Mat frame;
        if (!cap.read(frame) || frame.empty()) break;
        const long long idx = frameCounter++;
        decoded++;
        if (args.stride > 1 && (idx % args.stride) != 0) continue;  // 抽稀

        pipeline::ReplayFrame rf;
        rf.frameIndex = static_cast<int>(idx);
        rf.tsMs = idx * stepMs;
        rf.detectionRan = true;

        if (recognizerReady) {
            const auto matches = recognizer.identify(frame);
            for (const auto& m : matches) {
                pipeline::ReplayDetection d;
                d.x = m.rect.x;
                d.y = m.rect.y;
                d.width = m.rect.width;
                d.height = m.rect.height;
                // 只有「被接受且身份非空」才写已认证；否则一律 authenticated=0、身份 '-'。
                if (m.accepted && !m.personId.empty()) {
                    d.authenticated = true;
                    d.identityId = m.personId;
                } else {
                    d.authenticated = false;
                }
                rf.detections.push_back(std::move(d));
            }
        }
        script.push_back(std::move(rf));
        emitted++;
        if (args.maxFrames > 0 && emitted >= args.maxFrames) break;
    }

    // 注释头记录抽稀率与来源——解析器忽略 '#' 行；「首次确认耗时以回放条目步数计」，
    // 抽稀后读数会偏大，故必须把 stride 一并留痕（见 test_track_replay.cpp 顶部说明）。
    std::ostringstream header;
    header << "source=" << args.video << "\n"
           << "stride=" << args.stride << " emitted=" << emitted << " decoded=" << decoded << "\n"
           << "ts_ms_per_frame=" << stepMs << " fps=" << fps << "\n"
           << "gallery=" << (args.gallery.empty() ? std::string("(none)") : args.gallery)
           << " enrolled=" << enrollCount << "\n"
           << "recognizer_ready=" << (recognizerReady ? 1 : 0);

    // ── 4. 落盘：检查 open/write/flush/close 每一步 ─────────────────────────
    const std::filesystem::path outPath(args.out);
    ensureParentDir(outPath);
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "REPLAY_ERROR out_open_failed path=" << outPath.string() << "\n";
        return 3;
    }
    std::string writeErr;
    if (!pipeline::writeReplayScript(out, script, header.str(), writeErr)) {
        std::cerr << "REPLAY_ERROR serialize_rejected: " << writeErr << "\n";
        out.close();
        // 导出器**先校验后写出**，故此刻文件必为 0 字节。但 0 字节的 sequence.txt 会被后来者
        // 误当成「导出成功的结果」，而回放侧解析它必然报「检测序列为空」——失败要失败得干净，
        // 不留下会被误读的半成品（A2 审计 L-7）。
        std::error_code rmEc;
        std::filesystem::remove(outPath, rmEc);
        return 3;
    }
    out.flush();
    if (!out) {
        std::cerr << "REPLAY_ERROR out_flush_failed（磁盘/权限）: " << outPath.string() << "\n";
        return 3;
    }
    out.close();
    if (!out) {
        std::cerr << "REPLAY_ERROR out_close_failed（写入可能未完整落盘）: " << outPath.string()
                  << "\n";
        return 3;
    }

    std::cout << "REPLAY_SUMMARY video=" << args.video << " out=" << outPath.string()
              << " decoded=" << decoded << " emitted=" << emitted << " stride=" << args.stride
              << " recognizer_ready=" << (recognizerReady ? 1 : 0) << " enrolled=" << enrollCount
              << " ts_ms_per_frame=" << stepMs << " fps=" << fps << "\n";
    return 0;
}
