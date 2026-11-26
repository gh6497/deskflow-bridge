/* Windows HID over GATT adapter. GATT layout follows platform/linux/ble_hid.c; reports
 * come from bridge.c (scrcpy keyboard / absolute tablet), not the prototype.
 * SPDX-License-Identifier: Apache-2.0 */
#include "ble_win.h"
#include "ble_win_queue.h"
#include "ble_win_wait.h"
#include "ble_win_metadata.h"
#include "ble_win_pipe.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>
extern "C" {
#include "bridge.h"
}
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <map>
#include <set>
#include <algorithm>
#include "ble_limits.h"

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
struct State;
struct Router {
    std::mutex mutex;
    std::map<uint64_t, std::weak_ptr<State>> phones;
    std::set<uint64_t> quarantined;
    void quarantine(uint64_t address)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (address) quarantined.insert(address);
    }
    std::map<std::wstring, std::weak_ptr<State>> devices;
    std::map<unsigned, std::vector<std::wstring>> subscriptions;
    void bind(const hstring& id, const std::shared_ptr<State>& state)
    {
        std::lock_guard<std::mutex> lock(mutex);
        devices[std::wstring(id)] = state;
    }
    bool owned_by_other(const hstring& id, const std::shared_ptr<State>& state)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = devices.find(std::wstring(id));
        if (it == devices.end()) return false;
        auto current = it->second.lock();
        return current && current != state;
    }
    void forget(const std::shared_ptr<State>& state)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = phones.begin(); it != phones.end(); ) {
            if (it->second.lock() == state) it = phones.erase(it);
            else ++it;
        }
        for (auto it = devices.begin(); it != devices.end(); ) {
            if (it->second.lock() == state) it = devices.erase(it);
            else ++it;
        }
    }
    std::vector<std::shared_ptr<State>> changed(unsigned kind, const std::vector<std::wstring>& now)
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::shared_ptr<State>> affected;
        const auto before = subscriptions[kind];
        auto changed_id = [&](const std::wstring& id) {
            auto it = devices.find(id);
            if (it != devices.end()) if (auto phone = it->second.lock()) affected.push_back(phone);
        };
        for (const auto& id : before) if (std::find(now.begin(), now.end(), id) == now.end()) changed_id(id);
        for (const auto& id : now) if (std::find(before.begin(), before.end(), id) == before.end()) changed_id(id);
        subscriptions[kind] = now;
        for (auto it = devices.begin(); it != devices.end(); ) {
            if (it->second.expired()) it = devices.erase(it);
            else ++it;
        }
        return affected;
    }
    bool claim(uint64_t address, const std::shared_ptr<State>& state)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (quarantined.count(address)) return false;
        for (auto it = phones.begin(); it != phones.end(); ) {
            if (it->second.expired()) it = phones.erase(it);
            else ++it;
        }
        auto current = phones[address].lock();
        if (current && current != state) return false;
        phones[address] = state;
        return true;
    }
    std::shared_ptr<State> find(uint64_t address)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = phones.find(address);
        return it == phones.end() ? nullptr : it->second.lock();
    }
};
struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> stopping{false};
    bool ready = false, failed = false, closing = false, in_flight = false, worker_done = false;
    bool suspended = false;
    uint64_t peer_address = 0, epoch = 0;
    std::chrono::microseconds state_interval = otg_ble_win_detail::fallback_state_interval;
    std::chrono::microseconds motion_interval = otg_ble_win_detail::fallback_motion_interval;
    uint8_t protocol_mode = 1; // Report Protocol; Boot Protocol is not implemented.
    std::string waiting;
    otg_ble_win_detail::Queue queue;
    std::array<uint8_t, 8> keyboard{}, pointer{}, last_mouse{};
    uint8_t leds = 0;
    std::vector<uint8_t> report_map;
    std::shared_ptr<Router> router;
    bool remote_idle = true;
    bool remote_release = false;
};

static std::shared_ptr<State> routed(const std::shared_ptr<State>& state, uint64_t address)
{
    if (!state->router) return state;
    // The host's reports remain zero. Before a listener claims a paired phone,
    // reads return neutral reports, never another phone's input state.
    auto phone = state->router->find(address);
    return phone ? phone : state;
}

static void log(const char *level, const std::string& message)
{
    static const auto started = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    std::fprintf(stderr, "%s: Windows BLE [%lldms] %s\n", level, static_cast<long long>(elapsed), message.c_str());
}

static void set_connection_interval(const std::shared_ptr<State>& state, uint16_t units)
{
    if (!units) return;
    const auto interval = otg_ble_win_detail::connection_interval(units);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->state_interval = state->motion_interval = interval;
    }
    log("INFO", "BLE negotiated connection interval=" + std::to_string(interval.count()) + "us");
}
static std::string describe(const hresult_error& e)
{
    return "HRESULT=" + std::to_string(e.code().value) + " " + to_string(e.message());
}
static std::string address_text(uint64_t address)
{
    char text[18];
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
        static_cast<unsigned>((address >> 40) & 0xff), static_cast<unsigned>((address >> 32) & 0xff),
        static_cast<unsigned>((address >> 24) & 0xff), static_cast<unsigned>((address >> 16) & 0xff),
        static_cast<unsigned>((address >> 8) & 0xff), static_cast<unsigned>(address & 0xff));
    return text;
}
static guid uuid(uint16_t id)
{
    return {id, 0x0000, 0x1000, {0x80,0x00,0x00,0x80,0x5f,0x9b,0x34,0xfb}};
}
static std::string session_text(const GattSession& session)
{
    try {
        return session.SessionStatus() == GattSessionStatus::Active ? "active" : "closed";
    } catch (...) { return "unavailable"; }
}
template<class Bytes> static IBuffer buffer(const Bytes& bytes)
{
    DataWriter writer;
    writer.WriteBytes(bytes);
    return writer.DetachBuffer();
}

/* Only the worker waits for WinRT operations. Cancellation/timeouts never
 * block Deskflow. Poll Status rather than reassigning the Completed delegate. */
