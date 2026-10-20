#include "rk_win/WinJsonConfig.h"
#include "rk_win/JsonLite.h"
#include "rk_win/RuntimeBootstrap.h"
#include "rk_win/IRecognizer.h"
#include "rk_win/FaceRecognizer.h"
#include "rk_win/FramePipeline.h"
#include "rk_win/FrameProcessor.h"
#include "pipeline/RealtimeFacePipeline.h"
#include "../cpp/ncnn_test_models.h"

#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <thread>

namespace {
struct ProfileTestFiles {
    std::filesystem::path dir = std::filesystem::path(RK_TEST_SOURCE_ROOT) / "cache" /
        ("rk_profile_runtime_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ProfileTestFiles() { std::filesystem::create_directories(dir); }
    ~ProfileTestFiles() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
    rk_win::AppConfig config(const std::string& space) const {
        const auto profileDir = dir / (space.empty() ? "legacy_profile" : space);
        std::filesystem::create_directories(profileDir);
        std::ofstream(profileDir / "det.param") << "synthetic-det";
        std::ofstream(profileDir / "det.bin") << "synthetic-det-weights";
        std::ofstream(profileDir / "emb.param") << "synthetic-emb";
        std::ofstream(profileDir / "emb.bin") << "synthetic-emb-weights";
        std::ofstream(profileDir / "manifest.json") <<
            "{\"schemaVersion\":1,\"profileId\":\"synthetic\",\"displayName\":\"合成模型\","
            "\"supportedAbis\":[\"x86_64\"],"
            "\"detector\":{\"param\":{\"path\":\"det.param\"},\"bin\":{\"path\":\"det.bin\"},"
            "\"input\":\"input.1\",\"outputs\":[\"out0\",\"out1\",\"out2\",\"out3\",\"out4\",\"out5\",\"out6\",\"out7\",\"out8\"],"
            "\"inputWidth\":320,\"inputHeight\":320},"
            "\"embedder\":{\"param\":{\"path\":\"emb.param\"},\"bin\":{\"path\":\"emb.bin\"},"
            "\"input\":\"in0\",\"output\":\"out0\",\"inputWidth\":112,\"inputHeight\":112,\"embeddingDim\":512},"
            "\"detectionConfidence\":0.55,\"nmsIou\":0.4,\"identitySimilarity\":0.73,\"modelVersion\":\"synthetic-v1\",\"licenseId\":\"test\""
            << (space.empty() ? "" : ",\"templateSpaceId\":\"" + space + "\"") << "}";
        rk_win::AppConfig cfg;
        cfg.model.faceProfileDir = profileDir;
        cfg.recognition.databasePath = dir / "faces.db";
        cfg.recognition.cascadePath = dir / "absent_legacy_cascade.xml";
        cfg.recognition.arcFaceModelPath = dir / "absent_legacy_embedder.onnx";
        cfg.dnn.modelPath = dir / "absent_legacy_detector.onnx";
        cfg.recognition.identifyThresholdArcFace = 0.01;
        cfg.dnn.enable = true;
        return cfg;
    }
};

class SyntheticProfileDetector final : public rk_core::IRealtimeDetector {
public:
    bool detect(const cv::Mat&, std::vector<rk_core::RealtimeFaceBox>& out, std::string& err) override {
        err.clear();
        rk_core::RealtimeFaceBox face{0, 0, 112, 112, 0.99f};
        face.hasKeypoints = true;
        face.keypoints = {38.2946f, 51.6963f, 73.5318f, 51.5014f, 56.0252f, 71.7366f,
                          41.5493f, 92.3655f, 70.7299f, 92.2041f};
        out = {face};
        return true;
    }
    const char* name() const override { return "synthetic_scrfd"; }
};
class SyntheticProfileEmbedder final : public rk_core::IRealtimeEmbedder {
public:
    explicit SyntheticProfileEmbedder(std::size_t axis = 0) : axis_(axis) {}
    bool embed(const cv::Mat&, std::vector<float>& out, std::string& err) override {
        err.clear(); out.assign(512, 0.0f); out[axis_] = 1.0f; return true;
    }
    int embeddingDim() const override { return 512; }
    const char* name() const override { return "synthetic_arcface"; }
private:
    std::size_t axis_;
};
rk_core::RealtimePipelineHooks syntheticProfileHooks(std::size_t axis = 0) {
    auto hooks = rk_core::makeDefaultRealtimePipelineHooks();
    hooks.loadDetector = [](const rk_core::FaceRuntimeProfile&, const std::string&, std::string&) {
        return std::make_shared<SyntheticProfileDetector>();
    };
    hooks.loadEmbedder = [axis](const rk_core::FaceRuntimeProfile&, const std::string&, std::string&) {
        return std::make_shared<SyntheticProfileEmbedder>(axis);
    };
    return hooks;
}
class ProfileTestCamera final : public rk_win::ICameraSource {
public:
    rk_win::CameraOpenResult open(const std::wstring& id, int width, int, int) override {
        std::lock_guard<std::mutex> lock(mu);
        rk_win::CameraOpenResult result;
        result.ok = width <= 800;
        result.code = result.ok ? "ok" : "synthetic_open_failure";
        if (result.ok) openedId = id;
        openState = result.ok;
        return result;
    }
    void close() override { openState = false; }
    rk_win::CameraOpenResult readFrameBgr(cv::Mat& out, std::uint64_t& timestamp) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        out = cv::Mat(112, 112, CV_8UC3, cv::Scalar(160, 160, 160));
        timestamp = 1;
        rk_win::CameraOpenResult result; result.ok = true; result.code = "ok"; return result;
    }
    bool isOpen() const override { return openState; }
    rk_win::CameraDevice currentDevice() const override { return {}; }
    rk_win::CameraFormat currentFormat() const override { return {}; }
    std::wstring lastOpenedId() { std::lock_guard<std::mutex> lock(mu); return openedId; }
private:
    std::mutex mu;
    std::wstring openedId;
    std::atomic<bool> openState{false};
};
std::vector<rk_win::CameraDevice> profileTestDevices() {
    std::vector<rk_win::CameraDevice> devices(2);
    for (int i = 0; i < 2; ++i) {
        devices[i].deviceId = i == 0 ? L"cam0" : L"cam1";
        devices[i].name = devices[i].deviceId;
        for (const auto& size : {std::make_pair(640, 480), {800, 600}, {1024, 768}}) {
            rk_win::CameraFormat format; format.width = size.first; format.height = size.second; format.fps = 30;
            devices[i].formats.push_back(format);
        }
    }
    return devices;
}
}

