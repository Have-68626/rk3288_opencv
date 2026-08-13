#include "rk_win/HttpFacesServerPath.h"
#include "rk_win/HttpFacesServer.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

TEST(HttpFacesServer, PathValidation) {
    std::filesystem::path docRoot = std::filesystem::temp_directory_path() / "rk_http_faces_server_path_test_root";
    std::error_code ec;
    std::filesystem::remove_all(docRoot, ec);
    std::filesystem::create_directories(docRoot, ec);
    std::filesystem::create_directories(docRoot / "assets", ec);
    ASSERT_TRUE(!ec && std::filesystem::exists(docRoot))
        << "unable to create docRoot path=" << docRoot.string()
        << " ec=" << ec.value() << " message=" << ec.message();
    {
        std::ofstream out(docRoot / "index.html");
        out.put('\n');
    }

    auto check = [&](const std::string& urlPath, bool expected) {
        std::filesystem::path out;
        bool ok = rk_win::isSafeRelativePath(docRoot, urlPath, out);
        EXPECT_EQ(ok, expected) << "urlPath=" << urlPath;
    };

    auto checkResolved = [&](const std::string& urlPath, const std::filesystem::path& expectedSuffix) {
        std::filesystem::path out;
        bool ok = rk_win::isSafeRelativePath(docRoot, urlPath, out);
        ASSERT_TRUE(ok) << "urlPath=" << urlPath;
        // isSafeRelativePath 内部用 weakly_canonical 把 8.3 短名（如 RUNNER~1）
        // 解析为长名（runneradmin）。期望值也必须先 canonical 再比较，否则在
        // 使用 8.3 短名 profile 路径的 CI runner上会不相等。
        std::error_code ec;
        const std::filesystem::path docRootCanon = std::filesystem::weakly_canonical(docRoot, ec);
        ASSERT_FALSE(ec) << "weakly_canonical(docRoot) failed: " << ec.message();
        const std::filesystem::path expectedPath = (docRootCanon / expectedSuffix).lexically_normal();
        EXPECT_EQ(out.lexically_normal(), expectedPath) << "urlPath=" << urlPath;
    };

    // Safe paths
    check("/index.html", true);
    check("/assets/style.css", true);
    check("/assets/logo%20new.png", true);
    check("/assets/logo+new.png", true);
    check("/assets/missing/deep/file.js", true);
    check("/assets/a..b.png", true);
    checkResolved("/assets/missing/deep/file.js", std::filesystem::path("assets") / "missing" / "deep" / "file.js");
    checkResolved("/assets/a..b.png", std::filesystem::path("assets") / "a..b.png");

    // Malicious paths
    check("../", false);
    check("/../", false);
    check("/assets/../../", false);
    check("/..\\", false);
    check("/%2e%2e/", false);
    check("/assets/%2e%2e%2f", false);
    check("/C:%5cWindows%5cSystem32", false);
    check("/%5c%5cserver%5cshare", false);
    check("//server/share", false);
    check("/%zz%xx", false);

    // Incomplete hex encoding
    check("/%", false);
    check("/%A", false);
    check("/abc%", false);

    // Non-existent base directory
    std::filesystem::path fakeRoot = docRoot.parent_path() / "rk_http_faces_server_path_missing_root";
    std::filesystem::remove_all(fakeRoot, ec);
    std::filesystem::path out;
    bool ok = rk_win::isSafeRelativePath(fakeRoot, "/index.html", out);
    EXPECT_FALSE(ok) << "Expected false for non-existent base directory.";

    // Symlink escape: skip if current environment cannot create the symlink.
    std::filesystem::path outsideRoot = docRoot.parent_path() / "rk_http_faces_server_path_outside_root";
    std::filesystem::remove_all(outsideRoot, ec);
    std::filesystem::create_directories(outsideRoot / "escape", ec);
    std::filesystem::path linkPath = docRoot / "link_outside";
    std::filesystem::remove(linkPath, ec);
    ec.clear();
    std::filesystem::create_directory_symlink(outsideRoot, linkPath, ec);
    if (ec) {
        GTEST_SKIP() << "create_directory_symlink failed: " << ec.message();
    } else {
        check("/link_outside/escape/file.txt", false);
    }
}

// ── Q-CQ-B06-017：客户端线程/句柄回收机制（有界性）────────────────────
#ifdef RK_TEST_SOURCE_ROOT
// 无需制造时序窗口：结构断言保证逐层相对固定目录句柄打开，且拒绝重解析点。
TEST(StaticFileToctouIntegration, OpensRelativeToPinnedRootWithoutReparsePoints) {
    const std::string path = std::string(RK_TEST_SOURCE_ROOT) + "/src/win/src/HttpFacesServerPath.cpp";
    std::ifstream in(path, std::ios::binary);
    std::ostringstream source;
    source << in.rdbuf();
    const std::string code = source.str();
    ASSERT_FALSE(code.empty()) << "无法读取 HttpFacesServerPath.cpp";

    const std::size_t relativeOpenPos = code.find("static HANDLE openRelativeObject");
    const std::size_t rootedAttributesPos = code.find(
        "InitializeObjectAttributes(&attributes, &objectName, OBJ_CASE_INSENSITIVE, parentHandle, nullptr)");
    const std::size_t reparseFlagPos = code.find("FILE_OPEN_REPARSE_POINT", relativeOpenPos);
    const std::size_t rootedWalkPos = code.find("static HANDLE openNoReparsePointRelativeToRoot");
    const std::size_t usePos = code.find("openNoReparsePointRelativeToRoot(rootHandle, path)");
    const std::size_t rootTreePos = code.find("static HANDLE openNoReparseDirectoryTree");
    const std::size_t rootFlagPos = code.find("FILE_FLAG_OPEN_REPARSE_POINT", rootTreePos);
    const std::size_t rootUsePos = code.find("openNoReparseDirectoryTree(docRoot)");
    ASSERT_NE(relativeOpenPos, std::string::npos);
    ASSERT_NE(rootedAttributesPos, std::string::npos);
    ASSERT_NE(reparseFlagPos, std::string::npos);
    ASSERT_NE(rootedWalkPos, std::string::npos);
    ASSERT_NE(usePos, std::string::npos);
    ASSERT_NE(rootTreePos, std::string::npos);
    ASSERT_NE(rootFlagPos, std::string::npos);
    ASSERT_NE(rootUsePos, std::string::npos);
    EXPECT_LT(relativeOpenPos, reparseFlagPos);
    EXPECT_LT(relativeOpenPos, rootedAttributesPos);
    EXPECT_LT(reparseFlagPos, rootedWalkPos);
    EXPECT_LT(rootedWalkPos, usePos);
    EXPECT_LT(rootTreePos, rootFlagPos);
    EXPECT_LT(rootFlagPos, rootUsePos);
    EXPECT_EQ(code.find("GetFinalPathNameByHandleW"), std::string::npos);
}
#endif

// 背景：HttpFacesServer 每个连接一个 std::thread，修复前仅 stop() 才 join，
// 已完成连接的线程句柄随请求总量无限累积（线性增长）。
// 修复后 acceptLoop 每次 accept 后调用 ClientThreadPool::reclaimFinished()，
// 及时 join 已完成线程。
// 仅 win_unit_tests（Windows + OpenCV）运行：core_unit_tests 保持无 std::thread
// 依赖（Linux 下不显式链接 pthread），故非该目标时全部 GTEST_SKIP。
#if defined(_WIN32) && !defined(RK_SKIP_OPENCV)
#include "rk_win/FramePipeline.h"
#include "rk_win/WinJsonConfig.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