template<class Operation> static auto wait(const std::shared_ptr<State>& s,
    const Operation& operation, const char *name, std::chrono::milliseconds timeout = 5s,
    std::optional<uint64_t> epoch = {})
{
    using namespace otg_ble_win_detail;
    const auto deadline = Clock::now() + timeout;
    return wait_operation(name,
        [&] { return operation.Status() == Windows::Foundation::AsyncStatus::Started; },
        [&] { operation.Cancel(); }, [&] { return operation.GetResults(); },
        [&](bool pending) {
            if (s->stopping) return WaitReason::stopping;
            if (epoch) {
                {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    if (*epoch != s->epoch) return WaitReason::session_changed;
                }
                if (operation.Status() == Windows::Foundation::AsyncStatus::Canceled)
                    return WaitReason::session_changed;
            }
            return pending && Clock::now() >= deadline ? WaitReason::timed_out : WaitReason::none;
        }, [] { std::this_thread::sleep_for(1ms); });
}

/* A new readiness epoch discards unsent input and its read-back state. */
static void invalidate(const std::shared_ptr<State>& s, const char *reason, bool force = false)
{
    bool changed;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        changed = s->waiting != reason;
        s->waiting = reason;
        if (s->ready || force) {
            s->ready = false;
            ++s->epoch;
            s->queue.clear();
            s->keyboard = {};
            s->pointer[0] = s->pointer[1] = 0;
            s->pointer[6] = s->pointer[7] = 0;
            s->last_mouse = s->pointer;
        }
    }
    s->cv.notify_all();
    if (changed) log("INFO", std::string("waiting: ") + reason);
}

static void fail(const std::shared_ptr<State>& s, const std::string& reason)
{
    invalidate(s, "transport failure", true);
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->failed = true;
    }
    s->cv.notify_all();
    log("ERROR", reason);
}

static Windows::Foundation::IAsyncOperation<uint64_t> paired_address(GattSession session)
{
    auto device = co_await BluetoothLEDevice::FromIdAsync(session.DeviceId().Id());
    if (device && device.DeviceInformation().Pairing().IsPaired() &&
        device.ConnectionStatus() == BluetoothConnectionStatus::Connected)
        co_return device.BluetoothAddress();
    co_return 0;
}

static fire_and_forget read_report(GattReadRequestedEventArgs args,
    std::shared_ptr<State> s, unsigned kind)
{
    auto deferral = args.GetDeferral();
    try {
        auto request = co_await args.GetRequestAsync();
        if (request) {
            const auto address = co_await paired_address(args.Session());
            IBuffer value{nullptr};
            s = routed(s, address);
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                if (address && (!s->peer_address || address == s->peer_address))
                    value = kind == 0 ? buffer(s->keyboard) : kind == 1 ? buffer(s->pointer) :
                        buffer(std::array<uint8_t, 1>{s->leds});
            }
            if (!value) {
                log("WARN", "report read rejected by identity/pairing gate: kind=" + std::to_string(kind) +
                    " DeviceId=" + to_string(args.Session().DeviceId().Id()));
                request.RespondWithProtocolError(0x08); // insufficient authorization
                deferral.Complete();
                co_return;
            }
            request.RespondWithValue(value);
        }
    } catch (const hresult_error& e) { log("WARN", "read: " + describe(e)); }
    deferral.Complete();
}

static fire_and_forget write_value(GattWriteRequestedEventArgs args,
    std::shared_ptr<State> s, bool control)
{
    auto deferral = args.GetDeferral();
    try {
        auto request = co_await args.GetRequestAsync();
        if (request) {
            uint8_t error = 0;
            const auto address = co_await paired_address(args.Session());
            bool allowed;
            s = routed(s, address);
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                allowed = address && address == s->peer_address;
            }
            if (!allowed) {
                log("WARN", std::string(control ? "control" : "LED") +
                    " write rejected by identity/pairing gate: DeviceId=" + to_string(args.Session().DeviceId().Id()));
                error = 0x08;
            }
            else if (request.Offset() || request.Value().Length() != 1) error = 0x0d;
            else {
                auto value = DataReader::FromBuffer(request.Value()).ReadByte();
                if (control && value > 1) error = 0x13;
                else {
                    {
                        std::lock_guard<std::mutex> lock(s->mutex);
                        if (control) s->suspended = value == 0;
                        else s->leds = value;
                    }
                    if (control) invalidate(s, value == 0 ? "HID suspended" : "HID resume", true);
                }
            }
            if (request.Option() == GattWriteOption::WriteWithResponse) {
                if (error) request.RespondWithProtocolError(error);
                else request.Respond();
            }
        }
    } catch (const hresult_error& e) { log("WARN", "write: " + describe(e)); }
    deferral.Complete();
}

static fire_and_forget write_protocol_mode(GattWriteRequestedEventArgs args,
    std::shared_ptr<State> s)
{
    auto deferral = args.GetDeferral();
    try {
        auto request = co_await args.GetRequestAsync();
        if (request) {
            const auto address = co_await paired_address(args.Session());
            uint8_t error = 0;
            s = routed(s, address);
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                if (!address || (s->peer_address && address != s->peer_address)) error = 0x08;
                else if (request.Offset() || request.Value().Length() != 1) error = 0x0d;
                // The bridge only supports Report Protocol.
                else if (DataReader::FromBuffer(request.Value()).ReadByte() != 1) error = 0x03;
                else s->protocol_mode = 1;
            }
            if (request.Option() == GattWriteOption::WriteWithResponse) {
                if (error) request.RespondWithProtocolError(error);
                else request.Respond();
            }
        }
    } catch (const hresult_error& e) { log("WARN", "protocol mode write: " + describe(e)); }
    deferral.Complete();
}

static fire_and_forget read_protocol_mode(GattReadRequestedEventArgs args,
    std::shared_ptr<State> s)
{
    auto deferral = args.GetDeferral();
    try {
        auto request = co_await args.GetRequestAsync();
        if (request) {
            uint8_t mode;
            { std::lock_guard<std::mutex> lock(s->mutex); mode = s->protocol_mode; }
            request.RespondWithValue(buffer(std::array<uint8_t, 1>{mode}));
        }
    } catch (const hresult_error& e) { log("WARN", "protocol mode read: " + describe(e)); }
    deferral.Complete();
}