bool test_profile_config_roundtrip_and_legacy_default() {
    ProfileTestFiles files;
    SetEnvironmentVariableW(L"RK_WCFR_CONFIG", (files.dir / "config.json").wstring().c_str());
    struct ResetEnv { ~ResetEnv() { SetEnvironmentVariableW(L"RK_WCFR_CONFIG", nullptr); } } reset;
    rk_win::WinJsonConfigStore store;
    std::string warning;
    if (!store.initialize(warning)) return false;
    if (!store.current().model.faceProfileDir.empty()) return false;
    const auto updated = store.updateFromJsonBody("{\"model\":{\"faceProfileDir\":\"profiles/synthetic\"}}");
    if (!updated.ok) { std::fprintf(stderr, "profile config PUT failed status=%d code=%s message=%s\n", updated.httpStatus, updated.code.c_str(), updated.message.c_str()); for (const auto& detail : updated.details) std::fprintf(stderr, "%s\n", detail.c_str()); return false; }
    rk_win::JsonValue doc;
    std::string err;
    if (!rk_win::parseJson(store.currentRedactedJsonPretty(), doc, err)) return false;
    const auto* model = doc.find("model");
    const auto* value = model ? model->find("faceProfileDir") : nullptr;
    if (!value || !value->isString() || value->s.find("synthetic") == std::string::npos) return false;
    rk_win::WinJsonConfigStore reopened;
    if (!reopened.initialize(warning) || reopened.current().model.faceProfileDir != store.current().model.faceProfileDir) return false;
    if (store.updateFromJsonBody("{\"model\":{\"faceProfileDir\":7}}").ok) return false;
    return store.updateFromJsonBody("{\"model\":{\"faceProfileDir\":\"\"}}").ok && store.current().model.faceProfileDir.empty();
}