TEST(ClientThreadPool, ReclaimsFinishedThreads) {
    rk_win::HttpFacesServer::ClientThreadPool pool;
    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread t([done]() { *done = true; });
        pool.add(std::move(t), std::move(done));
    }
    // 轮询回收直到全部完成（避免尚未调度的线程 done 未置位导致偶发失败）
    std::size_t totalReclaimed = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (pool.size() > 0 && std::chrono::steady_clock::now() < deadline) {
        totalReclaimed += pool.reclaimFinished();
        if (pool.size() == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // 全部线程体已返回 → 全部被 join 并清空
    EXPECT_EQ(totalReclaimed, static_cast<std::size_t>(kCount));
    EXPECT_EQ(pool.size(), static_cast<std::size_t>(0));
}

TEST(ClientThreadPool, RunningThreadNotReclaimedUntilFinished) {
    rk_win::HttpFacesServer::ClientThreadPool pool;
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread t([done]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        *done = true;
    });
    pool.add(std::move(t), std::move(done));
    // 线程仍在运行 → 不应被提前回收（避免 join 长连接导致 acceptLoop 阻塞）
    EXPECT_EQ(pool.reclaimFinished(), static_cast<std::size_t>(0));
    EXPECT_EQ(pool.size(), static_cast<std::size_t>(1));
    // 等待线程结束 → 再次回收应成功
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (pool.size() != 0 && std::chrono::steady_clock::now() < deadline) {
        pool.reclaimFinished();
        if (pool.size() == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(pool.size(), static_cast<std::size_t>(0));
    pool.joinAll();  // 兜底，确保测试无残留线程
}

TEST(ClientThreadPool, BoundedUnderChurn) {
    // 模拟 acceptLoop 节奏：每轮加入并发上限数量的短连接线程，完成后回收。
    // 总量远超并发上限（10 轮 × 32 = 320），但跟踪线程数峰值不应随总量线性增长。
    rk_win::HttpFacesServer::ClientThreadPool pool;
    constexpr int kConcurrent = 32;
    constexpr int kRounds = 10;
    std::size_t maxSeen = 0;
    for (int round = 0; round < kRounds; ++round) {
        for (int i = 0; i < kConcurrent; ++i) {
            auto done = std::make_shared<std::atomic<bool>>(false);
            std::thread t([done]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                *done = true;
            });
            pool.add(std::move(t), std::move(done));
        }
        maxSeen = std::max(maxSeen, pool.size());
        EXPECT_LE(pool.size(), static_cast<std::size_t>(kConcurrent));
        // 等待该轮线程完成并被回收
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (pool.size() > 0 && std::chrono::steady_clock::now() < deadline) {
            pool.reclaimFinished();
            if (pool.size() == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        EXPECT_EQ(pool.size(), static_cast<std::size_t>(0)) << "第 " << round << " 轮结束后线程未被回收";
    }
    // 有界断言：峰值 ≤ 并发数，而非随总请求量（320）线性增长
    EXPECT_LE(maxSeen, static_cast<std::size_t>(kConcurrent));
}

// ── 真实服务端连接洪泛 RED 测试 ────────────────────────────────────────
// RED 判别逻辑：发起远超并发配额（64）的请求量，断言跟踪线程数长期有界。
// 修复前 acceptLoop 只增不减（仅 stop() 才 join），clientThreads_.size() 随请求数
// 线性增长（≈400），断言必然失败；修复后每 accept 回收已完成线程，跟踪数回落并
// 稳定 ≤ 并发上限 + 余量，断言通过。
TEST(HttpFacesServer, ClientThreadsBoundedAfterConnectionFlood) {
    constexpr int kFloodConnections = 400;  // 远超 ConnectionQuota 上限 64

    WSADATA wsa;
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &wsa), 0);

    // 找空闲端口：先 bind(0) 让系统分配，再关闭交给 HttpFacesServer
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(probe, INVALID_SOCKET);
    sockaddr_in probeAddr{};
    probeAddr.sin_family = AF_INET;
    probeAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(probe, reinterpret_cast<sockaddr*>(&probeAddr), sizeof(probeAddr)), 0);
    int len = sizeof(probeAddr);
    ASSERT_EQ(getsockname(probe, reinterpret_cast<sockaddr*>(&probeAddr), &len), 0);
    const int port = ntohs(probeAddr.sin_port);
    closesocket(probe);

    rk_win::HttpFacesServer server;
    ASSERT_TRUE(server.start(nullptr, nullptr, port, nullptr))
        << "HttpFacesServer 启动失败 port=" << port;

    // 洪泛：每个连接发一个普通 GET（命中 404 路径，不触达 pipe/settings）后关闭
    for (int i = 0; i < kFloodConnections; ++i) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ASSERT_NE(s, INVALID_SOCKET);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            closesocket(s);  // 配额满时服务器直接关闭连接；继续下一轮
            continue;
        }
        const char* req = "GET /rk-flood-no-such-file HTTP/1.1\r\nHost: t\r\n\r\n";
        send(s, req, static_cast<int>(std::strlen(req)), 0);
        char buf[256];
        while (recv(s, buf, sizeof(buf), 0) > 0) {}  // 读到 EOF（Connection: close）
        closesocket(s);
    }

    // 等待所有客户端连接处理完成
    const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (server.activeClientCount() > 0 && std::chrono::steady_clock::now() < drainDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(server.activeClientCount(), 0) << "连接洪泛后仍有活动客户端";

    // 长期有界断言：跟踪线程数（= 未回收线程句柄数）不随请求总量线性增长。
    // 修复前恒为 400（线性）；修复后 ≤ 并发上限 64 + 余量。
    constexpr int kTrackedThreadBound = 128;  // 2 × MAX_CONCURRENT_CONNECTIONS
    const auto boundDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (server.clientThreadCount() > static_cast<std::size_t>(kTrackedThreadBound) &&
           std::chrono::steady_clock::now() < boundDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_LE(server.clientThreadCount(), static_cast<std::size_t>(kTrackedThreadBound))
        << "跟踪客户端线程数超出有界上限（泄漏：已完成线程/句柄未被回收）";

    server.stop();
    WSACleanup();
}

// ── S-SEC-B09-001：HTTP 认证授权（Bearer Token）────────────────────
// 认证模型冻结于 docs/security/auth-model.md：
//  - token 来源 = 环境变量 RK_WCFR_HTTP_AUTH_TOKEN（start() 时读取）；
//  - 未配置 token = 兼容模式（匿名放行，残余风险见规格 §3.3）；
//  - 匿名白名单 = 静态文件 + /api/v1/health + /api/v1/acceleration；
//  - 其余一切 /api 请求（含 SSE/MJPEG 流）须携带有效 Bearer token，否则 401。

namespace {

// RAII：设置/清除环境变量，析构时清除（value==nullptr 表示清除），避免跨用例污染。
struct AuthEnvGuard {
    const wchar_t* name_;
    AuthEnvGuard(const wchar_t* name, const wchar_t* value) : name_(name) {
        SetEnvironmentVariableW(name_, value);  // nullptr → 删除变量
    }
    ~AuthEnvGuard() { SetEnvironmentVariableW(name_, nullptr); }
};

// 找空闲端口：bind(0) 由系统分配后关闭，交给 HttpFacesServer（与洪泛用例同法）。
int findFreePort() {
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (probe == INVALID_SOCKET) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(probe);
        return 0;
    }
    int len = sizeof(addr);
    if (getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        closesocket(probe);
        return 0;
    }
    const int port = ntohs(addr.sin_port);
    closesocket(probe);
    return port;
}

struct RawHttpResult {
    int status = 0;
    std::string response;
    bool connected = false;
};

// 发送原始 HTTP 请求并读取完整响应（非流式响应均为 Connection: close，读到 EOF）。
RawHttpResult rawHttpExchange(int port, const std::string& request) {
    RawHttpResult out;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return out;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return out;
    }
    out.connected = true;
    (void)send(s, request.data(), static_cast<int>(request.size()), 0);
    char buf[4096];
    int n;
    while ((n = recv(s, buf, static_cast<int>(sizeof(buf)), 0)) > 0) {
        out.response.append(buf, buf + n);
    }
    closesocket(s);
    const auto sp = out.response.find(' ');
    if (sp != std::string::npos) {
        out.status = std::atoi(out.response.c_str() + static_cast<long>(sp) + 1);
    }
    return out;
}