#ifndef OTG_BLE_WIN_TEST
class Service {
    std::shared_ptr<State> s;
    GattServiceProvider provider{nullptr};
    GattLocalCharacteristic keyboard{nullptr}, pointer{nullptr};
    GattLocalCharacteristic protocol_mode{nullptr};
    GattServiceProvider battery_provider{nullptr};
    BluetoothLEDevice peer{nullptr};
    GattSubscribedClient key_client{nullptr}, pointer_client{nullptr};
    BluetoothLEDevice::ConnectionStatusChanged_revoker connection_changed;
    Windows::Foundation::IAsyncOperation<GattClientNotificationResult> notification{nullptr};
    Clock::time_point notification_started{};
    uint64_t sent_keyboard = 0, sent_pointer = 0;
    hstring peer_id;
    Clock::time_point pairing_refresh{};
    Clock::time_point advertisement_retry{};
    bool paired = false;
    Windows::Foundation::IAsyncOperation<BluetoothLEDevice> resolving{nullptr};
    Windows::Foundation::IAsyncOperation<Windows::Devices::Enumeration::DeviceInformation> pairing_query{nullptr};
    otg_ble_win_detail::MetadataRequest resolve_request, pairing_request;
    otg_ble_win_detail::MetadataLease pairing_lease;
    hstring resolving_id;
    std::map<std::wstring, Clock::time_point> resolve_retry;
    Clock::time_point pairing_retry{};
    BluetoothLEDevice::ConnectionParametersChanged_revoker parameters_changed;

    bool multi_subscriber(uint64_t epoch, GattSubscribedClient& key, GattSubscribedClient& mouse,
                          BluetoothLEDevice& resolved)
    {
        using namespace otg_ble_win_detail;
        const auto now = Clock::now();
        if (resolving) {
            const auto result = resolve_request.step(now, epoch,
                resolving.Status() == Windows::Foundation::AsyncStatus::Started, [&] { resolving.Cancel(); });
            if (result == MetadataResult::timed_out) {
                log("WARN", "verify multi subscriber: timed out; DeviceId=" + to_string(resolving_id) +
                    "; retaining session, retrying after cancellation completes");
                resolve_retry[std::wstring(resolving_id)] = now + 1s;
            }
            if (resolve_request.active()) return false;
            auto operation = std::move(resolving);
            resolving = nullptr;
            if (result == MetadataResult::complete) {
                try {
                    auto device = operation.GetResults();
                    auto current_key = subscriber(keyboard, resolving_id);
                    auto current_mouse = subscriber(pointer, resolving_id);
                    uint64_t target;
                    { std::lock_guard<std::mutex> lock(s->mutex); target = s->peer_address; }
                    if (device && current_key && current_mouse &&
                        device.ConnectionStatus() == BluetoothConnectionStatus::Connected &&
                        device.DeviceInformation().Pairing().IsPaired()) {
                        const auto address = device.BluetoothAddress();
                        if (address && (!target || target == address) && s->router->claim(address, s)) {
                            { std::lock_guard<std::mutex> lock(s->mutex); s->peer_address = address; }
                            s->router->bind(resolving_id, s);
                            key = current_key; mouse = current_mouse; resolved = device;
                            resolve_retry.erase(std::wstring(resolving_id));
                            return true;
                        }
                    }
                } catch (const hresult_error& e) { log("WARN", "verify multi subscriber: " + describe(e) + "; retrying"); }
            }
            resolve_retry[std::wstring(resolving_id)] = now + 1s;
        }
        for (const auto& candidate : keyboard.SubscribedClients()) {
            const auto id = candidate.Session().DeviceId().Id();
            if (s->router->owned_by_other(id, s) || !subscriber(pointer, id) || !subscriber(keyboard, id)) continue;
            const auto retry = resolve_retry.find(std::wstring(id));
            if (retry != resolve_retry.end() && now < retry->second) continue;
            try {
                resolving_id = id;
                resolving = BluetoothLEDevice::FromIdAsync(id);
                resolve_request.start(now, epoch, 5s);
            } catch (const hresult_error& e) {
                resolve_retry[std::wstring(id)] = now + 1s;
                log("WARN", "verify multi subscriber: " + describe(e) + "; retrying");
            }
            break;
        }
        return false;
    }

    bool multi_pairing(uint64_t epoch)
    {
        using namespace otg_ble_win_detail;
        const auto now = Clock::now();
        if (pairing_query) {
            const auto result = pairing_request.step(now, epoch,
                pairing_query.Status() == Windows::Foundation::AsyncStatus::Started, [&] { pairing_query.Cancel(); });
            if (result == MetadataResult::timed_out) {
                log("WARN", "refresh pairing state: timed out; retaining session and retrying");
                pairing_retry = now + 1s;
            }
            if (!pairing_request.active()) {
                auto operation = std::move(pairing_query);
                pairing_query = nullptr;
                if (result == MetadataResult::complete) {
                    try {
                        auto info = operation.GetResults();
                        paired = info && info.Pairing().IsPaired();
                        if (paired) pairing_lease.renew(now, epoch);
                        else pairing_lease.clear();
                    } catch (const hresult_error& e) {
                        log("WARN", "refresh pairing state: " + describe(e) + "; retrying");
                    }
                }
                pairing_retry = now + 1s;
            }
        }
        if (!pairing_query && now >= pairing_retry) {
            try {
                auto existing = peer.DeviceInformation();
                pairing_query = Windows::Devices::Enumeration::DeviceInformation::CreateFromIdAsync(
                    existing.Id(), nullptr, existing.Kind());
                pairing_request.start(now, epoch);
            } catch (const hresult_error& e) {
                pairing_retry = now + 1s;
                log("WARN", "refresh pairing state: " + describe(e) + "; retrying");
            }
        }
        return paired && pairing_lease.valid(now, epoch);
    }

    GattLocalCharacteristic characteristic(uint16_t id, GattCharacteristicProperties properties,
        const std::vector<uint8_t>& value = {})
    {
        GattLocalCharacteristicParameters parameters;
        parameters.CharacteristicProperties(properties);
        parameters.ReadProtectionLevel(GattProtectionLevel::EncryptionRequired);
        parameters.WriteProtectionLevel(GattProtectionLevel::EncryptionRequired);
        if (!value.empty()) parameters.StaticValue(buffer(value));
        auto result = wait(s, provider.Service().CreateCharacteristicAsync(uuid(id), parameters), "create characteristic");
        if (result.Error() != BluetoothError::Success)
            throw std::runtime_error("create characteristic " + std::to_string(id) +
                ": BluetoothError=" + std::to_string(static_cast<int>(result.Error())));
        return result.Characteristic();
    }

    void reference(const GattLocalCharacteristic& c, uint8_t id, uint8_t type)
    {
        GattLocalDescriptorParameters parameters;
        parameters.ReadProtectionLevel(GattProtectionLevel::EncryptionRequired);
        parameters.StaticValue(buffer(std::array<uint8_t, 2>{id, type}));
        auto result = wait(s, c.CreateDescriptorAsync(uuid(0x2908), parameters), "create Report Reference");
        if (result.Error() != BluetoothError::Success)
            throw std::runtime_error("create Report Reference failed");
    }

