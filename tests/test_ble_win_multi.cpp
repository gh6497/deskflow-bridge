/* Actual named pipes, backend workers and bridge, with a fake GATT radio.
 * No Bluetooth adapter, pairing or system service is accessed. */
#include <windows.h>
#include <cstdint>
constexpr uint64_t test_address = 0xaabbcc000001ULL;
struct TestShared {
    volatile LONG online[8], failed[8], stall[8], keys[8], buttons[8], sent[8], in_flight[8];
    volatile LONG metadata_delay[8], metadata_cancels[8];
};
static TestShared *test_shared;
#define OTG_BLE_WIN_TEST
#include "../platform/windows/ble_win.cpp"
#include <QCoreApplication>
#include <QProcess>
#include <QTest>
#include <QUuid>
#include <QScopeGuard>

extern "C" bool aoa_hid_init(aoa_hid *) { std::abort(); }
extern "C" bool aoa_hid_open(aoa_hid *, const char *) { std::abort(); }
extern "C" void aoa_hid_destroy(aoa_hid *) { std::abort(); }
extern "C" bool aoa_hid_register(aoa_hid *, uint16_t, const uint8_t *, uint16_t) { std::abort(); }
extern "C" bool aoa_hid_send(aoa_hid *, uint16_t, const uint8_t *, uint16_t) { std::abort(); }
extern "C" bool aoa_hid_poll(aoa_hid *) { std::abort(); }
extern "C" bool aoa_hid_is_disconnected(const aoa_hid *) { std::abort(); }