// 设置 token 环境变量 → 启动真实服务端 → 执行一次 HTTP 请求 → 停止。
// tokenValue==nullptr 表示"未配置 token"（兼容模式）。
RawHttpResult runHttpRequest(const wchar_t* tokenValue, const std::string& request,
                             rk_win::WinJsonConfigStore* store) {
    AuthEnvGuard tokenGuard(L"RK_WCFR_HTTP_AUTH_TOKEN", tokenValue);
    // findFreePort 使用 socket()，须先 WSAStartup（与洪泛用例一致；HttpFacesServer 内部
    // 的静态 WsaGuard 也会再调用一次，Winsock 引用计数递增，进程内无害）。
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return RawHttpResult{};
    }
    const int port = findFreePort();
    rk_win::HttpFacesServer server;
    if (port == 0 || !server.start(nullptr, nullptr, port, store)) {
        return RawHttpResult{};
    }
    RawHttpResult r = rawHttpExchange(port, request);
    server.stop();
    return r;
}

// 构造临时 config.json 的 WinJsonConfigStore（PUT settings 用例需要真实存储）。
std::unique_ptr<rk_win::WinJsonConfigStore> makeAuthConfigStore(const std::wstring& name) {
    const auto base = std::filesystem::temp_directory_path() / L"rk3288_opencv_win_auth_tests" / name;
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base, ec);
    const auto cfgPath = base / L"config.json";
    AuthEnvGuard cfgGuard(L"RK_WCFR_CONFIG", cfgPath.c_str());
    auto store = std::make_unique<rk_win::WinJsonConfigStore>();
    std::string warn;
    (void)store->initialize(warn);
    return store;
}

}  // namespace

// exploit 证据（兼容模式）：未配置 token 时，无 Authorization 头的管理写请求先成功。
// 修复前后均返回 200 —— 证明"未认证写可达"是真实存在的状态，并记录 auth-model.md §3.3
// 的残余风险（未配置 token = 匿名兼容）。
TEST(HttpFacesAuth, AnonymousWriteReachableWithoutToken) {
    auto store = makeAuthConfigStore(L"anonymous_write");
    const std::string body = "{\"http\":{\"port\":8090}}";
    const std::string req = "PUT /api/v1/settings HTTP/1.1\r\nHost: t\r\n"
                            "Content-Type: application/json\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    const auto r = runHttpRequest(nullptr, req, store.get());
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 200) << r.response;
}

// RED→GREEN 主判别：配置 token 后，未认证 PUT settings 必须 401。
// 修复前：无鉴权 → 200（本用例失败=RED）；修复后：401（GREEN）。
TEST(HttpFacesAuth, UnauthenticatedWriteRejectedWhenAuthEnabled) {
    auto store = makeAuthConfigStore(L"unauth_write_reject");
    const std::string body = "{\"http\":{\"port\":8090}}";
    const std::string req = "PUT /api/v1/settings HTTP/1.1\r\nHost: t\r\n"
                            "Content-Type: application/json\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    const auto r = runHttpRequest(L"test-secret-token", req, store.get());
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 401) << r.response;
}

// RED→GREEN：配置 token 后，未认证 POST 动作（enroll）必须 401。
TEST(HttpFacesAuth, UnauthenticatedActionRejectedWhenAuthEnabled) {
    const std::string body = "{\"personId\":\"test\"}";
    const std::string req = "POST /api/v1/actions/enroll HTTP/1.1\r\nHost: t\r\n"
                            "Content-Type: application/json\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    const auto r = runHttpRequest(L"test-secret-token", req, nullptr);
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 401) << r.response;
}

// RED→GREEN：错误 token 必须 401，且 401 体须符合规格 §3.4/§6：
// 含 unauthorized 错误码、不发送 WWW-Authenticate、不回显请求中的 token。
TEST(HttpFacesAuth, WrongBearerTokenRejected) {
    const std::string req = "GET /api/v1/models HTTP/1.1\r\nHost: t\r\n"
                            "Authorization: Bearer wrong-token\r\n\r\n";
    const auto r = runHttpRequest(L"test-secret-token", req, nullptr);
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 401) << r.response;
    EXPECT_NE(r.response.find("\"unauthorized\""), std::string::npos) << r.response;
    EXPECT_EQ(r.response.find("WWW-Authenticate"), std::string::npos) << r.response;
    EXPECT_EQ(r.response.find("test-secret-token"), std::string::npos) << r.response;
}

// 安全要求：有效 token 放行（管理读端点返回 200）。
TEST(HttpFacesAuth, ValidBearerTokenAccepted) {
    const std::string req = "GET /api/v1/models HTTP/1.1\r\nHost: t\r\n"
                            "Authorization: Bearer test-secret-token\r\n\r\n";
    const auto r = runHttpRequest(L"test-secret-token", req, nullptr);
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 200) << r.response;
}

// 匿名白名单：health / acceleration 在启用认证后仍无需 token。
TEST(HttpFacesAuth, AnonymousWhitelistRemainsPublic) {
    {
        const std::string req = "GET /api/v1/health HTTP/1.1\r\nHost: t\r\n\r\n";
        const auto r = runHttpRequest(L"test-secret-token", req, nullptr);
        ASSERT_TRUE(r.connected);
        EXPECT_EQ(r.status, 200) << r.response;
    }
    {
        const std::string req = "GET /api/v1/acceleration HTTP/1.1\r\nHost: t\r\n\r\n";
        const auto r = runHttpRequest(L"test-secret-token", req, nullptr);
        ASSERT_TRUE(r.connected);
        EXPECT_EQ(r.status, 200) << r.response;
    }
}

// RED→GREEN：SSE 流（StreamSessionRunner 旁路）在启用认证后必须 401。
// 修复前：无鉴权直通旁路 → pipe=null 返回 503（本用例失败=RED）；修复后：401。
TEST(HttpFacesAuth, UnauthenticatedStreamRejectedWhenAuthEnabled) {
    const std::string req = "GET /api/v1/faces/stream HTTP/1.1\r\nHost: t\r\n\r\n";
    const auto r = runHttpRequest(L"test-secret-token", req, nullptr);
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 401) << r.response;
}

// ── S-SEC-B09-004：跨站请求伪造（CSRF）防护 ─────────────────────
// 决策树（冻结于 docs/security/auth-model.md §3.5）：
//  - 仅对写方法（POST/PUT/DELETE）校验；读方法与 GET 不受影响；
//  - Sec-Fetch-Site: cross-site → 拒绝（现代浏览器强信号，独立于 Origin）；
//  - Sec-Fetch-Site 缺失（旧浏览器）：Origin 存在且 host 非本机回环 → 拒绝；
//    字面量 "null"（沙箱 iframe / data: 等无源上下文）→ 拒绝；
//  - Origin/Sec-Fetch-Site 均缺失（curl / API 客户端 / 旧浏览器）→ 放行；
//  - 同源写（Origin: http://127.0.0.1:* 或 http://localhost:*）→ 放行。
// 所有写请求均携带有效 Bearer token，以隔离"认证已通过、仅 CSRF 决定成败"。

// RED→GREEN 主判别：跨站 Origin 的已认证写请求必须 403。
// 修复前：无 CSRF 校验 → 200（本用例失败=RED）；修复后：403（GREEN）。
TEST(HttpFacesAuth, CrossSiteOriginWriteRejected) {
    auto store = makeAuthConfigStore(L"csrf_cross_site_origin");
    const std::string body = "{\"http\":{\"port\":8090}}";
    const std::string req = "PUT /api/v1/settings HTTP/1.1\r\nHost: t\r\n"
                            "Authorization: Bearer test-secret-token\r\n"
                            "Origin: https://evil.example\r\n"
                            "Content-Type: application/json\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    const auto r = runHttpRequest(L"test-secret-token", req, store.get());
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 403) << r.response;
}