    GattLocalCharacteristic input(uint8_t id, unsigned kind)
    {
        auto c = characteristic(0x2a4d,
            GattCharacteristicProperties::Read | GattCharacteristicProperties::Notify);
        reference(c, id, 1);
        auto state = s;
        c.ReadRequested([state, kind](auto const&, auto const& args) { read_report(args, state, kind); });
        c.SubscribedClientsChanged([state, kind](auto const& characteristic, auto const&) {
            if (!state->router) { invalidate(state, "subscriptions changed", true); return; }
            try {
                std::vector<std::wstring> ids;
                for (const auto& client : characteristic.SubscribedClients())
                    ids.emplace_back(client.Session().DeviceId().Id());
                for (const auto& phone : state->router->changed(kind, ids))
                    invalidate(phone, "phone subscriptions changed", true);
            } catch (const hresult_error& e) { log("WARN", "subscription snapshot: " + describe(e)); }
        });
        return c;
    }

public:
    explicit Service(std::shared_ptr<State> state, const Service *host = nullptr) : s(std::move(state))
    {
        if (host) { keyboard = host->keyboard; pointer = host->pointer; protocol_mode = host->protocol_mode; }
    }

    GattSubscribedClient subscriber(const GattLocalCharacteristic& c, const hstring& id)
    {
        GattSubscribedClient found{nullptr};
        auto clients = c.SubscribedClients();
        if (!s->router && clients.Size() != 1) return nullptr;
        for (const auto& client : clients) {
            if (client.Session().DeviceId().Id() != id) continue;
            if (found) return nullptr;
            found = client;
        }
        return found;
    }

    void ensure_advertising()
    {
        if (!provider) return;
        const auto status = provider.AdvertisementStatus();
        if (status == GattServiceProviderAdvertisementStatus::Started ||
            status == GattServiceProviderAdvertisementStatus::StartedWithoutAllAdvertisementData)
            return;

        // Windows can transiently report Aborted while an advertisement is
        // being replaced (notably when a central connects).  Do not tear down
        // the GATT provider in that window: retry the advertisement instead.
        if (status != GattServiceProviderAdvertisementStatus::Aborted &&
            status != GattServiceProviderAdvertisementStatus::Stopped)
            return;
        // StartAdvertising is asynchronous.  In particular, Windows may
        // briefly expose Aborted before the initial request reaches Started;
        // calling StartAdvertising again immediately fails with
        // RPC_E_WRONG_THREAD/invalid timing ("called at an unexpected time").
        // Give the original request time to settle before retrying.
        const auto now = Clock::now();
        if (now < advertisement_retry) return;
        advertisement_retry = now + 500ms;
        GattServiceProviderAdvertisingParameters advertising;
        advertising.IsConnectable(true);
        advertising.IsDiscoverable(true);
        log("WARN", "BLE advertisement stopped; restarting it");
        try {
            provider.StartAdvertising(advertising);
        } catch (const hresult_error& e) {
            log("WARN", "BLE advertisement restart deferred: " + describe(e));
        }
    }

    ~Service()
    {
        try { if (resolving) resolving.Cancel(); } catch (...) {}
        try { if (pairing_query) pairing_query.Cancel(); } catch (...) {}
        // A timed-out notification can still complete after Cancel(). Do not
        // let a new IPC session overtake it on the same phone. Other phones
        // remain usable even if this peer must wait for a service restart.
        if (!provider && s->router && notification) {
            try {
                if (notification.Status() == Windows::Foundation::AsyncStatus::Started) {
                    notification.Cancel();
                    const auto deadline = Clock::now() + 5s;
                    while (notification.Status() == Windows::Foundation::AsyncStatus::Started && Clock::now() < deadline)
                        std::this_thread::sleep_for(1ms);
                    if (notification.Status() == Windows::Foundation::AsyncStatus::Started) {
                        s->router->quarantine(s->peer_address);
                        log("ERROR", "unsettled notification; phone quarantined until shared service stops");
                    }
                }
            } catch (...) { s->router->quarantine(s->peer_address); }
        }
        connection_changed.revoke();
        parameters_changed.revoke();
        if (provider) { try { provider.StopAdvertising(); } catch (...) {} }
    }

    void start()
    {
        auto adapter = wait(s, BluetoothAdapter::GetDefaultAsync(), "get default adapter");
        if (!adapter || !adapter.IsLowEnergySupported() || !adapter.IsPeripheralRoleSupported())
            throw std::runtime_error("default adapter/driver does not support BLE peripheral role");
        auto result = wait(s, GattServiceProvider::CreateAsync(uuid(0x1812)), "create HID service");
        if (result.Error() != BluetoothError::Success)
            throw std::runtime_error("create HID service: BluetoothError=" +
                std::to_string(static_cast<int>(result.Error())));
        provider = result.ServiceProvider();
        characteristic(0x2a4a, GattCharacteristicProperties::Read, {0x11,0x01,0,0x03});
        characteristic(0x2a4b, GattCharacteristicProperties::Read, s->report_map);
        protocol_mode = characteristic(0x2a4e,
            GattCharacteristicProperties::Read | GattCharacteristicProperties::WriteWithoutResponse);
        auto state = s;
        protocol_mode.ReadRequested([state](auto const&, auto const& args) {
            read_protocol_mode(args, state);
        });
        protocol_mode.WriteRequested([state](auto const&, auto const& args) {
            write_protocol_mode(args, state);
        });
        keyboard = input(1, 0);
        pointer = input(2, 1);
        auto led = characteristic(0x2a4d, GattCharacteristicProperties::Read |
            GattCharacteristicProperties::Write | GattCharacteristicProperties::WriteWithoutResponse);
        reference(led, 1, 2);
        led.ReadRequested([state](auto const&, auto const& args) { read_report(args, state, 2); });
        led.WriteRequested([state](auto const&, auto const& args) { write_value(args, state, false); });
        auto control = characteristic(0x2a4c, GattCharacteristicProperties::WriteWithoutResponse);
        control.WriteRequested([state](auto const&, auto const& args) { write_value(args, state, true); });
        try {
            auto battery = wait(s, GattServiceProvider::CreateAsync(uuid(0x180f)), "create battery service");
            if (battery.Error() == BluetoothError::Success) {
                battery_provider = battery.ServiceProvider();
                GattLocalCharacteristicParameters parameters;
                parameters.CharacteristicProperties(GattCharacteristicProperties::Read);
                parameters.ReadProtectionLevel(GattProtectionLevel::Plain);
                parameters.StaticValue(buffer(std::array<uint8_t, 1>{100}));
                auto level = wait(s, battery_provider.Service().CreateCharacteristicAsync(
                    uuid(0x2a19), parameters), "create battery level");
                if (level.Error() != BluetoothError::Success)
                    log("WARN", "create battery level failed: BluetoothError=" +
                        std::to_string(static_cast<int>(level.Error())));
            } else {
                log("WARN", "create battery service failed: BluetoothError=" +
                    std::to_string(static_cast<int>(battery.Error())));
            }
        } catch (const hresult_error& e) { log("WARN", "battery service: " + describe(e)); }
        provider.AdvertisementStatusChanged([state](auto const&, auto const& args) {
            log("INFO", "advertisement status=" + std::to_string(static_cast<int>(args.Status())) +
                " BluetoothError=" + std::to_string(static_cast<int>(args.Error())));
            // Aborted is a transient status on some Windows Bluetooth
            // drivers.  The worker checks the status and restarts advertising;
            // failing the transport here makes the PC disappear from scans.
            (void)state;
        });
        GattServiceProviderAdvertisingParameters advertising;
        advertising.IsConnectable(true);
        advertising.IsDiscoverable(true);
        provider.StartAdvertising(advertising);
        log("INFO", "HID service started; pair/connect a phone from Android Bluetooth settings");
        if (s->router) log("INFO", "multi-phone identity checks are asynchronous; verified phone objects are reused");
    }

