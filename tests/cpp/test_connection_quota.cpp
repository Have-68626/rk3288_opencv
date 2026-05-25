#include "rk_win/ConnectionQuota.h"

#include <iostream>
#include <vector>

bool test_connection_quota_acquire_release() {
    rk_win::ConnectionQuota q(4);

    std::vector<rk_win::ConnectionQuota::Lease> leases;
    for (int i = 0; i < 4; i++) {
        auto l = q.tryAcquire();
        if (!l.has_value()) {
            std::cout << "FAIL: tryAcquire should succeed at i=" << i << std::endl;
            return false;
        }
        leases.push_back(std::move(*l));
    }

    if (q.tryAcquire().has_value()) {
        std::cout << "FAIL: tryAcquire should fail when full" << std::endl;
        return false;
    }

    leases.pop_back();

    if (!q.tryAcquire().has_value()) {
        std::cout << "FAIL: tryAcquire should succeed after release" << std::endl;
        return false;
    }

    return true;
}

