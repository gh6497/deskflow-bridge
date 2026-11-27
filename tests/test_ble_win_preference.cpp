/* Connection preference ownership with fake requests; no Bluetooth or sleeps.
 * SPDX-License-Identifier: Apache-2.0 */
#include "ble_win_preference.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #test); std::exit(1); } } while (0)
using namespace otg_ble_win_detail;

struct Radio {
    int requests = 0, closes = 0, live = 0;
    bool unsupported = false, close_fails = false;
};
struct Request {
    Radio* radio;
    void Close()
    {
        ++radio->closes;
        --radio->live;
        if (radio->close_fails) throw std::runtime_error("fake close failure");
    }
};
static Request create(Radio& radio)
{
    ++radio.requests;
    if (radio.unsupported) throw std::runtime_error("fake unsupported/denied API");
    ++radio.live;
    return {&radio};
}

static void retry_slow_link()
{
    using namespace std::chrono_literals;
    using Preference = ConnectionPreference<Request>;
    Radio radio;
    Preference preference;
    auto now = Preference::Clock::time_point{};
    auto acquire = [&] { return preference.acquire([&] { return create(radio); }, now); };
    preference.observe(0, true);
    CHECK(acquire());
    for (unsigned retry = 1; retry <= Preference::max_retries; ++retry) {
        CHECK(preference.check_interval(36, now) == PreferenceRetry::none); // Windows accepted, still 45ms.
        now += 2999ms;
        CHECK(preference.check_interval(36, now) == PreferenceRetry::none);
        CHECK(!acquire() && radio.live == 1);
        now += 1ms;
        CHECK(preference.check_interval(36, now) == PreferenceRetry::scheduled);
        CHECK(preference.retry_count() == retry);
        CHECK(radio.live == 0 && radio.closes == static_cast<int>(retry));
        CHECK(!acquire());
        now += 999ms;
        CHECK(!acquire());
        now += 1ms;
        CHECK(acquire());
        CHECK(radio.live == 1 && radio.requests == static_cast<int>(retry + 1));
    }
    CHECK(preference.check_interval(36, now) == PreferenceRetry::none);
    now += 3s;
    CHECK(preference.check_interval(36, now) == PreferenceRetry::exhausted);
    now += 60s;
    CHECK(preference.check_interval(36, now) == PreferenceRetry::none); // Log once; no unbounded retries.
    CHECK(!acquire() && radio.live == 1 && radio.requests == 4);
    CHECK(preference.check_interval(12, now) == PreferenceRetry::none);
    now += 1s;
    CHECK(preference.check_interval(36, now) == PreferenceRetry::none);
    now += 60s;
    CHECK(preference.check_interval(36, now) == PreferenceRetry::none); // Recovery does not refill link budget.
    CHECK(!acquire());
    preference.observe(1, true); // Physical reconnect, even when both events happen between polls.
    CHECK(radio.live == 0 && preference.retry_count() == 0);
    CHECK(acquire() && radio.requests == 5);
    CHECK(preference.check_interval(36, now) == PreferenceRetry::none);
    now += 3s;
    CHECK(preference.check_interval(36, now) == PreferenceRetry::scheduled);
}