class MultiTest : public QObject {
    Q_OBJECT
private slots:
    void protocol()
    {
        using namespace otg_ble_win_pipe;
        QVERIFY(valid_token(std::string(32, 'a')));
        QVERIFY(!valid_token("../../other"));
        Packet p;
        p.op = hello; p.length = 8;
        QVERIFY(valid_request(p, true));
        p.version = 2; QVERIFY(!valid_request(p, true)); p.version = 1;
        p.flags = Flag::online; QVERIFY(!valid_request(p, true)); p.flags = 0;
        p.op = report; p.id = 2;
        QVERIFY(valid_request(p, false));
        p.length = 9; QVERIFY(!valid_request(p, false)); p.length = 8;
        p.address = test_address; QVERIFY(!valid_request(p, false));
        p = Packet{}; p.op = Op::release;
        QVERIFY(valid_request(p, false));
        p.id = 1; QVERIFY(!valid_request(p, false));
        Router router;
        auto a = std::make_shared<State>(), b = std::make_shared<State>();
        QVERIFY(router.claim(test_address, a));
        QVERIFY(!router.claim(test_address, b));
        QVERIFY(router.claim(test_address + 1, b));
        QCOMPARE(router.find(test_address), a);
        QCOMPARE(router.find(test_address + 1), b);
        router.bind(L"a", a); router.bind(L"b", b);
        QVERIFY(router.owned_by_other(L"a", b));
        QVERIFY(!router.owned_by_other(L"a", a));
        QVERIFY(!router.owned_by_other(L"unclaimed", a));
        router.changed(0, {L"a"});
        auto added = router.changed(0, {L"a", L"b"});
        QCOMPARE(added.size(), size_t(1)); QCOMPARE(added.front(), b);
        auto removed = router.changed(0, {L"b"});
        QCOMPARE(removed.size(), size_t(1)); QCOMPARE(removed.front(), a);
        router.quarantine(test_address);
        QVERIFY(!router.claim(test_address, b));
        router.forget(b);
        QVERIFY(!router.find(test_address + 1));
        QVERIFY(!router.owned_by_other(L"b", a));
        QVERIFY(router.claim(test_address + 1, a));
    }
    void sessions()
    {
        using namespace otg_ble_win_pipe;
        const auto token = QUuid::createUuid().toString(QUuid::Id128).toStdString();
        const auto mapping = "Local\\DeskflowBleTest-" + token;
        Handle shared(CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(TestShared), mapping.c_str()));
        auto data = static_cast<TestShared *>(MapViewOfFile(shared.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TestShared)));
        QVERIFY(data);
        const auto unmap = qScopeGuard([&] { UnmapViewOfFile(data); });
        std::memset(data, 0, sizeof(*data));
        data->online[0] = data->online[1] = 1;
        QProcess server;
        server.start(QCoreApplication::applicationFilePath(), {"--server", QString::fromStdString(token)});
        QVERIFY(server.waitForStarted());
        const auto shutdown = qScopeGuard([&] { server.closeWriteChannel(); server.waitForFinished(12000); });
        QByteArray output;
        auto listening = [&] { output += server.readAllStandardOutput(); return output.contains("ble-multi-ready"); };
        QTRY_VERIFY_WITH_TIMEOUT(listening(), 10000);
        // A malformed descriptor closes only the offending pipe.
        {
            Handle bad(CreateFileA(name(token).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
            QVERIFY(bad.value != INVALID_HANDLE_VALUE);
            DWORD mode = PIPE_READMODE_MESSAGE;
            QVERIFY(SetNamedPipeHandleState(bad.value, &mode, nullptr, nullptr));
            Packet invalid;
            invalid.op = hello; invalid.length = 8;
            QVERIFY(transfer(bad.value, invalid, true));
            QVERIFY(!transfer(bad.value, invalid, false));
        }
        auto a = otg_ble_win_create_session(nullptr, token.c_str());
        auto b = otg_ble_win_create_session(nullptr, token.c_str());
        QVERIFY(a && b);
        const auto destroy = qScopeGuard([&] { otg_ble_win_destroy(a); otg_ble_win_destroy(b); });
        otg_bridge first{}, second{};
        QCOMPARE(otg_bridge_open_transport(&first, otg_ble_win_ops(), a, 540, 1170, OTG_MOUSE_ABSOLUTE), 0);
        QCOMPARE(otg_bridge_open_transport(&second, otg_ble_win_ops(), b, 720, 1600, OTG_MOUSE_ABSOLUTE), 0);
        QTRY_VERIFY(ready(a) && ready(b));
        const std::string identity_a = identity_transport(a), identity_b = identity_transport(b);
        QVERIFY(identity_a != identity_b);
        const int ai = identity_a == address_text(test_address) ? 0 : 1;
        const int bi = 1 - ai;
        const auto key = [&](int i) { return InterlockedCompareExchange(&data->keys[i], 0, 0); };
        otg_bridge_enter(&first, 100, 100, 0);
        otg_bridge_key(&first, 'a', true);
        otg_bridge_enter(&second, 200, 200, 0);
        otg_bridge_key(&second, 'b', true);
        QTRY_COMPARE(key(ai), LONG(4));
        QTRY_COMPARE(key(bi), LONG(5));
        auto duplicate = otg_ble_win_create_session(identity_a.c_str(), token.c_str());
        const auto close_duplicate = qScopeGuard([&] { otg_ble_win_destroy(duplicate); });
        otg_bridge rejected{};
        QCOMPARE(otg_bridge_open_transport(&rejected, otg_ble_win_ops(), duplicate, 540, 1170, OTG_MOUSE_ABSOLUTE), 0);
        QTRY_VERIFY(!poll_transport(duplicate));
        QVERIFY(ready(a) && ready(b));
        QTRY_VERIFY(otg_ble_win_input_idle(a));
        const LONG before = InterlockedCompareExchange(&data->sent[ai], 0, 0);
        InterlockedExchange(&data->stall[ai], 2);
        std::array<uint8_t, 8> pending{};
        pending[2] = 6;
        QVERIFY(send_report(a, 1, pending.data(), 8));
        QTRY_VERIFY(InterlockedCompareExchange(&data->in_flight[ai], 0, 0) != 0);
        for (uint8_t usage = 7; usage < 17; ++usage) {
            pending[2] = usage;
            QVERIFY(send_report(a, 1, pending.data(), 8));
        }
        const auto submitted = [&] {
            std::lock_guard<std::mutex> lock(a->state->mutex);
            return a->state->queue.empty() && !a->state->in_flight && !a->state->remote_release;
        };
        QTRY_VERIFY(submitted());
        otg_ble_win_release_inputs(a);
        QTRY_VERIFY(submitted()); // Release RPC acknowledged, prior unsent presses discarded remotely.
        QVERIFY(!otg_ble_win_input_idle(a)); // The old notification still has to finish.
        InterlockedExchange(&data->stall[ai], 0);
        QTRY_VERIFY(otg_ble_win_input_idle(a));
        QTRY_COMPARE(key(ai), LONG(0));
        // One in-flight press, then keyboard release, buttons up and range end.
        QCOMPARE(InterlockedCompareExchange(&data->sent[ai], 0, 0) - before, LONG(4));
        QCOMPARE(key(bi), LONG(5));
        // A metadata lookup can time out and stay pending after Cancel(). The
        // IPC endpoint and other phone must remain usable until it recovers.
        InterlockedExchange(&data->metadata_delay[ai], 1);
        QTRY_VERIFY(!ready(a));
        QVERIFY(poll_transport(a) && ready(b));
        QCOMPARE(InterlockedCompareExchange(&data->metadata_cancels[ai], 0, 0), LONG(1));
        std::array<uint8_t, 8> other_key{}; other_key[2] = 5;
        const LONG other_sent = InterlockedCompareExchange(&data->sent[bi], 0, 0);
        QVERIFY(send_report(b, 1, other_key.data(), 8));
        QTRY_VERIFY(InterlockedCompareExchange(&data->sent[bi], 0, 0) > other_sent);
        QVERIFY(poll_transport(a));
        InterlockedExchange(&data->metadata_delay[ai], 0);
        QTRY_VERIFY(ready(a));
        QCOMPARE(InterlockedCompareExchange(&data->metadata_cancels[ai], 0, 0), LONG(1));
        QCOMPARE(std::string(identity_transport(a)), identity_a);

        // A report already in the pipe when readiness drops is discarded, not
        // treated as a protocol error that tears down the endpoint.
        {
            Handle waiting(CreateFileA(name(token).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
            QVERIFY(waiting.value != INVALID_HANDLE_VALUE);
            DWORD mode = PIPE_READMODE_MESSAGE;
            QVERIFY(SetNamedPipeHandleState(waiting.value, &mode, nullptr, nullptr));
            Packet request;
            request.op = hello; request.address = test_address + 3;
            request.length = static_cast<uint32_t>(a->state->report_map.size());
            std::copy(a->state->report_map.begin(), a->state->report_map.end(), request.data.begin());
            QVERIFY(transfer(waiting.value, request, true) && transfer(waiting.value, request, false));
            QVERIFY(!(request.flags & Flag::online));
            request = Packet{}; request.op = report; request.id = 1; request.length = 8; request.data[2] = 4;
            QVERIFY(transfer(waiting.value, request, true) && transfer(waiting.value, request, false));
            QVERIFY(!(request.flags & (Flag::online | Flag::failed)));
            request = Packet{}; request.op = Op::poll;
            QVERIFY(transfer(waiting.value, request, true) && transfer(waiting.value, request, false));
        }
        // Abrupt bridge process exit releases only its phone.
        InterlockedExchange(&data->online[2], 1);
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--client", QString::fromStdString(token),
            QString::fromStdString(address_text(test_address + 2))});
        QVERIFY(child.waitForStarted());
        QTRY_COMPARE(key(2), LONG(29));
        auto listener = otg_ble_win_create_session(nullptr, token.c_str());
        const auto close_listener = qScopeGuard([&] { otg_ble_win_destroy(listener); });
        otg_bridge listening_bridge{};
        QCOMPARE(otg_bridge_open_transport(&listening_bridge, otg_ble_win_ops(), listener, 540, 1170, OTG_MOUSE_ABSOLUTE), 0);
        child.kill();
        QVERIFY(child.waitForFinished(5000));
        QTRY_COMPARE(key(2), LONG(0));
        QTRY_VERIFY(ready(listener)); // Existing discovery can claim a released identity without a new pipe accept.
        QCOMPARE(std::string(identity_transport(listener)), address_text(test_address + 2));
        otg_bridge_close(&listening_bridge);
        QVERIFY(ready(a) && ready(b));
        // Pause/release is acknowledged only after the remote queue drains.
        otg_ble_win_release_inputs(a);
        QTRY_VERIFY(otg_ble_win_input_idle(a));
        QTRY_COMPARE(key(ai), LONG(0));
        QCOMPARE(key(bi), LONG(5));
        InterlockedExchange(&data->online[ai], 0);
        QTRY_VERIFY(!ready(a));
        QVERIFY(ready(b));
        InterlockedExchange(&data->online[ai], 1);
        QTRY_VERIFY(ready(a));
        // One slow/failing phone cannot delay or terminate another phone.
        InterlockedExchange(&data->stall[ai], 1);
        otg_bridge_key(&first, 'c', true);
        otg_bridge_key(&second, 'b', false);
        otg_bridge_key(&second, 'd', true);
        QTRY_COMPARE(key(bi), LONG(7));
        QTRY_VERIFY_WITH_TIMEOUT(!poll_transport(a), 6000);
        QVERIFY(ready(b));
        otg_bridge_close(&first);
        QVERIFY(server.state() == QProcess::Running);
        server.closeWriteChannel(); // GUI crash/EOF ends its owned service and all endpoints.
        QVERIFY(server.waitForFinished(12000));
        QTRY_VERIFY(!poll_transport(b));
        QTRY_COMPARE(key(bi), LONG(0));
        otg_bridge_close(&second);
    }
};

