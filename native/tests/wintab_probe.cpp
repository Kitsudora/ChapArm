#include <windows.h>
#include "wintab.h"
#include "chaparm_bridge.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

// This includes a pointer field and intentional trailing padding on x64.
// Arrays expose ABI stride errors that a single packet would conceal.
#define PACKETDATA (PK_CONTEXT | PK_STATUS | PK_TIME | PK_SERIAL_NUMBER | PK_CURSOR | PK_BUTTONS | PK_X | PK_Y | PK_NORMAL_PRESSURE | PK_ORIENTATION)
#define PACKETMODE 0
#include "pktdef.h"

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

namespace {
void pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
template<class Predicate> void until(Predicate predicate, DWORD timeout = 1500) {
    const auto end = GetTickCount64() + timeout;
    while (!predicate()) {
        CHECK(GetTickCount64() < end);
        pump(); Sleep(4);
    }
}
ChapArmSample sample(float x = .25f, float y = .75f, float pressure = 0) {
    return {sizeof(ChapArmSample), 1, x, y, pressure, 120, 70, 30,
        pressure > 0 ? 1u : 0u, 1, 0};
}

int writer(const wchar_t* session) {
    const auto handle = ChapArmOpenPublisher(session);
    CHECK(handle);
    auto s = sample(.4f, .6f, .5f);
    CHECK(ChapArmPublish(handle, &s));
    // The reader must release the pressed pen even though this process lives.
    Sleep(850);
    CHECK(ChapArmClosePublisher(handle));
    return 0;
}

void selftest() {
    const auto session = L"probe_" + std::to_wstring(GetCurrentProcessId());
    CHECK(SetEnvironmentVariableW(L"CHAPARM_SESSION", session.c_str()));
    const auto module = LoadLibraryW(L"Wintab32.dll");
    CHECK(module);
    for (const auto name : {"WTInfoA", "WTInfoW", "WTOpenA", "WTOpenW", "WTClose",
        "WTPacket", "WTPacketsGet", "WTPacketsPeek", "WTGetW", "WTSetW", "WTQueuePacketsEx",
        "WTEnable", "WTOverlap", "ChapArmPublish", "ChapArmGetStatus"}) CHECK(GetProcAddress(module, name));
    CHECK(GetProcAddress(module, MAKEINTRESOURCEA(1020)) == GetProcAddress(module, "WTInfoW"));
    CHECK(sizeof(LOGCONTEXTA) == 172);
    CHECK(sizeof(LOGCONTEXTW) == 212);
    CHECK(sizeof(ChapArmSample) == 48);

    WORD version = 0;
    CHECK(WTInfoW(WTI_INTERFACE, IFC_SPECVERSION, &version) == sizeof(version));
    CHECK(version == 0x0104);
    AXIS pressure{};
    CHECK(WTInfoW(WTI_DEVICES, DVC_NPRESSURE, &pressure) == sizeof(AXIS));
    CHECK(pressure.axMax == 8191);
    std::array<AXIS, 3> orientation{};
    CHECK(WTInfoW(WTI_DEVICES, DVC_ORIENTATION, orientation.data()) == sizeof(orientation));
    CHECK(orientation[0].axResolution != 0);
    CHECK(WTInfoW(WTI_DEVICES + 1, DVC_NAME, nullptr) == 0);
    CHECK(WTInfoW(WTI_EXTENSIONS, 0, nullptr) == 0);

    // Context notifications must be sent even without CXO_MESSAGES.
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"ChapArmProbeWindow";
    CHECK(RegisterClassW(&windowClass));
    const auto window = CreateWindowW(windowClass.lpszClassName, L"ChapArm probe", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance, nullptr);
    CHECK(window);
    LOGCONTEXTW messageContext{};
    CHECK(WTInfoW(WTI_DEFCONTEXT, 0, &messageContext));
    messageContext.lcOptions = 0;
    messageContext.lcMsgBase = 0x6000;
    const auto notificationContext = WTOpenW(window, &messageContext, FALSE);
    CHECK(notificationContext);
    MSG message{};
    CHECK(PeekMessageW(&message, window, 0x6001, 0x6001, PM_REMOVE));
    CHECK(message.wParam == reinterpret_cast<WPARAM>(notificationContext));
    CHECK(WTSetW(notificationContext, &messageContext));
    CHECK(PeekMessageW(&message, window, 0x6003, 0x6003, PM_REMOVE));
    CHECK(WTClose(notificationContext));
    CHECK(PeekMessageW(&message, window, 0x6002, 0x6002, PM_REMOVE));
    CHECK(DestroyWindow(window));

    LOGCONTEXTW lc{};
    CHECK(WTInfoW(WTI_DEFSYSCTX, 0, &lc) == sizeof(lc));
    lc.lcPktData = lc.lcMoveMask = PACKETDATA;
    lc.lcPktMode = PACKETMODE;
    lc.lcOutOrgX = lc.lcOutOrgY = 0;
    lc.lcOutExtX = 65535; lc.lcOutExtY = -65535;
    const auto context = WTOpenW(nullptr, &lc, TRUE);
    CHECK(context);
    CHECK(lc.lcStatus == CXS_ONTOP);
    CHECK(WTQueueSizeSet(context, 64));
    CHECK(WTQueueSizeGet(context) == 64);
    CHECK(!WTQueueSizeSet(context, 5000));

    LOGCONTEXTA ansi{};
    CHECK(WTGetA(context, &ansi));
    CHECK(ansi.lcPktData == PACKETDATA);
    CHECK(WTSetA(context, &ansi));
    auto invalid = lc; invalid.lcInExtX = 0;
    CHECK(!WTSetW(context, &invalid));
    LOGCONTEXTW unchanged{};
    CHECK(WTGetW(context, &unchanged)); CHECK(unchanged.lcInExtX == 65535);

    auto publisher = ChapArmOpenPublisher(session.c_str());
    CHECK(publisher);
    CHECK(!ChapArmOpenPublisher(session.c_str()));
    auto broken = sample(); broken.pressure = std::numeric_limits<float>::quiet_NaN();
    CHECK(!ChapArmPublish(publisher, &broken));
    auto s = sample(); CHECK(ChapArmPublish(publisher, &s));
    s.pressure = .4f; s.buttons = 1; CHECK(ChapArmPublish(publisher, &s));
    s.x = .5f; s.pressure = .8f; CHECK(ChapArmPublish(publisher, &s));
    s.pressure = 0; s.buttons = 0; CHECK(ChapArmPublish(publisher, &s));
    std::array<PACKET, 64> packets{};
    until([&] { return WTPacketsPeek(context, 64, packets.data()) >= 4; });
    const auto count = WTPacketsPeek(context, 64, packets.data());
    CHECK(count == 4);
    CHECK(packets[0].pkContext == context);
    CHECK(std::abs(packets[0].pkX - 16384) <= 1);
    CHECK(std::abs(packets[0].pkY - 49151) <= 1);
    CHECK(packets[1].pkNormalPressure == static_cast<UINT>(std::lround(.4f * 8191)));
    CHECK(packets[1].pkButtons == 1);
    CHECK(packets[2].pkOrientation.orAzimuth == 1200);
    CHECK(packets[2].pkOrientation.orAltitude == 700);
    CHECK(packets[2].pkOrientation.orTwist == 300);
    CHECK(packets[3].pkNormalPressure == 0 && packets[3].pkButtons == 0);
    CHECK((packets[0].pkStatus & TPS_PROXIMITY) == 0);
    UINT first = 0, last = 0;
    CHECK(WTQueuePacketsEx(context, &first, &last));
    CHECK(first == packets[0].pkSerialNumber && last == packets[3].pkSerialNumber);
    PACKET single{};
    CHECK(WTPacket(context, packets[1].pkSerialNumber, &single));
    CHECK(single.pkNormalPressure == packets[1].pkNormalPressure);
    CHECK(WTPacketsPeek(context, 64, packets.data()) == 2);
    int copied = 0;
    CHECK(WTDataPeek(context, first, last, 1, &single, &copied) == 2 && copied == 1);
    CHECK(WTDataGet(context, first, last, 1, &single, &copied) == 2 && copied == 1);
    CHECK(WTPacketsGet(context, 64, nullptr) == 1);
    CHECK(!WTQueuePacketsEx(context, &first, &last));

    ChapArmStatus status{}; status.size = sizeof(status); status.version = 1;
    until([&] { return ChapArmGetStatus(session.c_str(), &status) && status.enabled_context_count == 1; });
    CHECK(status.consumer_pid == GetCurrentProcessId());
    CHECK(status.publisher_pid == GetCurrentProcessId());
    CHECK(status.published_samples == 4);

    // Relative output, and an ANSI context participating in overlap order.
    auto lc2 = ansi; lc2.lcPktMode = PK_X | PK_Y | PK_NORMAL_PRESSURE;
    const auto second = WTOpenA(nullptr, &lc2, TRUE); CHECK(second);
    CHECK(WTOverlap(second, FALSE));
    CHECK(WTGetA(second, &lc2)); CHECK(lc2.lcStatus == CXS_OBSCURED);
    CHECK(WTEnable(context, FALSE));
    CHECK(WTGetA(second, &lc2)); CHECK(lc2.lcStatus == CXS_ONTOP);
    s = sample(.2f, .3f, .5f); CHECK(ChapArmPublish(publisher, &s));
    until([&] { return WTPacketsPeek(second, 64, packets.data()) >= 2; });
    WTPacketsGet(second, 64, nullptr);
    s.x = .3f; CHECK(ChapArmPublish(publisher, &s));
    until([&] { return WTPacketsGet(second, 1, &single) == 1; });
    CHECK(std::abs(single.pkX - 6554) <= 1);
    CHECK(single.pkY == 0 && single.pkNormalPressure == 0);
    CHECK(WTClose(second));
    CHECK(!WTGetW(second, &unchanged));
    CHECK(WTEnable(context, TRUE));
    s = sample(.5f, .5f, .6f); CHECK(ChapArmPublish(publisher, &s));
    until([&] { return WTPacketsPeek(context, 64, packets.data()) >= 2; });
    CHECK(ChapArmClosePublisher(publisher));
    bool released = false;
    until([&] {
        const int n = WTPacketsGet(context, 64, packets.data());
        for (int i = 0; i < n; ++i) if (packets[i].pkStatus & TPS_PROXIMITY)
            released = packets[i].pkNormalPressure == 0 && packets[i].pkButtons == 0;
        return released;
    });

    // A separate process publishes through the named IPC ring. Its missing
    // heartbeat must create a release before it closes the producer handle.
    wchar_t executable[32768]{};
    CHECK(GetModuleFileNameW(nullptr, executable, 32768));
    auto command = L"\"" + std::wstring(executable) + L"\" --writer " + session;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    CHECK(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &child));
    bool down = false; released = false;
    until([&] {
        const int n = WTPacketsGet(context, 64, packets.data());
        for (int i = 0; i < n; ++i) {
            down = down || packets[i].pkNormalPressure > 0;
            if (down && (packets[i].pkStatus & TPS_PROXIMITY)) released = true;
        }
        return released;
    });
    CHECK(down);
    CHECK(WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(child.hProcess, 2000) == WAIT_OBJECT_0);
    DWORD exit = 1; CHECK(GetExitCodeProcess(child.hProcess, &exit)); CHECK(exit == 0);
    CloseHandle(child.hThread); CloseHandle(child.hProcess);