    void refresh()
    {
        uint64_t epoch;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (s->failed || s->closing) return;
            if (s->suspended) return;
            epoch = s->epoch;
        }
        // Cancel() is not completion. Keep the old operation alive and never
        // allow new-session reports to overtake an unsettled notification.
        if (notification) {
            if (notification.Status() == Windows::Foundation::AsyncStatus::Started) {
                if (Clock::now() - notification_started > 5s)
                    throw std::runtime_error("cancelled notification did not settle within 5 seconds");
                invalidate(s, "waiting for old notification cancellation to complete");
                return;
            }
            notification = nullptr; // old-session result is deliberately discarded
        }
        auto keys = keyboard.SubscribedClients();
        auto mice = pointer.SubscribedClients();
        GattSubscribedClient key{nullptr}, mouse{nullptr};
        BluetoothLEDevice resolved{nullptr};
        if (s->router) {
            // Once selected, use this phone's verified online object. Never
            // resolve every subscriber again in the notification worker.
            if (peer && !peer_id.empty()) {
                key = subscriber(keyboard, peer_id);
                mouse = subscriber(pointer, peer_id);
            }
            if (key && mouse && resolving) {
                resolve_request.abandon([&] { resolving.Cancel(); });
                resolve_request.step(Clock::now(), epoch,
                    resolving.Status() == Windows::Foundation::AsyncStatus::Started, [&] { resolving.Cancel(); });
                if (!resolve_request.active()) resolving = nullptr;
            }
            if (!key || !mouse) {
                invalidate(s, "waiting for verified phone subscriptions");
                { std::lock_guard<std::mutex> lock(s->mutex); epoch = s->epoch; }
                if (!multi_subscriber(epoch, key, mouse, resolved)) return;
            }
        } else if (keys.Size() == 1 && mice.Size() == 1) {
            key = keys.GetAt(0); mouse = mice.GetAt(0);
        }
        if (!key || !mouse) {
            invalidate(s, "need exactly one keyboard and one pointer subscription");
            return;
        }
        auto id = key.Session().DeviceId().Id();
        if (id != mouse.Session().DeviceId().Id()) {
            invalidate(s, "keyboard and pointer subscribers differ");
            return;
        }
        if (resolved || !peer || peer_id != id) {
            if (s->router) {
                std::lock_guard<std::mutex> lock(s->mutex);
                if (epoch != s->epoch) return;
            }
            connection_changed.revoke();
            peer = resolved ? resolved : wait(s, BluetoothLEDevice::FromIdAsync(id), "resolve subscriber", 5s, epoch);
            peer_id = id;
            paired = false;
            pairing_refresh = {};
            pairing_lease.clear();
            if (s->router && resolved) {
                paired = true; // The completed lookup verified this exact paired, online phone.
                pairing_lease.renew(Clock::now(), epoch);
                pairing_retry = Clock::now() + 1s;
            }
            if (peer) {
                auto state = s;
                auto key_session = key.Session(), mouse_session = mouse.Session();
                connection_changed = peer.ConnectionStatusChanged(auto_revoke,
                    [state, key_session, mouse_session](auto const& device, auto const&) {
                        if (device.ConnectionStatus() != BluetoothConnectionStatus::Connected) {
                            {
                                std::lock_guard<std::mutex> lock(state->mutex);
                                state->state_interval = otg_ble_win_detail::fallback_state_interval;
                                state->motion_interval = otg_ble_win_detail::fallback_motion_interval;
                            }
                            log("INFO", "device ConnectionStatus=disconnected; observed sessions keyboard=" +
                                session_text(key_session) + " pointer=" + session_text(mouse_session));
                            invalidate(state, "phone disconnected", true);
                        }
                    });
                try {
                    parameters_changed = peer.ConnectionParametersChanged(auto_revoke,
                        [state](auto const& device, auto const&) {
                            try {
                                set_connection_interval(state, device.GetConnectionParameters().ConnectionInterval());
                            } catch (const hresult_error& e) {
                                log("WARN", "connection parameters: " + describe(e));
                            }
                        });
                    set_connection_interval(s, peer.GetConnectionParameters().ConnectionInterval());
                } catch (const hresult_error& e) {
                    log("INFO", "negotiated BLE interval unavailable; fallback motion=15ms state=30ms: " + describe(e));
                }
            }
        }
        if (!peer) {
            const auto reason = "Windows could not resolve BluetoothLEDevice for subscriber; DeviceId=" + to_string(id);
            invalidate(s, reason.c_str());
            return;
        }
        const auto address = peer.BluetoothAddress();
        uint64_t expected_address;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            expected_address = s->peer_address;
        }
        if (!address || (expected_address && address != expected_address)) {
            const auto reason = "subscriber address mismatch: expected=" + address_text(expected_address) +
                " actual=" + address_text(address) + " DeviceId=" + to_string(id) +
                " paired=" + std::to_string(peer.DeviceInformation().Pairing().IsPaired()) +
                " connected=" + std::to_string(peer.ConnectionStatus() == BluetoothConnectionStatus::Connected);
            invalidate(s, reason.c_str());
            return;
        }
        if (peer.ConnectionStatus() != BluetoothConnectionStatus::Connected) {
            const auto reason = "phone disconnected; sessions keyboard=" + session_text(key.Session()) +
                " pointer=" + session_text(mouse.Session());
            invalidate(s, reason.c_str());
            return;
        }
        if (s->router && !multi_pairing(epoch)) {
            bool was_ready;
            { std::lock_guard<std::mutex> lock(s->mutex); was_ready = s->ready; }
            invalidate(s, "waiting for fresh phone pairing verification");
            if (was_ready) release(); // Only release this phone; metadata delays never fail the IPC endpoint.
            return;
        }
        if (!s->router && Clock::now() >= pairing_refresh) {
            // BluetoothLE# IDs are association endpoints, not necessarily
            // device interfaces (the default CreateFromIdAsync kind).
            auto existing = peer.DeviceInformation();
            auto info = wait(s, Windows::Devices::Enumeration::DeviceInformation::CreateFromIdAsync(
                existing.Id(), nullptr, existing.Kind()), "refresh pairing state", 1500ms, epoch);
            paired = info && info.Pairing().IsPaired();
            pairing_refresh = Clock::now() + 1s;
        }
        if (!paired) {
            invalidate(s, "target phone is not paired");
            return;
        }
        if (peer.ConnectionStatus() != BluetoothConnectionStatus::Connected) {
            invalidate(s, "phone disconnected during pairing check");
            return;
        }
        if (key.MaxNotificationSize() < 8 || mouse.MaxNotificationSize() < 8)
            throw std::runtime_error("negotiated notification size is less than 8 bytes");
        bool became_ready = false, selected = false;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (epoch != s->epoch || s->failed || s->closing || s->suspended) return;
            if (!s->peer_address) {
                s->peer_address = address;
                selected = true;
            }
            key_client = key;
            pointer_client = mouse;
            if (!s->ready) {
                // Release reports precede ALL newly accepted Deskflow input.
                s->queue.clear();
                s->queue.push({1, {}, false, s->epoch});
                s->queue.push({2, {}, false, s->epoch});
                s->ready = true;
                s->waiting.clear();
                became_ready = true;
            }
        }
        if (selected) log("INFO", "auto-selected paired phone " + address_text(address));
        if (became_ready) log("INFO", "target paired, connected and subscribed; input ready");
    }

    void send(const otg_ble_win_detail::Report& report, Clock::time_point& next_notification)
    {
        auto c = report.id == 1 ? keyboard : pointer;
        auto selected = report.id == 1 ? key_client : pointer_client;
        // Refresh snapshots are not authority to broadcast: always recheck
        // this characteristic and the live target immediately before sending.
        auto live = peer ? subscriber(c, peer_id) : GattSubscribedClient{nullptr};
        if (!selected || !peer || peer.ConnectionStatus() != BluetoothConnectionStatus::Connected ||
            !live) {
            invalidate(s, "phone/subscription changed before send", true);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (!s->ready || report.epoch != s->epoch || peer.BluetoothAddress() != s->peer_address) return;
        }
        if (notification) throw std::runtime_error("attempt to overlap notifications");
        notification_started = Clock::now();
        notification = c.NotifyValueAsync(buffer(report.bytes), live);
        GattClientNotificationResult result{nullptr};
        try {
            result = wait(s, notification, report.id == 1 ? "notify keyboard" : "notify pointer", 1500ms, report.epoch);
        } catch (const otg_ble_win_detail::WaitAborted& e) {
            size_t queued;
            { std::lock_guard<std::mutex> lock(s->mutex); queued = s->queue.size(); }
            log("WARN", std::string(e.what()) + " report=" + std::to_string(report.id) +
                " flags/modifiers=" + std::to_string(report.bytes[0]) +
                " buttons/reserved=" + std::to_string(report.bytes[1]) +
                " queue=" + std::to_string(queued) + " sent-keyboard=" + std::to_string(sent_keyboard) +
                " sent-pointer=" + std::to_string(sent_pointer) +
                " device=" + (peer.ConnectionStatus() == BluetoothConnectionStatus::Connected ? "connected" : "disconnected") +
                " keyboard-session=" + session_text(key_client.Session()) +
                " pointer-session=" + session_text(pointer_client.Session()));
            throw;
        }
        notification = nullptr;
        if (result.Status() != GattCommunicationStatus::Success) {
            log("WARN", "notification result=" + std::to_string(static_cast<int>(result.Status())));
            invalidate(s, "notification failed; waiting for phone", true);
            return;
        }
        if (report.id == 1) ++sent_keyboard;
        else ++sent_pointer;
        std::lock_guard<std::mutex> lock(s->mutex);
        if (report.epoch == s->epoch) {
            (report.id == 1 ? s->keyboard : s->pointer) = report.bytes;
            next_notification = otg_ble_win_detail::next_notification_time(report, notification_started,
                Clock::now(), s->motion_interval, s->state_interval);
        }
    }

    // A local overload/failure must not intentionally abandon held inputs on
    // a still-connected phone. This runs only on the worker, after its last
    // notification, and never broadcasts. Link loss can still prevent release.
    void release()
    {
        uint64_t address;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            address = s->peer_address;
        }
        if (s->stopping || !address || !peer || peer.BluetoothAddress() != address ||
            peer.ConnectionStatus() != BluetoothConnectionStatus::Connected || !paired) return;
        if (notification && notification.Status() == Windows::Foundation::AsyncStatus::Started) {
            log("WARN", "final release skipped: previous notification is still in flight");
            return;
        }
        notification = nullptr;
        for (auto c : {keyboard, pointer}) {
            if (!c) continue;
            try {
                auto client = subscriber(c, peer_id);
                if (!client) continue;
                notification = c.NotifyValueAsync(buffer(std::array<uint8_t, 8>{}), client);
                notification_started = Clock::now();
                auto result = wait(s, notification, "final release", 250ms);
                notification = nullptr;
                if (result.Status() != GattCommunicationStatus::Success)
                    log("WARN", "final release could not be delivered");
            } catch (const std::exception& e) {
                log("WARN", std::string("final release failed: ") + e.what());
                if (notification && notification.Status() == Windows::Foundation::AsyncStatus::Started) return;
                notification = nullptr;
            } catch (...) {
                log("WARN", "final release failed (WinRT error; best effort only)");
                return;
            }
        }
    }

    bool recover(const otg_ble_win_detail::WaitAborted& e)
    {
        if (e.reason == otg_ble_win_detail::WaitReason::stopping) return false;
        if (e.reason == otg_ble_win_detail::WaitReason::session_changed ||
            (peer && peer.ConnectionStatus() != BluetoothConnectionStatus::Connected)) {
            invalidate(s, "session ended; waiting for phone reconnect", true);
            pairing_refresh = {};
            log("INFO", std::string(e.what()) + "; retaining GATT service");
            return true;
        }
        return false;
    }

    bool recover_disconnected(const hresult_error& e)
    {
        if (!peer || peer.ConnectionStatus() == BluetoothConnectionStatus::Connected) return false;
        invalidate(s, "session ended; waiting for phone reconnect", true);
        pairing_refresh = {};
        log("INFO", describe(e) + "; phone disconnected, retaining GATT service");
        return true;
    }
};