// 跨站表单 POST（application/x-www-form-urlencoded，无 CORS 预检的典型 CSRF 载体）
// 同样必须 403：浏览器强制携带 Origin，host 为攻击源。
TEST(HttpFacesAuth, CrossSiteFormPostRejected) {
    const std::string body = "personId=attacker";
    const std::string req = "POST /api/v1/actions/enroll HTTP/1.1\r\nHost: t\r\n"
                            "Authorization: Bearer test-secret-token\r\n"
                            "Origin: https://evil.example\r\n"
                            "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    const auto r = runHttpRequest(L"test-secret-token", req, nullptr);
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 403) << r.response;
}

// 现代浏览器强信号：即便不带 Origin，Sec-Fetch-Site: cross-site 也必须 403。
TEST(HttpFacesAuth, CrossSiteSecFetchSiteRejected) {
    auto store = makeAuthConfigStore(L"csrf_cross_site_sec_fetch");
    const std::string body = "{\"http\":{\"port\":8090}}";
    const std::string req = "PUT /api/v1/settings HTTP/1.1\r\nHost: t\r\n"
                            "Authorization: Bearer test-secret-token\r\n"
                            "Sec-Fetch-Site: cross-site\r\n"
                            "Content-Type: application/json\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    const auto r = runHttpRequest(L"test-secret-token", req, store.get());
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 403) << r.response;
}

// 合法同源写（SPA 由本服务以 127.0.0.1 托管）：Origin 为 http://127.0.0.1:* → 放行 200。
TEST(HttpFacesAuth, SameOriginLoopbackWriteAccepted) {
    auto store = makeAuthConfigStore(L"csrf_same_origin_127");
    const std::string body = "{\"http\":{\"port\":8090}}";
    const std::string req = "PUT /api/v1/settings HTTP/1.1\r\nHost: t\r\n"
                            "Authorization: Bearer test-secret-token\r\n"
                            "Origin: http://127.0.0.1:8080\r\n"
                            "Content-Type: application/json\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    const auto r = runHttpRequest(L"test-secret-token", req, store.get());
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 200) << r.response;
}

// 合法同源写（localhost 别名）：Origin 为 http://localhost:* → 放行 200。
TEST(HttpFacesAuth, SameOriginLocalhostWriteAccepted) {
    auto store = makeAuthConfigStore(L"csrf_same_origin_localhost");
    const std::string body = "{\"http\":{\"port\":8090}}";
    const std::string req = "PUT /api/v1/settings HTTP/1.1\r\nHost: t\r\n"
                            "Authorization: Bearer test-secret-token\r\n"
                            "Origin: http://localhost:9000\r\n"
                            "Content-Type: application/json\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    const auto r = runHttpRequest(L"test-secret-token", req, store.get());
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 200) << r.response;
}

// 同源写带 Sec-Fetch-Site: same-origin → 放行（非 cross-site 均放行）。
TEST(HttpFacesAuth, SameOriginSecFetchSiteAccepted) {
    auto store = makeAuthConfigStore(L"csrf_same_origin_sec_fetch");
    const std::string body = "{\"http\":{\"port\":8090}}";
    const std::string req = "PUT /api/v1/settings HTTP/1.1\r\nHost: t\r\n"
                            "Authorization: Bearer test-secret-token\r\n"
                            "Origin: http://127.0.0.1:8080\r\n"
                            "Sec-Fetch-Site: same-origin\r\n"
                            "Content-Type: application/json\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    const auto r = runHttpRequest(L"test-secret-token", req, store.get());
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 200) << r.response;
}

// curl / API 客户端兼容：无 Origin / 无 Sec-Fetch-Site 的写请求照常放行（不破坏 SEC-02）。
TEST(HttpFacesAuth, HeadlessClientWriteAcceptedWithoutOrigin) {
    auto store = makeAuthConfigStore(L"csrf_headless_client");
    const std::string body = "{\"http\":{\"port\":8090}}";
    const std::string req = "PUT /api/v1/settings HTTP/1.1\r\nHost: t\r\n"
                            "Authorization: Bearer test-secret-token\r\n"
                            "Content-Type: application/json\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    const auto r = runHttpRequest(L"test-secret-token", req, store.get());
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 200) << r.response;
}

// ── SEC-02：认证畸形 Authorization 头 fail-closed（RFC 6750 / auth-model.md §3.4）────
// requestAuthorized 的解析契约（生产实现 HttpFacesServer.cpp）：
//  - 仅接受 `Bearer` scheme（RFC 6750 scheme 大小写不敏感）；
//  - 分隔符必须是单个空格：无空格 / tab 分隔都视为畸形 → 拒绝；
//  - 多个 Authorization 头只取第一个（读取时 break），首条畸形即拒绝。
// 所有用例均启用认证（配置 token），验证畸形头一律 401，绝不因畸形而放行。

// Basic scheme（合法 HTTP 认证 scheme，但本项目只接受 Bearer）→ 401。
TEST(HttpFacesAuth, BasicSchemeRejected) {
    const std::string req = "GET /api/v1/models HTTP/1.1\r\nHost: t\r\n"
                            "Authorization: Basic dXNlcjpwYXNz\r\n\r\n";
    const auto r = runHttpRequest(L"test-secret-token", req, nullptr);
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 401) << r.response;
    EXPECT_NE(r.response.find("\"unauthorized\""), std::string::npos) << r.response;
}

// `Bearer` 无空格无 token（find(' ') 为 npos）→ 401。
TEST(HttpFacesAuth, BearerWithoutSpaceRejected) {
    const std::string req = "GET /api/v1/models HTTP/1.1\r\nHost: t\r\n"
                            "Authorization: Bearer\r\n\r\n";
    const auto r = runHttpRequest(L"test-secret-token", req, nullptr);
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 401) << r.response;
}

// 大写 scheme `BEARER`：RFC 6750 规定 scheme 大小写不敏感，故 `BEARER <正确 token>`
// 会被接受（生产注释明确此契约）。本用例验证的 fail-closed 性质是：大写 scheme 不构成
// 绕过——携带错误 token 时仍必须 401；同时以正确 token 断言大写 scheme 不破坏 RFC 契约。
TEST(HttpFacesAuth, UppercaseSchemeDoesNotBypassTokenCheck) {
    {
        const std::string req = "GET /api/v1/models HTTP/1.1\r\nHost: t\r\n"
                                "Authorization: BEARER wrong-token\r\n\r\n";
        const auto r = runHttpRequest(L"test-secret-token", req, nullptr);
        ASSERT_TRUE(r.connected);
        EXPECT_EQ(r.status, 401) << r.response;
    }
    {
        const std::string req = "GET /api/v1/models HTTP/1.1\r\nHost: t\r\n"
                                "Authorization: BEARER test-secret-token\r\n\r\n";
        const auto r = runHttpRequest(L"test-secret-token", req, nullptr);
        ASSERT_TRUE(r.connected);
        EXPECT_EQ(r.status, 200) << r.response;
    }
}

// tab 分隔 `Bearer\t<token>`：解析器只认空格，tab 使 find(' ') 为 npos →
// 即使 token 正确也畸形 → 401（fail-closed）。
TEST(HttpFacesAuth, TabSeparatedSchemeRejected) {
    const std::string req = "GET /api/v1/models HTTP/1.1\r\nHost: t\r\n"
                            "Authorization: Bearer\ttest-secret-token\r\n\r\n";
    const auto r = runHttpRequest(L"test-secret-token", req, nullptr);
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 401) << r.response;
}

// 多个 Authorization 头：读取仅取第一个，首条为畸形 Basic 时即使后随有效 Bearer 也必须
// 401 —— 头歧义不得通过"追加一条有效头"绕过校验（fail-closed）。
TEST(HttpFacesAuth, DuplicateAuthorizationMalformedFirstRejected) {
    const std::string req = "GET /api/v1/models HTTP/1.1\r\nHost: t\r\n"
                            "Authorization: Basic dXNlcjpwYXNz\r\n"
                            "Authorization: Bearer test-secret-token\r\n\r\n";
    const auto r = runHttpRequest(L"test-secret-token", req, nullptr);
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 401) << r.response;
}

