/* Shared Windows GATT provider and private per-phone IPC connections.
 * Included by ble_win.cpp to reuse the same identity, queue and release gates.
 * SPDX-License-Identifier: Apache-2.0 */
namespace {
static bool collect_descriptor(void *ctx, uint16_t id, const uint8_t *data, uint16_t length)
{
    auto state = static_cast<State *>(ctx);
    return otg_ble_win_detail::append_descriptor(state->report_map, data, length, static_cast<uint8_t>(id));
}
static bool descriptor_send(void *, uint16_t, const uint8_t *, uint16_t) { return false; }
static bool descriptor_poll(void *) { return true; }
static void descriptor_close(void *) {}

struct MultiConnection {
    std::atomic<bool> done{false};
    std::thread thread;
    std::unique_ptr<otg_ble_win> bridge = std::make_unique<otg_ble_win>();
    ~MultiConnection() { if (thread.joinable()) thread.join(); }
};

static void serve_phone(HANDLE raw, MultiConnection *connection, const Service *host,
    const std::shared_ptr<State>& owner, const std::atomic<bool>& stopping)
{
    using namespace otg_ble_win_pipe;
    Handle pipe(raw);
    auto b = connection->bridge.get();
    auto s = b->state;
    try {
        ULONG pid = 0;
        Packet packet;
        if (!GetNamedPipeClientProcessId(pipe.value, &pid) || !same_user(pid) ||
            !transfer(pipe.value, packet, false) || !valid_request(packet, true) ||
            packet.length != owner->report_map.size() ||
            !std::equal(owner->report_map.begin(), owner->report_map.end(), packet.data.begin()))
            throw std::runtime_error("invalid multi-phone client or HID descriptor");
        s->router = owner->router;
        s->peer_address = packet.address;
        if (packet.address && !s->router->claim(packet.address, s))
            throw std::runtime_error("phone already has a multi-phone session");
        b->thread = std::thread(worker, s, host);
        for (;;) {
            const auto op = packet.op;
            if (op == Op::release) otg_ble_win_release_inputs(b);
            if (op == Op::report && !send_report(b, static_cast<uint16_t>(packet.id), packet.data.data(), 8)) {
                std::lock_guard<std::mutex> lock(s->mutex);
                if (s->failed || s->closing || s->stopping)
                    throw std::runtime_error("multi-phone report rejected");
                // Link/metadata readiness can change while a report is in IPC.
                // Drop that old action, report not-ready, and retain the session.
            }
            packet = Packet{};
            packet.op = op;
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                packet.flags = (s->ready ? Flag::online : 0u) | (s->failed ? Flag::failed : 0u) |
                    (s->queue.empty() && !s->in_flight ? Flag::idle : 0u);
                packet.address = s->ready ? s->peer_address : 0;
                // Replies can carry display metadata in the existing payload.
                if (s->ready && (op == Op::hello || op == Op::poll) && s->peer_name.size() <= packet.data.size()) {
                    packet.length = static_cast<uint32_t>(s->peer_name.size());
                    std::copy(s->peer_name.begin(), s->peer_name.end(), packet.data.begin());
                }
            }
            if (stopping || !transfer(pipe.value, packet, true) || packet.flags & Flag::failed ||
                !transfer(pipe.value, packet, false, 5000) || !valid_request(packet, false)) break;
        }
    } catch (const std::exception& e) { log("WARN", e.what()); }
    // Only this endpoint is released. Other phones retain their workers, CCCs
    // and report state, including if this client's process is killed.
    close_transport(b);
    if (s->router) s->router->forget(s); // Release ownership after inputs/notifications have settled.
    connection->done = true;
}
} // namespace

extern "C" int otg_ble_win_multi_service_run(const char *token)
{
    using namespace otg_ble_win_pipe;
    if (!token || !valid_token(token)) return 2;
    try {
        init_apartment(apartment_type::multi_threaded);
        auto owner = std::make_shared<State>();
        owner->router = std::make_shared<Router>();
        const otg_hid_transport_ops collect{collect_descriptor, descriptor_send, descriptor_poll,
            descriptor_poll, descriptor_close, nullptr};
        otg_bridge descriptor_bridge{};
        if (otg_bridge_open_transport(&descriptor_bridge, &collect, owner.get(), 1080, 1920, OTG_MOUSE_ABSOLUTE))
            throw std::runtime_error("cannot construct multi-phone HID descriptor");
        Service host(owner);
        Security security;
        if (!security.descriptor) throw std::runtime_error("cannot protect multi-phone pipe");
        bool first = true;
        auto make_pipe = [&] {
            const DWORD flags = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
            auto pipe = CreateNamedPipeA(name(token).c_str(), flags,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                PIPE_UNLIMITED_INSTANCES, sizeof(Packet) * 2, sizeof(Packet) * 2, 0, &security.attributes);
            if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create multi-phone pipe");
            first = false;
            return pipe;
        };
        Handle listener(make_pipe());
        host.start();
        std::atomic<bool> stopping{false};
        std::vector<std::unique_ptr<MultiConnection>> clients;
        std::puts("{\"event\":\"ble-multi-ready\"}");
        std::fflush(stdout);
        try {
            while (!stopping) {
                Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                if (!event.value) throw std::runtime_error("cannot create pipe connection event");
                PendingConnect connect(listener.value, event.value);
                const BOOL connected = ConnectNamedPipe(listener.value, &connect.operation);
                const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
                connect.pending = !connected && error == ERROR_IO_PENDING;
                if (!connected && error != ERROR_PIPE_CONNECTED && error != ERROR_IO_PENDING)
                    throw std::runtime_error("multi-phone pipe connect failed");
                bool accepted = connected || error == ERROR_PIPE_CONNECTED;
                while (!accepted && !stopping) {
                    accepted = WaitForSingleObject(event.value, 50) == WAIT_OBJECT_0;
                    DWORD count = 0;
                    if (!PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, &count, nullptr) || count)
                        stopping = true;
                    host.ensure_advertising();
                }
                DWORD available = 0;
                if (!PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, &available, nullptr) || available)
                    stopping = true;
                if (stopping) {
                    break;
                }
                if (connect.pending) {
                    DWORD ignored;
                    if (!GetOverlappedResult(listener.value, &connect.operation, &ignored, FALSE))
                        throw std::runtime_error("multi-phone pipe connection was cancelled");
                    connect.pending = false;
                }
                clients.erase(std::remove_if(clients.begin(), clients.end(),
                    [](const auto& client) { return client->done.load(); }), clients.end());
                HANDLE pipe = listener.value;
                listener.value = make_pipe();
                if (clients.size() >= OTG_BLE_MAX_PHONES) { CloseHandle(pipe); continue; }
                auto client = std::make_unique<MultiConnection>();
                auto ptr = client.get();
                client->thread = std::thread(serve_phone, pipe, ptr, &host, owner, std::cref(stopping));
                clients.push_back(std::move(client));
            }
        } catch (...) {
            stopping = true;
            clients.clear();
            throw;
        }
        stopping = true;
        clients.clear();
        return 0;
    } catch (const hresult_error& e) { log("ERROR", describe(e)); }
    catch (const std::exception& e) { log("ERROR", e.what()); }
    return 1;
}
