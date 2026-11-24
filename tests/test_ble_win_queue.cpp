/* Offline regression: no Bluetooth API or adapter use. SPDX-License-Identifier: Apache-2.0 */
#include "ble_win_queue.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); std::exit(1); } } while (0)
using namespace otg_ble_win_detail;

static Report mouse(uint8_t flags, uint8_t buttons, uint8_t x, uint8_t wheel = 0)
{
    return {2, {flags, buttons, x, 0, 20, 0, wheel, 0}, false, 1};
}

int main()
{
    using namespace std::chrono_literals;
    Queue queue;
    CHECK(queue.push(mouse(2, 0, 0))); // first hover is a barrier
    CHECK(queue.push(mouse(2, 0, 1)));
    CHECK(queue.push(mouse(2, 0, 2)));
    CHECK(queue.size() == 2);
    auto enter = queue.pop();
    auto move = queue.pop();
    CHECK(!enter.movement && move.movement && move.bytes[2] == 2);
    const auto start = std::chrono::steady_clock::time_point{};
    CHECK(next_notification_time(enter, start, start + 10ms, fallback_motion_interval,
                                 fallback_state_interval) == start + 40ms);
    for (auto units : {6, 10}) { // Windows reports 7.5 or 12.5 ms in 1.25 ms units.
        auto interval = connection_interval(units);
        auto now = start;
        for (int i = 0; i < 20; ++i) {
            auto completed = now + 5ms;
            now = next_notification_time(move, now, completed, interval, interval);
            CHECK(now >= completed);
        }
        CHECK(now - start == (units == 6 ? 150ms : 250ms));
        // A slow notification already consumed its entire interval.
        CHECK(next_notification_time(move, now, now + 20ms, interval, interval) <= now + 20ms);
    }

    CHECK(queue.push(mouse(3, 1, 3))); // preserve down position
    CHECK(queue.push(mouse(3, 1, 4)));
    CHECK(queue.push(mouse(3, 1, 5)));
    CHECK(queue.push({1, {0, 0, 4, 0, 0, 0, 0, 0}, false, 1}));
    CHECK(queue.push(mouse(3, 1, 6))); // cannot merge across keyboard
    CHECK(queue.push(mouse(2, 0, 7))); // release is a barrier
    CHECK(queue.push(mouse(2, 0, 8, 1)));
    CHECK(queue.push(mouse(2, 0, 9))); // wheel reset is a barrier too
    CHECK(queue.push(mouse(0, 0, 10))); // end range
    CHECK(queue.size() == 8);
    CHECK(queue.pop().bytes[2] == 3);
    CHECK(queue.pop().bytes[2] == 5);
    CHECK(queue.pop().id == 1);
    CHECK(queue.pop().bytes[2] == 6);
    CHECK(queue.pop().bytes[1] == 0);
    CHECK(queue.pop().bytes[6] == 1);
    CHECK(!queue.pop().movement);
    CHECK(queue.pop().bytes[0] == 0);

    queue.clear();
    for (size_t i = 0; i < Queue::capacity; ++i) CHECK(queue.push({1, {}, false, 1}));
    CHECK(!queue.push(mouse(2, 0, 1)));
    CHECK(queue.size() == 64);
    queue.clear();
    CHECK(queue.empty());
    CHECK(queue.push(mouse(2, 0, 0)));
    CHECK(!queue.front().movement);
    CHECK(queue.push(mouse(2, 0, 1)));
    auto newer = mouse(2, 0, 2);
    newer.epoch = 2;
    CHECK(queue.push(newer)); // never merge across readiness generations
    CHECK(queue.size() == 3);
    CHECK(!queue.push({3, {}, false, 1}));

    // Disconnect retains the provider, but must not replay queued presses.
    queue_releases(queue, {3, 1, 123, 1, 234, 2, 1, 1}, 7, true);
    CHECK(queue.size() == 3);
    auto key_release = queue.pop();
    CHECK(key_release.id == 1 && key_release.epoch == 7);
    CHECK((key_release.bytes == std::array<uint8_t, 8>{}));
    auto button_release = queue.pop();
    CHECK(button_release.id == 2 && button_release.bytes[0] == 2 && button_release.bytes[1] == 0);
    CHECK(button_release.bytes[2] == 123 && button_release.bytes[4] == 234);
    CHECK(button_release.bytes[6] == 0 && button_release.bytes[7] == 0);
    auto range_release = queue.pop();
    CHECK(range_release.bytes[0] == 0 && queue.empty());
    CHECK(queue.push(mouse(3, 1, 5)));
    queue_releases(queue, {}, 8, false); // Link loss discards, later readiness releases first.
    CHECK(queue.empty());

    const uint8_t desc[] = {0x05, 1, 0x09, 6, 0xa1, 1, 0xc0};
    std::vector<uint8_t> map;
    CHECK(append_descriptor(map, desc, sizeof(desc), 1));
    CHECK(append_descriptor(map, desc, sizeof(desc), 2));
    CHECK(map.size() == 18 && map[6] == 0x85 && map[7] == 1 && map[16] == 2);
    CHECK(!append_descriptor(map, nullptr, 0, 1));
    CHECK(!append_descriptor(map, desc, 3, 1));
    std::puts("PASS: bounded queue, move coalescing, input barriers, epochs and HOGP Report IDs");
    return 0;
}