// ── SEC-07：CSRF 与认证正交（auth-model.md §3.5）────────────────────
// 兼容模式（未配置 token = 匿名放行）只豁免"认证"，不豁免 CSRF：跨站 Origin 的写请求
// 在无 token 配置下同样必须 403。判别点：无 Authorization 头 + 跨站 Origin 的 POST。
TEST(HttpFacesAuth, CrossSiteWriteRejectedWithoutAuthToken) {
    const std::string body = "personId=attacker";
    const std::string req = "POST /api/v1/actions/enroll HTTP/1.1\r\nHost: t\r\n"
                            "Origin: https://evil.example\r\n"
                            "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    const auto r = runHttpRequest(nullptr, req, nullptr);
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.status, 403) << r.response;
    EXPECT_NE(r.response.find("\"cross_site_forbidden\""), std::string::npos) << r.response;
}

// ── S-SEC-B09-005：流会话配额（FPS 预算 / 字节配额 / 会话超时）────────────────────
// 背景：全局 ConnectionQuota=64 只限制"并发连接数"，不限制单个流会话消耗的
// CPU / 字节 / 时长 —— MJPEG 可被恶意客户端迫使按源帧率（30+ fps）连续 JPEG 编码；
// SSE/MJPEG 会话可被无限期持有。本组用例直接驱动 StreamSessionRunner
// （真实 Winsock socket 对 + 合成帧提供器），以极小注入配额验证三种机制：
//  - FPS 预算：超过预算的写入被节流（帧间最小间隔 1000/fps ms）；
//  - 字节配额：单会话累计写字节超限 → 拒绝写入并终止会话；
//  - 会话超时：存活超过 maxSessionDuration → 自动断开（SSE/MJPEG 共用）。
// 生产默认值（mjpegMaxFps=20、8GiB 字节回退、30min 时长）及产品依据见
// StreamLimits 注释与 docs/security/auth-model.md §8。

namespace {

// 建立本机回环 socket 对：serverSock 供 StreamSessionRunner 写入，clientSock 供测试读取。
// 与 findFreePort 同法（bind(0) + listen + connect + accept），避免依赖固定测试端口。
bool makeLoopbackSocketPair(SOCKET& serverSock, SOCKET& clientSock) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(ls, 1) != 0) {
        closesocket(ls);
        return false;
    }
    int len = sizeof(addr);
    if (getsockname(ls, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        closesocket(ls);
        return false;
    }
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c == INVALID_SOCKET) { closesocket(ls); return false; }
    if (connect(c, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(c);
        closesocket(ls);
        return false;
    }
    SOCKET s = accept(ls, nullptr, nullptr);
    closesocket(ls);
    if (s == INVALID_SOCKET) { closesocket(c); return false; }
    serverSock = s;
    clientSock = c;
    return true;
}

// 设置客户端接收超时：recv 在无数据时周期返回，令"等待 runnerDone / 截止时间"
// 的循环得以推进，避免服务端停止后 recv 永久阻塞。
void setRecvTimeoutMs(SOCKET s, DWORD ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

// 读取 socket 直到连接关闭或超时（超时/无数据时继续，截止时间到才返回）。
std::string recvUntilDeadline(SOCKET s, std::chrono::milliseconds windowMs) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + windowMs;
    char buf[4096];
    while (std::chrono::steady_clock::now() < deadline) {
        const int n = recv(s, buf, static_cast<int>(sizeof(buf)), 0);
        if (n > 0) out.append(buf, buf + n);
        else if (n == 0) break;  // 对端关闭
    }
    return out;
}

}  // namespace

// RED→GREEN 判别 1：超过 FPS 预算的写入被节流。
// 修复前 runMjpeg 无帧间最小间隔 → 合成提供器立即出帧，1.5s 内编码/写入数百帧；
// 修复后 ≤ 预算帧率（fps=10 → 100ms 间隔）→ 1.5s 内 ≤ ~15 帧。
TEST(HttpFacesStreamQuota, MjpegFpsThrottle) {
    WSADATA wsaData;
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &wsaData), 0);
    SOCKET serverSock = INVALID_SOCKET, clientSock = INVALID_SOCKET;
    ASSERT_TRUE(makeLoopbackSocketPair(serverSock, clientSock));
    setRecvTimeoutMs(clientSock, 50);

    cv::Mat frame(48, 64, CV_8UC3, cv::Scalar(90, 60, 30));
    std::atomic<bool> running{true};
    rk_win::StreamLimits limits;
    limits.mjpegMaxFps = 10;  // 帧间最小间隔 100ms（生产默认 20fps，测试注入小预算加速判别）
    rk_win::StreamSessionRunner runner(
        running, [&](cv::Mat& out) { out = frame; return true; }, limits);

    std::thread th([&]() {
        runner.run(static_cast<std::uintptr_t>(serverSock), rk_win::StreamType::Mjpeg);
    });

    const std::string data = recvUntilDeadline(clientSock, std::chrono::milliseconds(1500));

    running = false;
    th.join();
    closesocket(serverSock);
    closesocket(clientSock);
    WSACleanup();

    // 帧边界计数：multipart 每帧部分以 "--rk_boundary\r\n" 开头（响应头仅含
    // "boundary=rk_boundary"，无前导 --，不计数）。
    const std::string boundary = "--rk_boundary";
    int parts = 0;
    for (std::size_t pos = 0; (pos = data.find(boundary, pos)) != std::string::npos;) {
        ++parts;
        pos += boundary.size();
    }
    // 正常流可用：节流下仍持续出帧（1.5s / 100ms ≈ 15 帧）
    EXPECT_GE(parts, 3) << "正常 MJPEG 流未工作（无帧到达）";
    // 资源有界：写入被节流在预算内（≤ 窗口/间隔 + 首帧即时 + 调度抖动余量）
    EXPECT_LE(parts, 20) << "超过 FPS 预算的写入未被节流 parts=" << parts;
}

// RED→GREEN 判别 2：单会话字节配额耗尽 → 拒绝写入并终止会话。
// 修复前无字节记账 → 会话持续编码/写入直至 running=false（runnerDone 不置位 = RED）。
TEST(HttpFacesStreamQuota, MjpegByteQuotaTerminatesSession) {
    WSADATA wsaData;
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &wsaData), 0);
    SOCKET serverSock = INVALID_SOCKET, clientSock = INVALID_SOCKET;
    ASSERT_TRUE(makeLoopbackSocketPair(serverSock, clientSock));
    setRecvTimeoutMs(clientSock, 100);

    cv::Mat frame(48, 64, CV_8UC3, cv::Scalar(90, 60, 30));
    std::atomic<bool> running{true};
    std::atomic<bool> runnerDone{false};
    rk_win::StreamLimits limits;
    limits.maxBytesPerSession = 4096;  // 极小配额：数百字节后即耗尽（生产默认 8GiB）
    rk_win::StreamSessionRunner runner(
        running, [&](cv::Mat& out) { out = frame; return true; }, limits);

    std::thread th([&]() {
        runner.run(static_cast<std::uintptr_t>(serverSock), rk_win::StreamType::Mjpeg);
        runnerDone = true;
    });

    std::string data;
    char buf[4096];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !runnerDone.load()) {
        const int n = recv(clientSock, buf, static_cast<int>(sizeof(buf)), 0);
        if (n > 0) data.append(buf, buf + n);
        else if (n == 0) break;
    }

    running = false;
    th.join();
    closesocket(serverSock);
    closesocket(clientSock);

    // 会话被字节配额终止（资源有界，而非依赖 running=false 外力停止）
    EXPECT_TRUE(runnerDone.load()) << "字节配额未生效：会话未自行终止";
    // 写出的数据不超配额（含头部/半帧余量）
    EXPECT_LE(data.size(), static_cast<std::size_t>(limits.maxBytesPerSession) + 1024)
        << "超出字节配额仍被写出 size=" << data.size();
    // 正常流可用：终止前已正常送出 MJPEG 响应头（配额内先服务、超限即断）
    EXPECT_NE(data.find("HTTP/1.1 200 OK"), std::string::npos) << "未收到 MJPEG 响应头";
    WSACleanup();
}