bool test_profile_legacy_snapshot_usage_tracks_consumers() {
    ProfileTestFiles files;
    rk_win::AppConfig cfg;
    cfg.model.recognition = "lbph";
    cfg.recognition.cascadePath = std::filesystem::path(RK_TEST_SOURCE_ROOT) / "app/src/main/assets/lbpcascade_frontalface.xml";
    cfg.recognition.databasePath = files.dir / "faces.db";
    cfg.dnn.enable = true;
    cfg.dnn.modelPath = files.dir / "absent_detector.onnx";
    const auto result = rk_win::RuntimeBootstrap::build(cfg);
    if (!result.ok) return false;
    bool cascadeUsed = false, ssdUsed = true;
    for (const auto& model : result.models) {
        if (model.id == "cascade_frontalface") cascadeUsed = model.isInUse;
        if (model.id == "dnn_face_detector") ssdUsed = model.isInUse;
    }
    return cascadeUsed && !ssdUsed;
}

bool test_profile_camera_and_model_same_patch_are_both_consumed() {
    ProfileTestFiles files;
    SetEnvironmentVariableW(L"RK_WCFR_CONFIG", (files.dir / "config.json").wstring().c_str());
    struct ResetEnv { ~ResetEnv() { SetEnvironmentVariableW(L"RK_WCFR_CONFIG", nullptr); } } reset;
    rk_win::WinJsonConfigStore store;
    std::string warning;
    if (!store.initialize(warning)) return false;
    rk_win::JsonValue base = rk_win::JsonValue::makeObject();
    base.o["model"] = rk_win::JsonValue::makeObject(); base.o["model"].o["recognition"] = rk_win::JsonValue::makeString("lbph");
    base.o["recognition"] = rk_win::JsonValue::makeObject();
    base.o["recognition"].o["cascadePath"] = rk_win::JsonValue::makeString((std::filesystem::path(RK_TEST_SOURCE_ROOT) / "app/src/main/assets/lbpcascade_frontalface.xml").string());
    base.o["recognition"].o["databasePath"] = rk_win::JsonValue::makeString((files.dir / "faces.db").string());
    base.o["dnn"] = rk_win::JsonValue::makeObject(); base.o["dnn"].o["enable"] = rk_win::JsonValue::makeBool(false);
    if (!store.updateFromJsonBody(rk_win::toJsonString(base)).ok) return false;
    const auto previous = store.current();
    rk_win::FramePipeline service;
    if (!service.initialize(previous)) return false;
    auto camera = std::make_unique<ProfileTestCamera>();
    auto* source = camera.get();
    service.setCameraForTest(std::move(camera));
    service.setDevicesForTest(profileTestDevices());
    if (!service.applyCameraSettings(0, 640, 480, 30, 300).ok) return false;
    if (!store.updateFromJsonBody("{\"camera\":{\"preferredDeviceId\":\"cam1\",\"width\":800,\"height\":600},\"model\":{\"faceProfileDir\":\"absent_profile\"}}").ok) return false;
    const auto next = store.current();
    const auto generation = service.runtimeReloadGen();
    service.applyModelAndCameraChanges(previous, next);
    const auto models = service.getActiveModels();
    if (source->lastOpenedId() != L"cam1" || service.runtimeReloadGen() == generation || service.runtimeReloadWarning().empty() || models.empty() || models[0].id != "cascade_frontalface") return false;
    auto failedCamera = next;
    failedCamera.camera.preferredDeviceId = L"cam0";
    failedCamera.camera.width = 1024; failedCamera.camera.height = 768;
    failedCamera.model.faceProfileDir = files.dir / "second_absent_profile";
    service.applyModelAndCameraChanges(next, failedCamera);
    const bool rolledBack = source->lastOpenedId() == L"cam1" && !service.runtimeReloadWarning().empty();
    service.shutdown();
    return rolledBack;
}