#else
#include "../../tests/ble_win_multi_service.inc"
#endif

static void worker(const std::shared_ptr<State>& s, const Service *host = nullptr)
{
    try {
        struct Apartment {
            Apartment() { init_apartment(apartment_type::multi_threaded); }
            ~Apartment() { uninit_apartment(); }
        } apartment;
        {
            Service service(s, host);
            try {
                if (!host) service.start();
                auto next_refresh = Clock::now();
                Clock::time_point next_notification{};
                while (!s->stopping) {
                    try {
                        if (Clock::now() >= next_refresh) {
                            service.ensure_advertising();
                            service.refresh();
                            next_refresh = Clock::now() + 100ms;
                        }
                        otg_ble_win_detail::Report report;
                        {
                            std::unique_lock<std::mutex> lock(s->mutex);
                            if (s->failed) break;
                            if (!s->ready || s->queue.empty()) {
                                s->cv.wait_for(lock, 5ms);
                                continue;
                            }
                            if (Clock::now() < next_notification) {
                                s->cv.wait_until(lock, next_notification);
                                continue;
                            }
                            report = s->queue.pop();
                            s->in_flight = true;
                        }
                        service.send(report, next_notification);
                        {
                            std::lock_guard<std::mutex> lock(s->mutex);
                            s->in_flight = false;
                        }
                        s->cv.notify_all();
                    } catch (const otg_ble_win_detail::WaitAborted& e) {
                        if (!service.recover(e)) {
                            // Disable input immediately, BEFORE slow best-effort
                            // releases/provider teardown. Otherwise new presses
                            // can still queue while a fatal send is unwinding.
                            if (!s->stopping) fail(s, e.what());
                            throw;
                        }
                        {
                            std::lock_guard<std::mutex> lock(s->mutex);
                            s->in_flight = false;
                        }
                        s->cv.notify_all();
                        next_refresh = Clock::now() + 100ms;
                    } catch (const hresult_error& e) {
                        if (!service.recover_disconnected(e)) throw;
                        {
                            std::lock_guard<std::mutex> lock(s->mutex);
                            s->in_flight = false;
                        }
                        s->cv.notify_all();
                        next_refresh = Clock::now() + 100ms;
                    }
                }
                service.release();
            } catch (...) {
                service.release();
                throw;
            }
        }
    } catch (const hresult_error& e) {
        if (!s->stopping) fail(s, describe(e));
    } catch (const std::exception& e) {
        if (!s->stopping) fail(s, e.what());
    } catch (...) {
        if (!s->stopping) fail(s, "unexpected worker exception");
    }
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->in_flight = false;
        s->ready = false;
        s->worker_done = true;
    }
    s->cv.notify_all();
}
} // namespace