// RED→GREEN 判别 3：SSE/流超时终止 —— 会话存活超过 maxSessionDuration 自动断开。
// 修复前 runSse 无时长检查 → 会话无限持有（runnerDone 不置位 = RED）；修复后到期断开。
TEST(HttpFacesStreamQuota, SseDurationTerminatesSession) {
    WSADATA wsaData;
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &wsaData), 0);
    SOCKET serverSock = INVALID_SOCKET, clientSock = INVALID_SOCKET;
    ASSERT_TRUE(makeLoopbackSocketPair(serverSock, clientSock));
    setRecvTimeoutMs(clientSock, 100);

    std::atomic<bool> running{true};
    std::atomic<bool> runnerDone{false};
    rk_win::StreamLimits limits;
    limits.maxSessionDuration = std::chrono::milliseconds(250);  // 生产默认 30min
    rk_win::StreamSessionRunner runner(
        running, [](cv::Mat&) { return false; }, limits);

    rk_win::FramePipeline pipe;  // 默认构造（无相机/识别）：facesSeq 恒 0 → keepalive 路径
    runner.setPipe(&pipe);

    std::thread th([&]() {
        runner.run(static_cast<std::uintptr_t>(serverSock), rk_win::StreamType::Sse);
        runnerDone = true;
    });

    // 仅消费数据、轮询 runnerDone，直到会话自行终止或超时。
    // 关键：此阶段不触碰 running_ —— 只有"时长配额使 run() 自行返回"才能置位
    // runnerDone；否则（无时长检查）runnerDone 恒 false → 判别为 RED。
    std::string data;
    char buf[4096];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !runnerDone.load()) {
        const int n = recv(clientSock, buf, static_cast<int>(sizeof(buf)), 0);
        if (n > 0) data.append(buf, buf + n);
    }
    // 记录"未外力干预（running_ 仍为 true）"时的终止状态
    const bool terminatedByTimeout = runnerDone.load();

    running = false;  // 兜底：确保线程可退出（未自行终止时）
    th.join();
    closesocket(serverSock);
    closesocket(clientSock);

    // 超时终止生效（独立于全局 ConnectionQuota 的流级时长限制）
    EXPECT_TRUE(terminatedByTimeout) << "会话超时未生效：SSE 会话未自行终止";
    // 正常流可用：终止前已正常送出 SSE 响应头
    EXPECT_NE(data.find("HTTP/1.1 200 OK"), std::string::npos) << "SSE 未收到响应头";
    EXPECT_NE(data.find("text/event-stream"), std::string::npos) << "SSE 响应头缺 event-stream";
    WSACleanup();
}

// ── S-SEC-B09-007：静态文件 TOCTOU（校验与打开两步非原子）────────────────
// 背景：isSafeRelativePath 先对「路径字符串」做 weakly_canonical + 祖先校验（确认在
// webroot 内），随后旧 readFileBinary 按路径字符串独立 open —— 校验与打开是两次
// 独立系统调用。攻击者可在校验后、open 前把路径替换为指向 webroot 外的 symlink
// （symlink-swap TOCTOU）→ 越界打开。修复为固定 webroot 句柄，并从该句柄逐层
// 相对打开路径分量；每层都打开重解析点本身并拒绝，不再依赖可变路径字符串校验。

namespace {

struct ToctouFixture {
    std::filesystem::path base;
    std::filesystem::path webroot;
    std::filesystem::path outside;
    std::filesystem::path secret;
};

// 构造临时 webroot（含 sub/）与 webroot 外的 secret 文件（含唯一标记串）。
bool makeToctouFixture(ToctouFixture& f) {
    f.base = std::filesystem::temp_directory_path() / L"rk_toctou_s_sec_b09_007";
    std::error_code ec;
    std::filesystem::remove_all(f.base, ec);
    ec.clear();
    f.webroot = f.base / L"webroot";
    f.outside = f.base / L"outside";
    std::filesystem::create_directories(f.webroot / L"sub", ec);
    std::filesystem::create_directories(f.outside, ec);
    if (ec) return false;
    f.secret = f.outside / L"secret.txt";
    {
        std::ofstream ofs(f.secret);
        ofs << "TOCTOU_SECRET_MARKER";
    }
    return true;
}

// Win10 1703+ 且开启开发者模式（或管理员）时，非特权进程可创建 symlink，但必须带
// SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE；旧 SDK 缺失该宏时回退为常量 0x2。
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

// 在 link 处创建指向 target 的 file symlink；受限环境（无开发者模式/非管理员）则失败。
bool tryCreateFileSymlink(const std::filesystem::path& link,
                          const std::filesystem::path& target,
                          DWORD& lastError) {
    std::error_code ec;
    std::filesystem::remove(link, ec);
    SetLastError(ERROR_SUCCESS);
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(),
                            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0) {
        lastError = ERROR_SUCCESS;
        return true;
    }
    lastError = GetLastError();
    return false;
}

bool tryCreateDirectorySymlink(const std::filesystem::path& link,
                               const std::filesystem::path& target,
                               DWORD& lastError) {
    std::error_code ec;
    std::filesystem::remove(link, ec);
    SetLastError(ERROR_SUCCESS);
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(),
                            SYMBOLIC_LINK_FLAG_DIRECTORY |
                                SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0) {
        lastError = ERROR_SUCCESS;
        return true;
    }
    lastError = GetLastError();
    return false;
}

}  // namespace

// 服务启动阶段也不得跟随 webroot 自身或其祖先中的 reparse point。
TEST(StaticFileToctou, StaticFileRootRejectsReparsePointInConfiguredRoot) {
    ToctouFixture f;
    ASSERT_TRUE(makeToctouFixture(f));
    const std::filesystem::path rootLink = f.base / L"webroot-link";
    DWORD symlinkError = ERROR_SUCCESS;
    if (!tryCreateDirectorySymlink(rootLink, f.webroot, symlinkError)) {
        GTEST_SKIP() << "无法创建目录 symlink（GetLastError=" << symlinkError << "，需开发者模式或管理员权限）";
    }

    rk_win::StaticFileRoot root;
    EXPECT_FALSE(root.open(rootLink));
}

TEST(StaticFileToctou, StaticFileRootRejectsReparsePointInRootAncestors) {
    ToctouFixture f;
    ASSERT_TRUE(makeToctouFixture(f));
    const std::filesystem::path parentLink = f.base / L"parent-link";
    DWORD symlinkError = ERROR_SUCCESS;
    if (!tryCreateDirectorySymlink(parentLink, f.base, symlinkError)) {
        GTEST_SKIP() << "无法创建目录 symlink（GetLastError=" << symlinkError << "，需开发者模式或管理员权限）";
    }

    rk_win::StaticFileRoot root;
    EXPECT_FALSE(root.open(parentLink / L"webroot"));
}

// RED 证据（正对照，修复前后恒通过）：确定性复现 TOCTOU 窗口 ——
// 先对「尚不存在的路径」做 isSafeRelativePath（resolveWithExistingAncestor 词法补全，
// 校验通过并返回 webroot 内路径），再在 open 前把该路径替换为指向 webroot 外的
// symlink，随后按当前旧读取方式（std::ifstream 按路径字符串 open）读到越界内容。
// 证明「校验字符串 + 独立 open」两步非原子：校验通过 ≠ open 安全。
TEST(StaticFileToctou, ValidatedPathSwappedOutsideIsReadableByRawOpen) {
    ToctouFixture f;
    ASSERT_TRUE(makeToctouFixture(f));

    // 1) 校验阶段：link 尚不存在 → isSafeRelativePath 对词法路径放行。
    std::filesystem::path resolved;
    ASSERT_TRUE(rk_win::isSafeRelativePath(f.webroot, "/sub/link.txt", resolved));

    // 2) open 前 swap：在已校验路径处创建指向 webroot 外的 symlink。
    DWORD symlinkError = ERROR_SUCCESS;
    if (!tryCreateFileSymlink(resolved, f.secret, symlinkError)) {
        GTEST_SKIP() << "无法创建 symlink（GetLastError=" << symlinkError << "，需开发者模式或管理员权限）";
    }

    // 3) 旧读取方式（按路径字符串 open）：跟随 symlink 读到越界内容。
    std::ostringstream oss;
    {
        std::ifstream ifs(resolved, std::ios::binary);
        oss << ifs.rdbuf();
    }
    EXPECT_NE(oss.str().find("TOCTOU_SECRET_MARKER"), std::string::npos)
        << "校验后、open 前被替换为越界 symlink 的路径仍可被原样打开（TOCTOU 窗口）";
}

