/* Fake radio for the real Windows broker/pipe/queue regression executable. */
class Service {
    std::shared_ptr<State> s;
    otg_ble_win_detail::MetadataRequest metadata;
    bool metadata_active = false;
    int index() const { return static_cast<int>(s->peer_address - test_address); }
public:
    explicit Service(std::shared_ptr<State> state, const Service * = nullptr) : s(std::move(state)) {}
    void start() {}
    void ensure_advertising() {}
    void refresh()
    {
        uint64_t target;
        { std::lock_guard<std::mutex> lock(s->mutex); target = s->peer_address; }
        if (!target) {
            for (int i = 0; i < OTG_BLE_MAX_PHONES; ++i) {
                if (InterlockedCompareExchange(&test_shared->online[i], 0, 0) && s->router->claim(test_address + i, s)) {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    s->peer_address = test_address + i;
                    break;
                }
            }
        }
        if (!s->peer_address) return;
        const int i = index();
        if (i < 0 || i >= OTG_BLE_MAX_PHONES) throw std::runtime_error("invalid fake phone");
        if (InterlockedCompareExchange(&test_shared->failed[i], 0, 0)) throw std::runtime_error("fake notify failure");
        uint64_t epoch;
        { std::lock_guard<std::mutex> lock(s->mutex); epoch = s->epoch; }
        const bool delayed = InterlockedCompareExchange(&test_shared->metadata_delay[i], 0, 0) != 0;
        if (metadata_active) {
            const auto result = metadata.step(Clock::now(), epoch, delayed, [&] {
                InterlockedIncrement(&test_shared->metadata_cancels[i]);
            });
            if (result == otg_ble_win_detail::MetadataResult::timed_out) {
                invalidate(s, "fake metadata check pending");
                release();
            }
            if (metadata.active()) return;
            metadata_active = false;
        }
        if (delayed) { metadata.start(Clock::now(), epoch); metadata_active = true; return; }
        if (!InterlockedCompareExchange(&test_shared->online[i], 0, 0)) { invalidate(s, "fake link lost"); return; }
        std::lock_guard<std::mutex> lock(s->mutex);
        if (!s->ready) {
            s->queue.clear();
            s->queue.push({1, {}, false, s->epoch});
            s->queue.push({2, {}, false, s->epoch});
            s->ready = true;
        }
    }
    void send(const otg_ble_win_detail::Report& report, Clock::time_point& next)
    {
        const int i = index();
        InterlockedExchange(&test_shared->in_flight[i], 1);
        const auto deadline = Clock::now() + (InterlockedCompareExchange(&test_shared->stall[i], 0, 0) == 2 ? 5s : 1500ms);
        while (!s->stopping && Clock::now() < deadline &&
               InterlockedCompareExchange(&test_shared->stall[i], 0, 0)) std::this_thread::sleep_for(1ms);
        if (s->stopping) return;
        if (InterlockedCompareExchange(&test_shared->stall[i], 0, 0)) throw std::runtime_error("fake notify timeout");
        InterlockedExchange(report.id == 1 ? &test_shared->keys[i] : &test_shared->buttons[i], report.bytes[report.id == 1 ? 2 : 1]);
        InterlockedIncrement(&test_shared->sent[i]);
        InterlockedExchange(&test_shared->in_flight[i], 0);
        next = Clock::now() + 1ms;
    }
    void release()
    {
        if (!s->peer_address) return;
        InterlockedExchange(&test_shared->keys[index()], 0);
        InterlockedExchange(&test_shared->buttons[index()], 0);
    }
    bool recover(const otg_ble_win_detail::WaitAborted&) { return false; }
    bool recover_disconnected(const hresult_error&) { return false; }
};
