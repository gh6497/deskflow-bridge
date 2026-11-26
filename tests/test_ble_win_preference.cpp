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
    std::puts("PASS: per-phone preference retention, disconnect/reconnect, cleanup and nonfatal request failure");
    return 0;
}