// 即使受限环境无法创建 symlink，也验证读取函数拒绝绝对路径直接越界。
// naive 版本忽略 docRoot，会在此处读出 secret。
TEST(StaticFileToctou, ReadFileWithinRootRejectsDirectOutsidePath) {
    ToctouFixture f;
    ASSERT_TRUE(makeToctouFixture(f));
    rk_win::StaticFileRoot root;
    ASSERT_TRUE(root.open(f.webroot));

    std::string out;
    EXPECT_FALSE(rk_win::readFileWithinRoot(root, f.secret, out));
    EXPECT_EQ(out.find("TOCTOU_SECRET_MARKER"), std::string::npos);
}

// 根目录在路径校验后被整体换成外部目录时，读取仍必须绑定到原 webroot，不能重开新根。
// 根目录在校验后重绑定为新目录时，读取必须仍绑定到原根对象；此用例不依赖 symlink 权限。
TEST(StaticFileToctou, ReadFileWithinRootRejectsRootSwapToOutside) {
    ToctouFixture f;
    ASSERT_TRUE(makeToctouFixture(f));
    rk_win::StaticFileRoot root;
    ASSERT_TRUE(root.open(f.webroot));

    std::filesystem::path resolved;
    ASSERT_TRUE(root.resolve("/secret.txt", resolved));

    std::error_code ec;
    const std::filesystem::path originalRoot = f.base / L"webroot-original";
    std::filesystem::rename(f.webroot, originalRoot, ec);
    if (ec) {
#ifdef _WIN32
        // Win32 ERROR_SHARING_VIOLATION=32；其他错误不能被当作固定根保护成功。
        EXPECT_EQ(ec, std::error_code(32, std::system_category()))
            << "webroot 重命名失败原因异常: " << ec.message();
#else
        ADD_FAILURE() << "非 Windows 根目录重命名失败: " << ec.message();
#endif
        return;
    }

    std::filesystem::create_directories(f.webroot, ec);
    ASSERT_FALSE(ec) << "无法创建替换后的 webroot: " << ec.message();
    {
        std::ofstream ofs(f.webroot / L"secret.txt");
        ofs << "TOCTOU_SECRET_MARKER";
    }

    std::string out;
    EXPECT_FALSE(rk_win::readFileWithinRoot(root, resolved, out));
    EXPECT_EQ(out.find("TOCTOU_SECRET_MARKER"), std::string::npos);
}

// RED→GREEN 主判别：逐层句柄打开必须拒绝「webroot 内 symlink → webroot 外」。
// 修复前（naive 直接 open）：返回 true 且读到越界内容（本用例失败 = RED）；
// 修复后（FILE_OPEN_REPARSE_POINT 打开链接本身并拒绝）：返回 false、不输出越界内容（GREEN）。
TEST(StaticFileToctou, ReadFileWithinRootRejectsOutsideSymlink) {
    ToctouFixture f;
    ASSERT_TRUE(makeToctouFixture(f));
    rk_win::StaticFileRoot root;
    ASSERT_TRUE(root.open(f.webroot));
    const std::filesystem::path link = f.webroot / L"sub" / L"link.txt";
    DWORD symlinkError = ERROR_SUCCESS;
    if (!tryCreateFileSymlink(link, f.secret, symlinkError)) {
        GTEST_SKIP() << "无法创建 symlink（GetLastError=" << symlinkError << "，需开发者模式或管理员权限）";
    }
    std::string out;
    const bool ok = rk_win::readFileWithinRoot(root, std::filesystem::path(L"sub") / L"link.txt", out);
    EXPECT_FALSE(ok) << "指向 webroot 外的 symlink 必须在逐层句柄打开时被拒绝";
    EXPECT_EQ(out.find("TOCTOU_SECRET_MARKER"), std::string::npos) << "越界内容不得被读出";
}

// 中间目录也不能是 junction/symlink；否则仅保护叶文件仍可绕到 webroot 外。
TEST(StaticFileToctou, ReadFileWithinRootRejectsOutsideDirectorySymlink) {
    ToctouFixture f;
    ASSERT_TRUE(makeToctouFixture(f));
    rk_win::StaticFileRoot root;
    ASSERT_TRUE(root.open(f.webroot));
    const std::filesystem::path link = f.webroot / L"sub" / L"linked-outside";
    DWORD symlinkError = ERROR_SUCCESS;
    if (!tryCreateDirectorySymlink(link, f.outside, symlinkError)) {
        GTEST_SKIP() << "无法创建目录 symlink（GetLastError=" << symlinkError << "，需开发者模式或管理员权限）";
    }

    std::string out;
    const bool ok = rk_win::readFileWithinRoot(
        root, std::filesystem::path(L"sub") / L"linked-outside" / L"secret.txt", out);
    EXPECT_FALSE(ok) << "中间目录重解析点必须被拒绝";
    EXPECT_EQ(out.find("TOCTOU_SECRET_MARKER"), std::string::npos) << "越界内容不得被读出";
}

// 正常静态文件不受影响：webroot 内普通文件照常读取（行为回归）。
TEST(StaticFileToctou, ReadFileWithinRootAllowsInsideFiles) {
    ToctouFixture f;
    ASSERT_TRUE(makeToctouFixture(f));
    rk_win::StaticFileRoot root;
    ASSERT_TRUE(root.open(f.webroot));
    const std::filesystem::path inside = f.webroot / L"sub" / L"app.js";
    {
        std::ofstream ofs(inside);
        ofs << "window.hello=1;";
    }
    std::string out;
    ASSERT_TRUE(rk_win::readFileWithinRoot(root, std::filesystem::path(L"sub") / L"app.js", out));
    EXPECT_NE(out.find("window.hello"), std::string::npos) << "webroot 内正常文件读取失败";
}

// 为消除最终路径字符串校验的祖先重绑定竞态，静态资源路径中的所有重解析点均拒绝。
TEST(StaticFileToctou, ReadFileWithinRootRejectsInsideSymlink) {
    ToctouFixture f;
    ASSERT_TRUE(makeToctouFixture(f));
    rk_win::StaticFileRoot root;
    ASSERT_TRUE(root.open(f.webroot));
    const std::filesystem::path real = f.webroot / L"real.js";
    {
        std::ofstream ofs(real);
        ofs << "window.inside=1;";
    }
    const std::filesystem::path link = f.webroot / L"alias.js";
    DWORD symlinkError = ERROR_SUCCESS;
    if (!tryCreateFileSymlink(link, real, symlinkError)) {
        GTEST_SKIP() << "无法创建 symlink（GetLastError=" << symlinkError << "，需开发者模式或管理员权限）";
    }
    std::string out;
    EXPECT_FALSE(rk_win::readFileWithinRoot(root, std::filesystem::path(L"alias.js"), out));
    EXPECT_EQ(out.find("window.inside"), std::string::npos) << "重解析点不得被读取";
}