bool test_profile_bootstrap_enroll_identify_and_space_isolation() {
    ProfileTestFiles files;
    auto cfgA = files.config(std::string(64, 'a'));
    auto cfgB = files.config(std::string(64, 'b'));
    rk_win::FaceDatabase legacy;
    std::vector<float> embedding(512, 0.0f); embedding[0] = 1.0f;
    if (!legacy.updateMean("legacy_synthetic", embedding) || !legacy.save(cfgA.recognition.databasePath)) return false;
    auto hooks = syntheticProfileHooks();
    auto a = rk_win::RuntimeBootstrap::buildForTesting(cfgA, hooks);
    if (!a.ok) std::fprintf(stderr, "profile A bootstrap failed: %s\n", a.warning.c_str());
    if (!a.ok || !a.recognizer || !a.recognizer->personIds().empty() || a.recognizer->threshold() < 0.72) return false;
    cv::Mat image(112, 112, CV_8UC3, cv::Scalar(127, 127, 127));
    int taken = 0;
    if (!a.recognizer->enrollFromFrame("synthetic_shared", image, 1, taken) || taken != 1 || !a.recognizer->saveDb()) { std::fprintf(stderr, "profile enrollment failed\n"); return false; }
    const auto dbA = files.dir / "face_spaces" / std::string(64, 'a') / "faces.db";
    std::ifstream ciphertext(dbA, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(ciphertext), {}};
    ciphertext.close();
    if (bytes.rfind("RKFDBCRY", 0) != 0 || bytes.find("synthetic_shared") != std::string::npos) { std::fprintf(stderr, "profile ciphertext mismatch\n"); return false; }
    auto again = rk_win::RuntimeBootstrap::buildForTesting(cfgA, hooks);
    auto hooksB = syntheticProfileHooks(1);
    auto b = rk_win::RuntimeBootstrap::buildForTesting(cfgB, hooksB);
    if (!again.ok || !b.ok || !b.recognizer->personIds().empty()) { std::fprintf(stderr, "profile reload/isolation failed\n"); return false; }
    auto int8Cfg = cfgA;
    int8Cfg.model.faceProfileDir = files.dir / "int8_profile";
    std::filesystem::copy(cfgA.model.faceProfileDir, int8Cfg.model.faceProfileDir, std::filesystem::copy_options::recursive);
    std::ifstream profileInput(int8Cfg.model.faceProfileDir / "manifest.json");
    const std::string profileText{std::istreambuf_iterator<char>(profileInput), {}};
    profileInput.close();
    rk_win::JsonValue int8Manifest;
    std::string parseError;
    if (!rk_win::parseJson(profileText, int8Manifest, parseError)) return false;
    int8Manifest.o["detector"].o["precision"] = rk_win::JsonValue::makeString("int8");
    int8Manifest.o["embedder"].o["precision"] = rk_win::JsonValue::makeString("int8");
    std::ofstream(int8Cfg.model.faceProfileDir / "manifest.json") << rk_win::toJsonString(int8Manifest);
    auto sameSpaceInt8 = rk_win::RuntimeBootstrap::buildForTesting(int8Cfg, hooks);
    if (!sameSpaceInt8.ok || sameSpaceInt8.models[0].backend != "ncnn_int8" || sameSpaceInt8.recognizer->personIds() != std::vector<std::string>{"synthetic_shared"}) return false;
    const auto matches = again.recognizer->identify(image);
    if (matches.size() != 1 || !matches[0].accepted || matches[0].personId != "synthetic_shared") { std::fprintf(stderr, "profile identify failed\n"); return false; }
    auto unknown = b.recognizer->identify(image);
    if (unknown.size() != 1 || unknown[0].accepted) return false;
    rk_win::FrameProcessor processor(b.recognizer.get());
    rk_win::ControlCommand command;
    const auto unknownResult = processor.run(image, command);
    if (unknownResult.matches.size() != 1 || unknownResult.matches[0].isIdentified) return false;
    // 同一人员在不同空间使用正交向量，防止仅凭人员列表证明隔离而漏掉模板混用。
    if (!b.recognizer->enrollFromFrame("synthetic_shared", image, 1, taken) || !b.recognizer->saveDb()) return false;
    auto bReopened = rk_win::RuntimeBootstrap::buildForTesting(cfgB, hooksB);
    auto bWrongEmbedding = rk_win::RuntimeBootstrap::buildForTesting(cfgB, hooks);
    auto aWrongEmbedding = rk_win::RuntimeBootstrap::buildForTesting(cfgA, hooksB);
    if (!bReopened.ok || !bWrongEmbedding.ok || !aWrongEmbedding.ok) return false;
    const auto bMatches = bReopened.recognizer->identify(image);
    const auto bWrongMatches = bWrongEmbedding.recognizer->identify(image);
    const auto aWrongMatches = aWrongEmbedding.recognizer->identify(image);
    if (bMatches.size() != 1 || !bMatches[0].accepted || bMatches[0].personId != "synthetic_shared" ||
        bWrongMatches.size() != 1 || bWrongMatches[0].accepted || aWrongMatches.size() != 1 || aWrongMatches[0].accepted) return false;
    again.recognizer->clearDb();
    auto cleared = rk_win::RuntimeBootstrap::buildForTesting(cfgA, hooks);
    auto keptB = rk_win::RuntimeBootstrap::buildForTesting(cfgB, hooksB);
    rk_win::FaceDatabase keptLegacy;
    if (!keptLegacy.load(cfgA.recognition.databasePath) || !keptLegacy.hasPerson("legacy_synthetic")) return false;
    const bool ok = cleared.ok && cleared.recognizer->personIds().empty() && keptB.ok && keptB.recognizer->personIds() == std::vector<std::string>{"synthetic_shared"};
    if (!ok) std::fprintf(stderr, "profile clear/reopen failed cleared=%d count=%zu keptB=%d count=%zu warning=%s\n", cleared.ok, cleared.recognizer->personIds().size(), keptB.ok, keptB.recognizer->personIds().size(), cleared.warning.c_str());
    return ok;
}

