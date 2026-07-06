#pragma once

#include <array>
#include <chrono>
#include <type_traits>

class JniCallbackThrottle {
public:
    enum class Event : std::uint8_t {
        NoFace,
        AuthFail,
        Faces,
        Verified,
    };

    JniCallbackThrottle() {
        const auto epoch = std::chrono::steady_clock::time_point::min();
        lastCall_.fill(epoch);
    }

    bool tryAcquire(Event e) noexcept {
        const auto now = std::chrono::steady_clock::now();
        const auto& last = lastCall_[static_cast<std::size_t>(e)];
        if (now - last < kIntervals[static_cast<std::size_t>(e)]) {
            return false;
        }
        lastCall_[static_cast<std::size_t>(e)] = now;
        return true;
    }

private:
    static constexpr std::chrono::milliseconds kIntervals[] = {
        std::chrono::milliseconds(2000),  // NoFace
        std::chrono::milliseconds(1000),  // AuthFail
        std::chrono::milliseconds(650),   // Faces
        std::chrono::milliseconds(800),   // Verified
    };
    static_assert(std::extent_v<decltype(kIntervals)> == 4,
                  "kIntervals must cover all Event values");

    std::array<std::chrono::steady_clock::time_point, 4> lastCall_;
};