// 不存在文件 → false（与旧 readFileBinary 行为一致）。
TEST(StaticFileToctou, ReadFileWithinRootMissingFileReturnsFalse) {
    ToctouFixture f;
    ASSERT_TRUE(makeToctouFixture(f));
    rk_win::StaticFileRoot root;
    ASSERT_TRUE(root.open(f.webroot));
    std::string out;
    EXPECT_FALSE(rk_win::readFileWithinRoot(root, std::filesystem::path(L"no-such.txt"), out));
}
#else
TEST(ClientThreadPool, ReclaimsFinishedThreads) {
    GTEST_SKIP() << "std::thread 用例仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(ClientThreadPool, RunningThreadNotReclaimedUntilFinished) {
    GTEST_SKIP() << "std::thread 用例仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(ClientThreadPool, BoundedUnderChurn) {
    GTEST_SKIP() << "std::thread 用例仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesServer, ClientThreadsBoundedAfterConnectionFlood) {
    GTEST_SKIP() << "需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, AnonymousWriteReachableWithoutToken) {
    GTEST_SKIP() << "HTTP 认证用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, UnauthenticatedWriteRejectedWhenAuthEnabled) {
    GTEST_SKIP() << "HTTP 认证用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, UnauthenticatedActionRejectedWhenAuthEnabled) {
    GTEST_SKIP() << "HTTP 认证用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, WrongBearerTokenRejected) {
    GTEST_SKIP() << "HTTP 认证用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, ValidBearerTokenAccepted) {
    GTEST_SKIP() << "HTTP 认证用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, AnonymousWhitelistRemainsPublic) {
    GTEST_SKIP() << "HTTP 认证用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, UnauthenticatedStreamRejectedWhenAuthEnabled) {
    GTEST_SKIP() << "HTTP 认证用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, CrossSiteOriginWriteRejected) {
    GTEST_SKIP() << "HTTP CSRF 用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, CrossSiteFormPostRejected) {
    GTEST_SKIP() << "HTTP CSRF 用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, CrossSiteSecFetchSiteRejected) {
    GTEST_SKIP() << "HTTP CSRF 用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, SameOriginLoopbackWriteAccepted) {
    GTEST_SKIP() << "HTTP CSRF 用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, SameOriginLocalhostWriteAccepted) {
    GTEST_SKIP() << "HTTP CSRF 用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, SameOriginSecFetchSiteAccepted) {
    GTEST_SKIP() << "HTTP CSRF 用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, HeadlessClientWriteAcceptedWithoutOrigin) {
    GTEST_SKIP() << "HTTP CSRF 用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, BasicSchemeRejected) {
    GTEST_SKIP() << "HTTP 认证用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, BearerWithoutSpaceRejected) {
    GTEST_SKIP() << "HTTP 认证用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, UppercaseSchemeDoesNotBypassTokenCheck) {
    GTEST_SKIP() << "HTTP 认证用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, TabSeparatedSchemeRejected) {
    GTEST_SKIP() << "HTTP 认证用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, DuplicateAuthorizationMalformedFirstRejected) {
    GTEST_SKIP() << "HTTP 认证用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesAuth, CrossSiteWriteRejectedWithoutAuthToken) {
    GTEST_SKIP() << "HTTP CSRF 用例需要真实 Winsock 服务端，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesStreamQuota, MjpegFpsThrottle) {
    GTEST_SKIP() << "流配额用例需要真实 Winsock socket 对，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesStreamQuota, MjpegByteQuotaTerminatesSession) {
    GTEST_SKIP() << "流配额用例需要真实 Winsock socket 对，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(HttpFacesStreamQuota, SseDurationTerminatesSession) {
    GTEST_SKIP() << "流配额用例需要真实 Winsock socket 对，仅在 win_unit_tests (Windows + OpenCV) 下运行";
}
TEST(StaticFileToctou, ValidatedPathSwappedOutsideIsReadableByRawOpen) {
    GTEST_SKIP() << "TOCTOU 用例需要 Windows symlink API，仅在 win_unit_tests 下运行";
}
TEST(StaticFileToctou, ReadFileWithinRootRejectsOutsideSymlink) {
    GTEST_SKIP() << "TOCTOU 用例需要 Windows symlink API，仅在 win_unit_tests 下运行";
}
TEST(StaticFileToctou, ReadFileWithinRootRejectsDirectOutsidePath) {
    const std::filesystem::path base = std::filesystem::temp_directory_path() / L"rk_toctou_direct_outside";
    const std::filesystem::path webroot = base / L"webroot";
    const std::filesystem::path secret = base / L"outside" / L"secret.txt";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(webroot, ec);
    std::filesystem::create_directories(secret.parent_path(), ec);
    ASSERT_FALSE(ec) << "无法创建 TOCTOU 临时目录: " << ec.message();
    {
        std::ofstream ofs(secret);
        ofs << "TOCTOU_SECRET_MARKER";
    }

    rk_win::StaticFileRoot root;
    ASSERT_TRUE(root.open(webroot));
    std::string out;
    EXPECT_FALSE(rk_win::readFileWithinRoot(root, secret, out));
    EXPECT_EQ(out.find("TOCTOU_SECRET_MARKER"), std::string::npos);

    root.close();
    std::filesystem::remove_all(base, ec);
}
TEST(StaticFileToctou, ReadFileWithinRootRejectsRootSwapToOutside) {
    const std::filesystem::path base = std::filesystem::temp_directory_path() / L"rk_toctou_root_swap";
    const std::filesystem::path webroot = base / L"webroot";
    const std::filesystem::path originalRoot = base / L"webroot-original";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(webroot, ec);
    ASSERT_FALSE(ec) << "无法创建 TOCTOU 临时目录: " << ec.message();

    rk_win::StaticFileRoot root;
    ASSERT_TRUE(root.open(webroot));
    std::filesystem::path resolved;
    ASSERT_TRUE(root.resolve("/secret.txt", resolved));

    std::filesystem::rename(webroot, originalRoot, ec);
    if (ec) {
#ifdef _WIN32
        // Win32 ERROR_SHARING_VIOLATION=32；其他错误不能被当作固定根保护成功。
        EXPECT_EQ(ec, std::error_code(32, std::system_category()))
            << "webroot 重命名失败原因异常: " << ec.message();
#else
        ADD_FAILURE() << "非 Windows 根目录重命名失败: " << ec.message();
#endif
        root.close();
        std::filesystem::remove_all(base, ec);
        return;
    }
    std::filesystem::create_directories(webroot, ec);
    ASSERT_FALSE(ec) << "无法创建替换后的 webroot: " << ec.message();
    {
        std::ofstream ofs(webroot / L"secret.txt");
        ofs << "TOCTOU_SECRET_MARKER";
    }

    std::string out;
    EXPECT_FALSE(rk_win::readFileWithinRoot(root, resolved, out));
    EXPECT_EQ(out.find("TOCTOU_SECRET_MARKER"), std::string::npos);

    root.close();
    std::filesystem::remove_all(base, ec);
}
TEST(StaticFileToctou, ReadFileWithinRootAllowsInsideFiles) {
#ifdef _WIN32
    const std::filesystem::path base = std::filesystem::temp_directory_path() / L"rk_toctou_inside_file";
    const std::filesystem::path webroot = base / L"webroot";
    const std::filesystem::path inside = webroot / L"sub" / L"app.js";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(inside.parent_path(), ec);
    ASSERT_FALSE(ec) << "无法创建 TOCTOU 临时目录: " << ec.message();
    {
        std::ofstream ofs(inside);
        ofs << "window.hello=1;";
    }

    rk_win::StaticFileRoot root;
    ASSERT_TRUE(root.open(webroot));
    std::string out;
    ASSERT_TRUE(rk_win::readFileWithinRoot(root, std::filesystem::path(L"sub") / L"app.js", out));
    EXPECT_NE(out.find("window.hello"), std::string::npos);

    root.close();
    std::filesystem::remove_all(base, ec);
#else
    GTEST_SKIP() << "非 Windows 读取路径按设计 fail-closed";
#endif
}
TEST(StaticFileToctou, ReadFileWithinRootRejectsInsideSymlink) {
    GTEST_SKIP() << "TOCTOU 用例需要 Windows symlink API，仅在 win_unit_tests 下运行";
}
TEST(StaticFileToctou, ReadFileWithinRootMissingFileReturnsFalse) {
    GTEST_SKIP() << "TOCTOU 用例需要 Windows symlink API，仅在 win_unit_tests 下运行";
}
#endif