bool test_profile_bootstrap_empty_space_and_bad_manifest() {
    ProfileTestFiles files;
    auto legacyCfg = files.config("");
    rk_win::FaceDatabase legacy;
    std::vector<float> embedding(512, 0.0f); embedding[0] = 1.0f;
    if (!legacy.updateMean("legacy_synthetic", embedding) || !legacy.save(legacyCfg.recognition.databasePath)) return false;
    auto hooks = syntheticProfileHooks();
    auto loaded = rk_win::RuntimeBootstrap::buildForTesting(legacyCfg, hooks);
    if (!loaded.ok) std::fprintf(stderr, "legacy profile bootstrap failed: %s\n", loaded.warning.c_str());
    if (!loaded.ok || loaded.recognizer->personIds() != std::vector<std::string>{"legacy_synthetic"}) return false;
    auto cfg = files.config(std::string(64, 'c'));
    std::ifstream input(cfg.model.faceProfileDir / "manifest.json");
    const std::string text{std::istreambuf_iterator<char>(input), {}};
    input.close();
    rk_win::JsonValue manifest;
    std::string err;
    if (!rk_win::parseJson(text, manifest, err)) return false;
    manifest.o["templateSpaceId"] = rk_win::JsonValue::makeString("../escape");
    std::ofstream(cfg.model.faceProfileDir / "manifest.json") << rk_win::toJsonString(manifest);
    auto bad = rk_win::RuntimeBootstrap::buildForTesting(cfg, hooks);
    if (bad.ok || bad.warning.empty()) return false;
    manifest.o["templateSpaceId"] = rk_win::JsonValue::makeString(std::string(64, 'A'));
    std::ofstream(cfg.model.faceProfileDir / "manifest.json") << rk_win::toJsonString(manifest);
    if (rk_win::RuntimeBootstrap::buildForTesting(cfg, hooks).ok) return false;
    std::ofstream(cfg.model.faceProfileDir / "manifest.json") << text;
    rk_win::FaceDatabase wrongDim;
    if (!wrongDim.updateMean("synthetic_wrong_dim", {1.0f, 0.0f, 0.0f}) ||
        !wrongDim.save(files.dir / "face_spaces" / std::string(64, 'c') / "faces.db")) return false;
    if (rk_win::RuntimeBootstrap::buildForTesting(cfg, hooks).ok) return false;
    std::filesystem::remove(legacyCfg.model.faceProfileDir / "det.bin");
    auto absent = rk_win::RuntimeBootstrap::buildForTesting(legacyCfg, hooks);
    return !absent.ok && !absent.warning.empty() && !std::filesystem::exists(files.dir / "escape");
}

