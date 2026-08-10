#include "rk_win/HttpFacesServerPath.h"
#include "rk_win/HttpFacesServer.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
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
// 背景：HttpFacesServer 每个连接一个 std::thread，修复前仅 stop() 才 join，
// 已完成连接的线程句柄随请求总量无限累积（线性增长）。
// 修复后 acceptLoop 每次 accept 后调用 ClientThreadPool::reclaimFinished()，
// 及时 join 已完成线程。
// 仅 win_unit_tests（Windows + OpenCV）运行：core_unit_tests 保持无 std::thread
// 依赖（Linux 下不显式链接 pthread），故非该目标时全部 GTEST_SKIP。
#if defined(_WIN32) && !defined(RK_SKIP_OPENCV)
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
#endif
