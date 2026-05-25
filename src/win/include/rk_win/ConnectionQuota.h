#pragma once

#include <atomic>
#include <optional>

namespace rk_win {

class ConnectionQuota {
public:
    class Lease {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept : q_(other.q_) {
            other.q_ = nullptr;
        }

        Lease& operator=(Lease&& other) noexcept {
            if (this == &other) return *this;
            release();
            q_ = other.q_;
            other.q_ = nullptr;
            return *this;
        }

        ~Lease() {
            release();
        }

    private:
        friend class ConnectionQuota;
        explicit Lease(ConnectionQuota* q) : q_(q) {}

        void release() {
            if (!q_) return;
            q_->release();
            q_ = nullptr;
        }

        ConnectionQuota* q_ = nullptr;
    };

    explicit ConnectionQuota(int max) : max_(max) {}

    std::optional<Lease> tryAcquire() {
        int cur = active_.load();
        while (cur < max_) {
            if (active_.compare_exchange_weak(cur, cur + 1)) {
                return Lease(this);
            }
        }
        return std::nullopt;
    }

    int active() const {
        return active_.load();
    }

    int max() const {
        return max_;
    }

private:
    void release() {
        active_--;
    }

    const int max_;
    std::atomic<int> active_{0};
};

}  // namespace rk_win