int main(int argc, char **argv)
{
    if (argc == 4 && std::string(argv[1]) == "--client") {
        auto b = otg_ble_win_create_session(argv[3], argv[2]);
        otg_bridge bridge{};
        if (!b || otg_bridge_open_transport(&bridge, otg_ble_win_ops(), b, 540, 1170, OTG_MOUSE_ABSOLUTE)) return 2;
        const auto deadline = Clock::now() + 5s;
        while (!ready(b) && poll_transport(b) && Clock::now() < deadline) Sleep(1);
        if (!ready(b)) return 3;
        otg_bridge_enter(&bridge, 0, 0, 0);
        otg_bridge_key(&bridge, 'z', true);
        (void)std::getchar();
        otg_ble_win_destroy(b);
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--server") {
        const auto mapping = std::string("Local\\DeskflowBleTest-") + argv[2];
        otg_ble_win_pipe::Handle memory(OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mapping.c_str()));
        test_shared = static_cast<TestShared *>(MapViewOfFile(memory.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TestShared)));
        if (!test_shared) return 2;
        const int result = otg_ble_win_multi_service_run(argv[2]);
        UnmapViewOfFile(test_shared);
        return result;
    }
    QCoreApplication app(argc, argv);
    MultiTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "test_ble_win_multi.moc"