struct otg_ble_win {
    std::shared_ptr<State> state = std::make_shared<State>();
    std::thread thread;
    unsigned registered = 0;
    std::string pipe_token;
};

static void remote_worker(const std::shared_ptr<State>& s, const std::string& token)
{
    using namespace otg_ble_win_pipe;
    try {
        HANDLE raw = INVALID_HANDLE_VALUE;
        const auto deadline = Clock::now() + 3s;
        do {
            raw = CreateFileA(name(token).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (raw != INVALID_HANDLE_VALUE || s->stopping) break;
            if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND) break;
            std::this_thread::sleep_for(10ms);
        } while (Clock::now() < deadline);
        Handle pipe(raw);
        ULONG pid = 0;
        if (pipe.value == INVALID_HANDLE_VALUE || !GetNamedPipeServerProcessId(pipe.value, &pid) || !same_user(pid))
            throw std::runtime_error("multi-phone service is unavailable or belongs to another user");
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(pipe.value, &mode, nullptr, nullptr))
            throw std::runtime_error("cannot configure multi-phone pipe");
        Packet packet;
        packet.op = hello;
        packet.address = s->peer_address;
        packet.length = static_cast<uint32_t>(s->report_map.size());
        if (packet.length > packet.data.size()) throw std::runtime_error("HID descriptor too large");
        std::copy(s->report_map.begin(), s->report_map.end(), packet.data.begin());
        while (!s->stopping) {
            const auto op = packet.op;
            if (!transfer(pipe.value, packet, true) || !transfer(pipe.value, packet, false) ||
                packet.op != op || packet.flags & ~(Flag::online | Flag::failed | Flag::idle) ||
                packet.flags & Flag::failed)
                throw std::runtime_error("multi-phone session failed or IPC closed");
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                const bool online = (packet.flags & Flag::online) != 0;
                if (s->ready && !online) {
                    ++s->epoch;
                    s->queue.clear();
                }
                s->ready = online;
                if (online) s->peer_address = packet.address;
                s->remote_idle = (packet.flags & Flag::idle) != 0;
                s->in_flight = false;
            }
            s->cv.notify_all();
            packet = Packet{};
            packet.op = Op::poll;
            {
                std::unique_lock<std::mutex> lock(s->mutex);
                if (s->queue.empty() && !s->remote_release) s->cv.wait_for(lock, 5ms);
                if (s->remote_release) {
                    packet.op = Op::release;
                    s->remote_release = false;
                    s->in_flight = true;
                    s->remote_idle = false;
                } else if (!s->queue.empty() && s->ready) {
                    auto next = s->queue.pop();
                    packet.op = Op::report;
                    packet.id = next.id;
                    packet.length = 8;
                    std::copy(next.bytes.begin(), next.bytes.end(), packet.data.begin());
                    s->in_flight = true;
                    s->remote_idle = false;
                }
            }
        }
    } catch (const std::exception& e) { if (!s->stopping) fail(s, e.what()); }
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->ready = false;
        s->worker_done = true;
        s->in_flight = false;
    }
    s->cv.notify_all();
}

