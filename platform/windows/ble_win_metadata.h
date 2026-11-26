/* Non-blocking identity/pairing checks. Metadata delays are not notify failures.
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <chrono>
#include <cstdint>

namespace otg_ble_win_detail {
enum class MetadataResult { pending, timed_out, complete, discarded };
class MetadataRequest {
    using Clock = std::chrono::steady_clock;
    Clock::time_point deadline{};
    uint64_t generation = 0;
    bool running = false, cancelling = false;
public:
    bool active() const { return running; }
    template<class Cancel> void abandon(Cancel cancel)
    {
        if (!running || cancelling) return;
        cancelling = true;
        try { cancel(); } catch (...) {}
    }
    void start(Clock::time_point now, uint64_t epoch,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(1500))
    {
        deadline = now + timeout;
        generation = epoch;
        running = true;
        cancelling = false;
    }
    template<class Cancel>
    MetadataResult step(Clock::time_point now, uint64_t epoch, bool pending, Cancel cancel)
    {
        if (!pending) {
            running = false;
            return cancelling || epoch != generation ? MetadataResult::discarded : MetadataResult::complete;
        }
        if (!cancelling && (epoch != generation || now >= deadline)) {
            cancelling = true;
            try { cancel(); } catch (...) {}
            if (epoch == generation) return MetadataResult::timed_out;
        }
        // Cancel is only a request: retain the operation and never stack another
        // lookup behind it until Windows actually completes/cancels the old one.
        return MetadataResult::pending;
    }
};

class MetadataLease {
    using Clock = std::chrono::steady_clock;
    Clock::time_point expiry{};
    uint64_t generation = 0;
    bool verified = false;
public:
    void renew(Clock::time_point now, uint64_t epoch)
    { expiry = now + std::chrono::seconds(3); generation = epoch; verified = true; }
    void clear() { verified = false; }
    bool valid(Clock::time_point now, uint64_t epoch) const
    { return verified && generation == epoch && now < expiry; }
};
} // namespace otg_ble_win_detail