bool test_profile_default_ncnn_hooks_end_to_end() {
#if defined(RK_HAVE_NCNN) && RK_HAVE_NCNN
    ProfileTestFiles files;
    auto cfg = files.config(std::string(64, 'd'));
    const auto dir = cfg.model.faceProfileDir;
    if (!rk_test::writeConstantScrfdModel(dir, 320) || !rk_test::writeEmbeddingModel(dir, false)) return false;
    for (const auto& names : {std::make_pair("scrfd.param", "det.param"), {"scrfd.bin", "det.bin"},
                              {"embedding.param", "emb.param"}, {"embedding.bin", "emb.bin"}}) {
        std::filesystem::copy_file(dir / names.first, dir / names.second, std::filesystem::copy_options::overwrite_existing);
    }
    std::ifstream in(dir / "manifest.json");
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    in.close();
    rk_win::JsonValue manifest;
    std::string err;
    if (!rk_win::parseJson(text, manifest, err)) return false;
    auto& detector = manifest.o["detector"];
    detector.o["decodeMode"] = rk_win::JsonValue::makeString("distance");
    detector.o["outputLayout"] = rk_win::JsonValue::makeString("nchw");
    auto anchors = rk_win::JsonValue::makeArray();
    for (int i = 0; i < 3; ++i) anchors.a.push_back(rk_win::JsonValue::makeNumber(2));
    detector.o["anchorsPerPos"] = std::move(anchors);
    auto outputs = rk_win::JsonValue::makeArray();
    for (const auto* name : {"s8", "s16", "s32", "b8", "b16", "b32", "k8", "k16", "k32"}) outputs.a.push_back(rk_win::JsonValue::makeString(name));
    detector.o["outputs"] = std::move(outputs);
    std::ofstream(dir / "manifest.json") << rk_win::toJsonString(manifest);
    // 不注入钩子：自造 ncnn 图也必须通过与服务相同的加载、自检和基准。
    auto production = rk_win::RuntimeBootstrap::build(cfg);
    if (!production.ok) { std::fprintf(stderr, "synthetic ncnn profile failed: %s\n", production.warning.c_str()); return false; }
    if (production.models.size() != 1 || production.models[0].id != "face_profile" || !production.models[0].isInUse) return false;
    cv::Mat image(320, 320, CV_8UC3, cv::Scalar(160, 160, 160));
    int taken = 0;
    if (!production.recognizer->enrollFromFrame("synthetic_real_ncnn", image, 1, taken) || taken != 1 || !production.recognizer->saveDb()) return false;
    auto reopened = rk_win::RuntimeBootstrap::build(cfg);
    if (!reopened.ok) return false;
    const auto matches = reopened.recognizer->identify(image);
    if (matches.size() != 1 || !matches[0].accepted || matches[0].personId != "synthetic_real_ncnn") return false;
    // 失败候选必须保留服务真正持有的旧运行时与旧空间，不能只证明旧裸识别器仍可调用。
    rk_win::FramePipeline service;
    cfg.recognition.enrollSamples = 1;
    if (!service.initialize(cfg)) return false;
    const auto before = service.getActiveModels();
    auto invalid = cfg;
    invalid.model.faceProfileDir = files.dir / "absent_profile";
    invalid.recognition.databasePath = files.dir / "wrong_candidate.db";
    if (service.reloadRuntime(invalid) || service.runtimeReloadWarning().empty()) return false;
    const auto after = service.getActiveModels();
    if (before.size() != 1 || after.size() != 1 || before[0].resolvedPath != after[0].resolvedPath || before[0].hash != after[0].hash) return false;
    service.requestEnroll("synthetic_old_runtime");
    const auto seq = service.currentFacesSeq();
    service.injectFrameForTest(image);
    std::uint64_t nextSeq = 0;
    if (!service.waitFacesSeqChanged(seq, 3000, nextSeq)) return false;
    rk_win::FaceDatabase database;
    const auto dbPath = files.dir / "face_spaces" / std::string(64, 'd') / "faces.db";
    if (!database.load(dbPath) || !database.hasPerson("synthetic_old_runtime")) return false;
    service.requestClearDb();
    service.injectFrameForTest(image);
    if (!service.waitFacesSeqChanged(nextSeq, 3000, nextSeq)) return false;
    service.shutdown();
    if (!database.load(dbPath) || !database.listPersonIds().empty() || std::filesystem::exists(invalid.recognition.databasePath)) return false;
    rk_win::FramePipeline combinedService;
    if (!combinedService.initialize(cfg)) return false;
    auto camera = std::make_unique<ProfileTestCamera>();
    auto* source = camera.get();
    combinedService.setCameraForTest(std::move(camera));
    combinedService.setDevicesForTest(profileTestDevices());
    if (!combinedService.applyCameraSettings(0, 640, 480, 30, 300).ok) return false;
    auto switched = cfg;
    switched.model.faceProfileDir = files.dir / "switched_profile";
    std::filesystem::copy(cfg.model.faceProfileDir, switched.model.faceProfileDir, std::filesystem::copy_options::recursive);
    switched.camera.preferredDeviceId = L"cam1";
    switched.camera.width = 800; switched.camera.height = 600;
    combinedService.applyModelAndCameraChanges(cfg, switched);
    const auto switchedModels = combinedService.getActiveModels();
    const bool bothChanged = switchedModels.size() == 1 && switchedModels[0].resolvedPath == (switched.model.faceProfileDir / "manifest.json").string() && source->lastOpenedId() == L"cam1";
    auto cameraFailure = switched;
    cameraFailure.model.faceProfileDir = files.dir / "valid_profile_with_failed_camera";
    std::filesystem::copy(cfg.model.faceProfileDir, cameraFailure.model.faceProfileDir, std::filesystem::copy_options::recursive);
    cameraFailure.camera.preferredDeviceId = L"cam0";
    cameraFailure.camera.width = 1024; cameraFailure.camera.height = 768;
    combinedService.applyModelAndCameraChanges(switched, cameraFailure);
    const auto cameraFailureModels = combinedService.getActiveModels();
    const bool modelSurvivedCameraFailure = cameraFailureModels.size() == 1 &&
        cameraFailureModels[0].resolvedPath == (cameraFailure.model.faceProfileDir / "manifest.json").string() &&
        source->lastOpenedId() == L"cam1" && combinedService.runtimeReloadWarning().empty();
    combinedService.shutdown();
    return bothChanged && modelSurvivedCameraFailure;
#else
    // 默认不启用 ncnn 的构建仍验证配置入口失败，而非把未编译的推理能力计为成功。
    ProfileTestFiles files;
    auto cfg = files.config(std::string(64, 'd'));
    auto result = rk_win::RuntimeBootstrap::build(cfg);
    return !result.ok && result.warning.find("build_disabled") != std::string::npos;
#endif
}