static bool register_report(void *ctx, uint16_t id, const uint8_t *desc, uint16_t size)
{
    auto b = static_cast<otg_ble_win *>(ctx);
    try {
        if ((id != 1 && id != 2) || b->registered != static_cast<unsigned>(id - 1) ||
            !desc || size < 7 || (id == 1 && (desc[1] != 1 || desc[3] != 6)) ||
            (id == 2 && (desc[1] != 0x0d || desc[3] != 1)) ||
            !otg_ble_win_detail::append_descriptor(b->state->report_map, desc, size, static_cast<uint8_t>(id)))
            return false;
        ++b->registered;
        if (id == 2) {
            if (b->pipe_token.empty()) b->thread = std::thread(worker, b->state, nullptr);
            else b->thread = std::thread(remote_worker, b->state, b->pipe_token);
        }
        return true;
    } catch (const std::exception& e) { log("ERROR", e.what()); return false; }
}

static bool send_report(void *ctx, uint16_t id, const uint8_t *data, uint16_t size)
{
    auto s = static_cast<otg_ble_win *>(ctx)->state;
    if (!data || size != 8 || (id != 1 && id != 2)) return false;
    bool overflow = false;
    try {
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (!s->ready || s->failed || s->closing) return false;
            otg_ble_win_detail::Report report{id, {}, false, s->epoch};
            std::memcpy(report.bytes.data(), data, 8);
            overflow = !s->queue.push(report);
            if (id == 2 && !overflow) s->last_mouse = report.bytes;
        }
        if (overflow) fail(s, "report queue overflow; closing transport rather than dropping input transitions");
        s->cv.notify_all();
        return !overflow;
    } catch (const std::exception& e) { fail(s, e.what()); return false; }
}

static bool poll_transport(void *ctx)
{
    auto s = static_cast<otg_ble_win *>(ctx)->state;
    std::lock_guard<std::mutex> lock(s->mutex);
    return !s->failed && !s->stopping;
}
static bool ready(void *ctx)
{
    auto s = static_cast<otg_ble_win *>(ctx)->state;
    std::lock_guard<std::mutex> lock(s->mutex);
    return s->ready && !s->failed && !s->closing;
}
static void close_transport(void *ctx)
{
    auto b = static_cast<otg_ble_win *>(ctx);
    if (!b->thread.joinable()) return;
    auto s = b->state;
    {
        std::unique_lock<std::mutex> lock(s->mutex);
        s->closing = true;
        // On exit, discard unsent actions, then release after any in-flight
        // notification. Never send queued presses after the final release.
        if (!b->pipe_token.empty()) {
            s->queue.clear();
            s->remote_release = true;
            s->remote_idle = false;
        } else {
            otg_ble_win_detail::queue_releases(s->queue, s->last_mouse, s->epoch, s->ready && !s->failed);
        }
        s->cv.notify_all();
        s->cv.wait_for(lock, 2s, [&] {
            // Let a failed worker attempt its own releases before cancelling
            // it. Otherwise a queue overflow could race shutdown and skip them.
            return s->worker_done || (!s->failed &&
                 (!s->ready || (s->queue.empty() && !s->in_flight && !s->remote_release && s->remote_idle)));
        });
    }
    s->stopping = true;
    s->cv.notify_all();
    b->thread.join();
}

static const char *identity_transport(void *ctx)
{
    auto s = static_cast<otg_ble_win *>(ctx)->state;
    static thread_local std::string text;
    uint64_t address;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        address = s->ready ? s->peer_address : 0;
    }
    text = address ? address_text(address) : std::string();
    return text.empty() ? nullptr : text.c_str();
}

extern "C" void otg_ble_win_release_inputs(otg_ble_win *b)
{
    auto s = b->state;
    std::lock_guard<std::mutex> lock(s->mutex);
    if (!b->pipe_token.empty()) {
        s->queue.clear();
        s->remote_release = true;
        s->remote_idle = false;
    } else {
        otg_ble_win_detail::queue_releases(s->queue, s->last_mouse, s->epoch,
                                         s->ready && !s->failed && !s->closing);
    }
    s->cv.notify_all();
}

extern "C" bool otg_ble_win_input_idle(otg_ble_win *b)
{
    auto s = b->state;
    std::lock_guard<std::mutex> lock(s->mutex);
    return s->queue.empty() && !s->in_flight && !s->remote_release && s->remote_idle;
}

extern "C" otg_ble_win *otg_ble_win_create(const char *peer)
{
    try {
        if (peer && *peer && std::strlen(peer) != 17) return nullptr;
        uint64_t address = 0;
        if (peer && *peer) {
            for (unsigned i = 0; i < 17; ++i) {
                if (i % 3 == 2) { if (peer[i] != ':') return nullptr; continue; }
                char c = peer[i];
                int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                if (digit < 0) return nullptr;
                address = (address << 4) | static_cast<unsigned>(digit);
            }
            if (!address) return nullptr;
        }
        auto b = std::make_unique<otg_ble_win>();
        b->state->peer_address = address;
        return b.release();
    } catch (...) { return nullptr; }
}
extern "C" const otg_hid_transport_ops *otg_ble_win_ops(void)
{
    static const otg_hid_transport_ops ops = {register_report, send_report, poll_transport, ready, close_transport, identity_transport};
    return &ops;
}

extern "C" otg_ble_win *otg_ble_win_create_session(const char *peer, const char *token)
{
    if (!token || !otg_ble_win_pipe::valid_token(token)) return nullptr;
    auto b = otg_ble_win_create(peer);
    if (b) b->pipe_token = token;
    return b;
}

#include "ble_win_multi.inc"
extern "C" const char *otg_ble_win_identity(otg_ble_win *b)
{
    return b ? identity_transport(b) : nullptr;
}
extern "C" void otg_ble_win_destroy(otg_ble_win *b)
{
    if (b) { close_transport(b); delete b; }
}
