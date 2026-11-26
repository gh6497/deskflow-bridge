/* Lifetime of a per-link Windows connection preference, independent of input readiness.
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstdint>
#include <optional>

namespace otg_ble_win_detail {
template<class Request> class ConnectionPreference {
    std::optional<Request> request;
    uint64_t generation = 0;
    bool connected = false, attempted = false;
public:
    ConnectionPreference() = default;
    ConnectionPreference(const ConnectionPreference&) = delete;
    ConnectionPreference& operator=(const ConnectionPreference&) = delete;
    ~ConnectionPreference() { release(); }

    void release() noexcept
    {
        if (request) {
            try { request->Close(); } catch (...) {}
            request.reset();
        }
    }
    void reset() noexcept
    {
        release();
        attempted = false;
        connected = false;
    }
    void observe(uint64_t link_generation, bool online)
    {
        // A fast disconnect/reconnect can happen between worker refreshes.
        if (generation != link_generation || !online) reset();
        generation = link_generation;
        connected = online;
    }
    template<class Create> Request* acquire(Create create)
    {
        if (!connected || attempted) return nullptr;
        // Unsupported/denied requests must not retry every 100ms. A new link
        // (or replacement device object) permits one new attempt.
        attempted = true;
        request.emplace(create());
        return &*request;
    }
};
} // namespace otg_ble_win_detail