static void retry_recovery_and_isolation()
{
    using namespace std::chrono_literals;
    using Preference = ConnectionPreference<Request>;
    auto now = Preference::Clock::time_point{};
    Radio r17, k20;
    Preference first, second;
    first.observe(0, true);
    second.observe(0, true);
    CHECK(first.acquire([&] { return create(r17); }, now));
    CHECK(second.acquire([&] { return create(k20); }, now));
    CHECK(first.check_interval(36, now) == PreferenceRetry::none);
    now += 2s;
    CHECK(first.check_interval(12, now) == PreferenceRetry::none); // Negotiation completes within grace.
    now += 60s;
    CHECK(first.check_interval(6, now) == PreferenceRetry::none); // 7.5ms is already fast enough.
    CHECK(r17.requests == 1 && r17.closes == 0);
    CHECK(first.check_interval(36, now) == PreferenceRetry::none); // Later phone-side rollback to 45ms.
    now += 2s;
    CHECK(first.check_interval(0, now) == PreferenceRetry::none); // Unknown is not evidence for retry.
    now += 60s;
    CHECK(first.check_interval(36, now) == PreferenceRetry::none);
    now += 3s;
    CHECK(first.check_interval(36, now) == PreferenceRetry::scheduled);
    CHECK(second.check_interval(12, now) == PreferenceRetry::none);
    CHECK(k20.live == 1 && k20.closes == 0);
    now += 1s;
    CHECK(first.acquire([&] { return create(r17); }, now));
    CHECK(first.check_interval(12, now) == PreferenceRetry::none);
    now += 60s;
    CHECK(first.check_interval(12, now) == PreferenceRetry::none);
    CHECK(!first.acquire([&] { return create(r17); }, now));
    CHECK(r17.requests == 2 && r17.live == 1 && r17.closes == 1);
    first.observe(0, true); // Deskflow pause/metadata refresh does not reset the retry budget.
    CHECK(first.retry_count() == 1);
    CHECK(first.check_interval(36, now) == PreferenceRetry::none);
    now += 3s;
    CHECK(first.check_interval(36, now) == PreferenceRetry::scheduled);
    first.observe(1, false); // Disconnect cancels a queued retry, including its old deadline.
    now += 1s;
    CHECK(!first.acquire([&] { return create(r17); }, now));
    first.observe(1, true);
    CHECK(first.acquire([&] { return create(r17); }, now));
    CHECK(first.retry_count() == 0 && k20.live == 1);
    first.release(); // Rejected/unsupported requests cannot retry from an unrelated slow reading.
    CHECK(first.check_interval(36, now) == PreferenceRetry::none);
    now += 60s;
    CHECK(first.check_interval(36, now) == PreferenceRetry::none);
    CHECK(!first.acquire([&] { return create(r17); }, now));
}

int main()
{
    Radio r17, k20;
    {
        ConnectionPreference<Request> first, second;
        CHECK(!first.acquire([&] { return create(r17); })); // Unconnected discovery has no preference.
        CHECK(r17.requests == 0);
        first.observe(0, true);
        second.observe(0, true);
        CHECK(first.acquire([&] { return create(r17); }));
        CHECK(second.acquire([&] { return create(k20); }));
        for (int i = 0; i < 100; ++i) {
            // Input pause, metadata checks and repeated refreshes retain the
            // preference as long as the underlying Bluetooth link survives.
            first.observe(0, true);
            CHECK(!first.acquire([&] { return create(r17); }));
        }
        CHECK(r17.requests == 1 && r17.live == 1 && r17.closes == 0);
        first.observe(1, false);
        first.observe(1, false);
        CHECK(r17.live == 0 && r17.closes == 1);
        CHECK(!first.acquire([&] { return create(r17); }));
        CHECK(k20.live == 1 && k20.closes == 0); // Another phone is unaffected.
        first.observe(1, true);
        CHECK(first.acquire([&] { return create(r17); }));
        CHECK(r17.requests == 2 && r17.live == 1);
        first.observe(2, true); // Disconnect/reconnect both happened between refreshes.
        CHECK(r17.live == 0 && r17.closes == 2);
        CHECK(first.acquire([&] { return create(r17); }));
        first.reset(); // Replacing the BluetoothLEDevice releases the old request first.
        CHECK(r17.live == 0 && r17.closes == 3);
        first.observe(2, true);
        CHECK(first.acquire([&] { return create(r17); }));
    }
    CHECK(r17.live == 0 && r17.closes == 4 && k20.live == 0 && k20.closes == 1);

    Radio denied;
    denied.unsupported = true;
    {
        ConnectionPreference<Request> preference;
        preference.observe(0, true);
        try {
            preference.acquire([&] { return create(denied); });
            CHECK(false);
        } catch (const std::runtime_error&) {}
        preference.release();
        for (int i = 0; i < 100; ++i) {
            preference.observe(0, true);
            CHECK(!preference.acquire([&] { return create(denied); }));
        }
        CHECK(denied.requests == 1 && denied.live == 0);
        denied.unsupported = false;
        preference.observe(1, true);
        CHECK(preference.acquire([&] { return create(denied); }));
        preference.release(); // Request returned a rejected status: close, but do not spin retries.
        CHECK(denied.live == 0 && denied.closes == 1);
        CHECK(!preference.acquire([&] { return create(denied); }));
        preference.observe(2, true);
        CHECK(preference.acquire([&] { return create(denied); }));
        denied.close_fails = true; // Cleanup errors must not terminate a bridge worker.
    }
    CHECK(denied.live == 0 && denied.closes == 2);
    retry_slow_link();
    retry_recovery_and_isolation();
    std::puts("PASS: preference retention, measured interval retry/backoff/cap, rollback, isolation and reconnect");
    return 0;
}
