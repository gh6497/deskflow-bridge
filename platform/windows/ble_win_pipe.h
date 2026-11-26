/* Private Windows BLE multi-session protocol. No HID input is broadcast.
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <windows.h>
#include <sddl.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace otg_ble_win_pipe {
constexpr uint32_t magic = 0x57424d31;
enum Op : uint32_t { hello = 1, poll = 2, report = 3, release = 4 };
enum Flag : uint32_t { online = 1, failed = 2, idle = 4 };
struct Packet {
    uint32_t tag = magic, version = 1, op = 0, flags = 0;
    uint64_t address = 0;
    uint32_t id = 0, length = 0;
    std::array<uint8_t, 512> data{};
};
static_assert(sizeof(Packet) == 544);
inline bool valid_token(const std::string& token)
{
    if (token.size() != 32) return false;
    for (char c : token) if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
    return true;
}
inline std::string name(const std::string& token) { return "\\\\.\\pipe\\deskflow-bridge-ble-" + token; }
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : value(v) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct PendingConnect {
    HANDLE pipe;
    OVERLAPPED operation{};
    bool pending = false;
    explicit PendingConnect(HANDLE handle, HANDLE event) : pipe(handle) { operation.hEvent = event; }
    ~PendingConnect()
    {
        if (pending) {
            CancelIoEx(pipe, &operation);
            DWORD ignored;
            GetOverlappedResult(pipe, &operation, &ignored, TRUE);
        }
    }
};
inline std::vector<uint8_t> user_sid(HANDLE process)
{
    HANDLE raw;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) return {};
    Handle token(raw);
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<uint8_t> info(size);
    if (!size || !GetTokenInformation(token.value, TokenUser, info.data(), size, &size)) return {};
    auto sid = reinterpret_cast<TOKEN_USER *>(info.data())->User.Sid;
    std::vector<uint8_t> copy(GetLengthSid(sid));
    if (!CopySid(static_cast<DWORD>(copy.size()), copy.data(), sid)) return {};
    return copy;
}
inline bool same_user(ULONG pid)
{
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (process.value == nullptr) return false;
    auto actual = user_sid(process.value), expected = user_sid(GetCurrentProcess());
    return !actual.empty() && !expected.empty() && EqualSid(actual.data(), expected.data());
}
struct Security {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    Security()
    {
        auto sid = user_sid(GetCurrentProcess());
        LPSTR text = nullptr;
        if (sid.empty() || !ConvertSidToStringSidA(sid.data(), &text)) return;
        const auto sddl = std::string("D:P(A;;GA;;;") + text + ")";
        LocalFree(text);
        if (ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl.c_str(), SDDL_REVISION_1,
                &descriptor, nullptr)) attributes.lpSecurityDescriptor = descriptor;
    }
    ~Security() { if (descriptor) LocalFree(descriptor); }
};
inline bool transfer(HANDLE pipe, Packet& packet, bool writing, DWORD timeout = 2000)
{
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.value) return false;
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    DWORD count = 0;
    BOOL done = writing ? WriteFile(pipe, &packet, sizeof(packet), &count, &operation) :
                          ReadFile(pipe, &packet, sizeof(packet), &count, &operation);
    if (!done) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(event.value, timeout) != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &operation);
            GetOverlappedResult(pipe, &operation, &count, TRUE);
            return false;
        }
        if (!GetOverlappedResult(pipe, &operation, &count, FALSE)) return false;
    }
    return count == sizeof(packet) && packet.tag == magic && packet.version == 1 &&
           packet.length <= packet.data.size();
}
inline bool valid_request(const Packet& p, bool initial)
{
    if (p.tag != magic || p.version != 1 || p.flags || p.address > 0xffffffffffffULL) return false;
    if (initial) return p.op == hello && !p.id && p.length > 0 && p.length <= p.data.size();
    if (p.address) return false;
    return ((p.op == poll || p.op == release) && !p.id && !p.length) ||
           (p.op == report && (p.id == 1 || p.id == 2) && p.length == 8);
}
} // namespace otg_ble_win_pipe
