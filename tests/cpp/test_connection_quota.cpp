#include "rk_win/ConnectionQuota.h"
#include <gtest/gtest.h>

#include <vector>

TEST(ConnectionQuota, AcquireRelease) {
    rk_win::ConnectionQuota q(4);

    std::vector<rk_win::ConnectionQuota::Lease> leases;
    for (int i = 0; i < 4; i++) {
        auto l = q.tryAcquire();
        ASSERT_TRUE(l.has_value()) << "tryAcquire should succeed at i=" << i;
        leases.push_back(std::move(*l));
    }

    EXPECT_FALSE(q.tryAcquire().has_value()) << "tryAcquire should fail when full";

    leases.pop_back();

    EXPECT_TRUE(q.tryAcquire().has_value()) << "tryAcquire should succeed after release";
}