    // A bounded queue retains the newest release and marks lost data.
    WTPacketsGet(context, 64, nullptr);
    CHECK(WTQueueSizeSet(context, 2));
    publisher = ChapArmOpenPublisher(session.c_str()); CHECK(publisher);
    s = sample(); CHECK(ChapArmPublish(publisher, &s));
    s.pressure = .5f; s.buttons = 1; CHECK(ChapArmPublish(publisher, &s));
    s.pressure = 0; s.buttons = 0; CHECK(ChapArmPublish(publisher, &s));
    until([&] {
        const int n = WTPacketsPeek(context, 64, packets.data());
        return n == 2 && (packets[1].pkStatus & TPS_QUEUE_ERR) && packets[1].pkButtons == 0;
    });
    CHECK(packets[0].pkButtons == 1 && packets[1].pkNormalPressure == 0);
    CHECK(ChapArmClosePublisher(publisher));
    CHECK(WTClose(context));
    CHECK(!WTClose(context));
    FreeLibrary(module);
    std::puts("PASS: x86/x64 ABI, ANSI/Unicode, scaling, pressure, orientation, queues, contexts, IPC, watchdog release");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--writer") return writer(argv[2]);
        if (argc == 2 && std::wstring(argv[1]) == L"--selftest") { selftest(); return 0; }
        ChapArmStatus status{}; status.size = sizeof(status); status.version = 1;
        CHECK(ChapArmGetStatus(nullptr, &status));
        std::printf("publisher_pid=%lu consumer_pid=%lu contexts=%lu enabled=%lu samples=%llu\n",
            static_cast<unsigned long>(status.publisher_pid), static_cast<unsigned long>(status.consumer_pid),
            static_cast<unsigned long>(status.context_count), static_cast<unsigned long>(status.enabled_context_count),
            static_cast<unsigned long long>(status.published_samples));
        std::puts("Use wintab_probe --selftest for the isolated native integration check.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s (Windows error %lu)\n", error.what(), GetLastError());
        return 1;
    }
}
