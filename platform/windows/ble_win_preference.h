/* Lifetime of a per-link Windows connection preference, independent of input readiness.
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <chrono>
#include <cstdint>
#include <optional>

namespace otg_ble_win_detail {
enum class PreferenceRetry { none, scheduled, exhausted };

template<class Request> class ConnectionPreference {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr unsigned max_retries = 3;
    static constexpr auto settle_time = std::chrono::seconds(3);
    static constexpr auto retry_delay = std::chrono::seconds(1);
private:
    std::optional<Request> request;
    uint64_t generation = 0;
    bool connected = false, attempted = false;
    unsigned retries = 0;
    bool exhaustion_reported = false;
    std::optional<Clock::time_point> slow_since, retry_at;
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
        retries = 0;
        exhaustion_reported = false;
        slow_since.reset();
        retry_at.reset();
    }
    void observe(uint64_t link_generation, bool online)
    {
        // A fast disconnect/reconnect can happen between worker refreshes.
        if (generation != link_generation || !online) reset();
        generation = link_generation;
        connected = online;
    }
    bool has_request() const { return request.has_value(); }
    unsigned retry_count() const { return retries; }

    PreferenceRetry check_interval(uint16_t units, Clock::time_point now)
    {
        // Only real negotiated values authorize a retry. Unknown readings and
        // 15ms or faster links must never churn an otherwise useful request.
        if (!connected || !request || !units || units <= 12) {
            slow_since.reset();
            return PreferenceRetry::none;
        }
        if (!slow_since) slow_since = now;
        if (now - *slow_since < settle_time) return PreferenceRetry::none;
        if (retries == max_retries) {
            if (exhaustion_reported) return PreferenceRetry::none;
            exhaustion_reported = true;
            return PreferenceRetry::exhausted; // Retain the last preference and all input sessions.
        }
        // Drop just this preference and leave time for Windows to retire it
        // before submitting a fresh one. GATT, pairing and input stay intact.
        release();
        slow_since.reset();
        retry_at = now + retry_delay;
        ++retries;
        return PreferenceRetry::scheduled;
    }

    template<class Create> Request* acquire(Create create, Clock::time_point now = Clock::now())
    {
        if (!connected || (attempted && (!retry_at || now < *retry_at))) return nullptr;
        // Unsupported/denied requests do not retry. Only a held request with
        // measured slow negotiation, or a new physical link, permits another.
        attempted = true;
        retry_at.reset();
        slow_since.reset();
        request.emplace(create());
        return &*request;
    }
};
} // namespace otg_ble_win_detail
