#include <windows.h>
#ifdef CHAPARM_KRITA_FIXTURE
#include <shellapi.h>
#include <cstdlib>
#include <cstring>
#include <cwchar>
extern "C" int __cdecl krita_main(int argc, char** argv) {
    // Qt only restores the Unicode Windows command line when argc/argv match
    // the narrow CRT arguments. Test that contract, including characters that
    // cannot be represented by common ANSI code pages such as 936 or 1252.
    if (argc != 4 || argc != __argc || argv != __argv || argv[argc] ||
        std::strcmp(argv[1], "--probe-entry") || std::strcmp(argv[2], "space separated")) return 38;
    int wideCount = 0;
    const auto wide = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    const bool preserved = wide && wideCount == argc &&
        !std::wcscmp(wide[2], L"space separated") && !std::wcscmp(wide[3], L"\u7ed8\u753b \U0001f3a8");
    if (wide) LocalFree(wide);
    return preserved ? 37 : 38;
}
#else
#include "wintab.h"
#include "chaparm_bridge.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

// Krita 5.2 requests this format, without the pointer field tested below.
#define PACKETNAME KRITA
#define KRITAPACKETDATA (PK_TIME | PK_CURSOR | PK_BUTTONS | PK_X | PK_Y | PK_Z | PK_NORMAL_PRESSURE | PK_TANGENT_PRESSURE | PK_ORIENTATION)
#define KRITAPACKETMODE 0
#include "pktdef.h"

// This includes a pointer field and intentional trailing padding on x64.
// Arrays expose ABI stride errors that a single packet would conceal.
#define PACKETDATA (PK_CONTEXT | PK_STATUS | PK_TIME | PK_SERIAL_NUMBER | PK_CURSOR | PK_BUTTONS | PK_X | PK_Y | PK_NORMAL_PRESSURE | PK_ORIENTATION)
#define PACKETMODE 0
#include "pktdef.h"

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

namespace {
BOOL CALLBACK findManifest(HMODULE, LPCWSTR, LPWSTR, LONG_PTR result) {
    *reinterpret_cast<bool*>(result) = true;
    return FALSE;
}

DWORD runLauncher(const std::wstring& path, const std::wstring& arguments = L"--chaparm-check") {
    auto command = L"\"" + path + L"\" " + arguments;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    CHECK(CreatePipe(&read, &write, &security, 0));
    CHECK(SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0));
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES; startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = startup.hStdError = write;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION child{};
    const bool started = CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &child) != FALSE;
    CloseHandle(write);
    if (!started) { CloseHandle(read); CHECK(started); }
    const auto wait = WaitForSingleObject(child.hProcess, 5000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 1);
        WaitForSingleObject(child.hProcess, 1000);
    }
    DWORD result = 1;
    const bool exited = GetExitCodeProcess(child.hProcess, &result) != FALSE;
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    std::string output;
    char buffer[1024]; DWORD bytes = 0;
    while (ReadFile(read, buffer, sizeof(buffer), &bytes, nullptr) && bytes) output.append(buffer, bytes);
    CloseHandle(read);
    if (result && result != 37) std::fprintf(stderr, "launcher exit %lu: %s\n", result, output.c_str());
    CHECK(wait == WAIT_OBJECT_0 && exited);
    return result;
}

void launcherTest() {
    wchar_t executable[32768]{};
    CHECK(GetModuleFileNameW(nullptr, executable, 32768));
    std::wstring directory(executable);
    directory.resize(directory.find_last_of(L"\\/") + 1);
    const auto originalLauncher = directory + L"chaparm-krita.exe";
    const auto image = LoadLibraryExW(originalLauncher.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    CHECK(image);
    bool hasManifest = false;
    SetLastError(ERROR_SUCCESS);
    EnumResourceNamesW(image, MAKEINTRESOURCEW(24), findManifest, reinterpret_cast<LONG_PTR>(&hasManifest));
    const auto resourceError = GetLastError();
    FreeLibrary(image);
    CHECK(!hasManifest);
    CHECK(resourceError == ERROR_RESOURCE_TYPE_NOT_FOUND || resourceError == ERROR_RESOURCE_DATA_NOT_FOUND);

    // A unique directory under the build output keeps all copies disposable.
    // Cleanup removes only the exact files created here, never recursively.
    const auto temporary = directory + L"launcher probe " + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    CHECK(CreateDirectoryW(temporary.c_str(), nullptr));
    const auto launcher = temporary + L"\\chaparm-krita.exe";
    const auto local = launcher + L".local";
    const auto provider = temporary + L"\\Wintab32.dll";
    const auto krita = temporary + L"\\krita.dll";
    struct Cleanup {
        std::wstring directory, launcher, local, provider, krita;
        ~Cleanup() {
            DeleteFileW(krita.c_str()); DeleteFileW(provider.c_str());
            DeleteFileW(local.c_str()); RemoveDirectoryW(local.c_str());
            DeleteFileW(launcher.c_str()); RemoveDirectoryW(directory.c_str());
        }
    } cleanup{temporary, launcher, local, provider, krita};
    CHECK(CopyFileW(originalLauncher.c_str(), launcher.c_str(), TRUE));
    const auto marker = CreateFileW(local.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(marker != INVALID_HANDLE_VALUE); CloseHandle(marker);
    CHECK(CopyFileW((directory + L"Wintab32.dll").c_str(), provider.c_str(), TRUE));
    CHECK(CopyFileW((directory + L"krita_launcher_fixture.dll").c_str(), krita.c_str(), TRUE));
    // Deploy the marker before the first launch, as an actual install does.
    // Windows can retain redirection state for an executable first run without it.
    CHECK(runLauncher(launcher) == 0); // real full-path Windows loader redirection
    CHECK(runLauncher(launcher, L"--probe-entry \"space separated\" \"\u7ed8\u753b \U0001f3a8\"") == 37);
    CHECK(CopyFileW((directory + L"Wintab32.dll").c_str(), krita.c_str(), FALSE));
    CHECK(runLauncher(launcher) == 5); // valid DLL without krita_main
    CHECK(DeleteFileW(krita.c_str()));
    CHECK(runLauncher(launcher) == 2); // missing adjacent Krita library
    CHECK(CopyFileW((directory + L"krita_launcher_fixture.dll").c_str(), krita.c_str(), TRUE));
    CHECK(CopyFileW((directory + L"krita_launcher_fixture.dll").c_str(), provider.c_str(), FALSE));
    CHECK(runLauncher(launcher) == 3); // correctly located but not a ChapArm provider
    CHECK(DeleteFileW(provider.c_str()));
    CHECK(runLauncher(launcher) == 2); // missing adjacent provider
    CHECK(DeleteFileW(local.c_str()));
    CHECK(runLauncher(launcher) == 2); // missing redirection marker
    CHECK(CreateDirectoryW(local.c_str(), nullptr));
    CHECK(runLauncher(launcher) == 2); // require the validated file form
    std::puts("PASS: launcher manifest, isolated full-path DLL loading, entry errors, original CRT and Unicode arguments");
}

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
    // Direct calls below exercise the import library (including x86 stdcall).
    // Dynamic clients must also find every public name and official ordinal.
    const struct { const char* name; WORD ordinal; } exports[]{
        {"WTInfoA", 20}, {"WTInfoW", 1020}, {"WTOpenA", 21}, {"WTOpenW", 1021},
        {"WTClose", 22}, {"WTPacketsGet", 23}, {"WTPacket", 24},
        {"WTEnable", 40}, {"WTOverlap", 41}, {"WTConfig", 60},
        {"WTGetA", 61}, {"WTGetW", 1061}, {"WTSetA", 62}, {"WTSetW", 1062},
        {"WTExtGet", 63}, {"WTExtSet", 64}, {"WTSave", 65}, {"WTRestore", 66},
        {"WTPacketsPeek", 80}, {"WTDataGet", 81}, {"WTDataPeek", 82},
        {"WTQueueSizeGet", 84}, {"WTQueueSizeSet", 85}, {"WTQueuePacketsEx", 200},
        {"WTMgrOpen", 100}, {"WTMgrClose", 101}, {"WTMgrContextEnum", 120},
        {"WTMgrContextOwner", 121}, {"WTMgrDefContext", 122}, {"WTMgrDefContextEx", 206},
        {"WTMgrDeviceConfig", 140}, {"WTMgrExt", 180}, {"WTMgrCsrEnable", 181},
        {"WTMgrCsrButtonMap", 182}, {"WTMgrCsrPressureBtnMarks", 183},
        {"WTMgrCsrPressureResponse", 184}, {"WTMgrCsrExt", 185},
        {"WTMgrConfigReplaceExA", 202}, {"WTMgrConfigReplaceExW", 1202},
        {"WTMgrPacketHookExA", 203}, {"WTMgrPacketHookExW", 1203},
        {"WTMgrPacketUnhook", 204}, {"WTMgrPacketHookNext", 205},
        {"WTMgrCsrPressureBtnMarksEx", 201},
        {"ChapArmOpenPublisher", 3000}, {"ChapArmPublish", 3001},
        {"ChapArmClosePublisher", 3002}, {"ChapArmGetStatus", 3003}
    };
    for (const auto& entry : exports) {
        const auto address = GetProcAddress(module, entry.name);
        CHECK(address);
        CHECK(GetProcAddress(module, MAKEINTRESOURCEA(entry.ordinal)) == address);
    }
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
    UINT cursorCount = 0, firstCursor = 0, deviceCursorCount = 0;
    CHECK(WTInfoW(WTI_INTERFACE, IFC_NCURSORS, &cursorCount) == sizeof(cursorCount));
    CHECK(WTInfoW(WTI_DEVICES, DVC_FIRSTCSR, &firstCursor) == sizeof(firstCursor));
    CHECK(WTInfoW(WTI_DEVICES, DVC_NCSRTYPES, &deviceCursorCount) == sizeof(deviceCursorCount));
    CHECK(cursorCount == 2 && firstCursor == 0 && deviceCursorCount == cursorCount);
    UINT penCursor = cursorCount, activeCursors = 0;
    for (UINT cursor = firstCursor; cursor < firstCursor + deviceCursorCount; ++cursor) {
        BOOL active = FALSE;
        CHECK(WTInfoW(WTI_CURSORS + cursor, CSR_NAME, nullptr) > 0);
        CHECK(WTInfoW(WTI_CURSORS + cursor, CSR_ACTIVE, &active) == sizeof(active));
        if (active) { penCursor = cursor; ++activeCursors; }
    }
    // Krita uses cursor ID modulo three, in addition to CSR_TYPE, to decide
    // whether it may pass pressure through. ID zero would silently lose it.
    CHECK(activeCursors == 1 && penCursor % 3 == 1);
    UINT cursorType = 0;
    CHECK(WTInfoW(WTI_CURSORS + penCursor, CSR_TYPE, &cursorType) == sizeof(cursorType));
    CHECK(cursorType == 0x0802);
    CHECK(WTInfoW(WTI_CURSORS + cursorCount, CSR_NAME, nullptr) == 0);

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
    CHECK(packets[0].pkCursor == penCursor && packets[1].pkCursor == penCursor);
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

    // A first contact must survive Krita consuming one hover packet to identify
    // the pen on proximity entry. Also verify its 44-byte packet array stride.
    CHECK(sizeof(KRITAPACKET) == 44);
    lc.lcPktData = lc.lcMoveMask = KRITAPACKETDATA;
    lc.lcPktMode = KRITAPACKETMODE;
    const auto kritaContext = WTOpenW(nullptr, &lc, TRUE); CHECK(kritaContext);
    publisher = ChapArmOpenPublisher(session.c_str()); CHECK(publisher);
    s = sample(.2f, .3f, .5f); CHECK(ChapArmPublish(publisher, &s));
    std::array<KRITAPACKET, 2> kritaPackets{};
    until([&] { return WTPacketsPeek(kritaContext, 2, kritaPackets.data()) == 2; });
    CHECK(kritaPackets[0].pkCursor == penCursor && kritaPackets[0].pkNormalPressure == 0);
    CHECK(kritaPackets[1].pkCursor == penCursor && kritaPackets[1].pkButtons == 1);
    CHECK(kritaPackets[1].pkNormalPressure == static_cast<UINT>(std::lround(.5f * 8191)));
    CHECK(kritaPackets[1].pkOrientation.orAltitude == 700);
    CHECK(WTPacketsGet(kritaContext, 1, kritaPackets.data()) == 1);
    CHECK(WTPacketsGet(kritaContext, 1, kritaPackets.data()) == 1);
    CHECK(kritaPackets[0].pkCursor == penCursor && kritaPackets[0].pkNormalPressure > 0);
    CHECK(ChapArmClosePublisher(publisher));
    CHECK(WTClose(kritaContext));
    FreeLibrary(module);
    std::puts("PASS: x86/x64 ABI, ANSI/Unicode, scaling, pressure, orientation, queues, contexts, IPC, watchdog release");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--writer") return writer(argv[2]);
        if (argc == 2 && std::wstring(argv[1]) == L"--selftest") { selftest(); launcherTest(); return 0; }
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
#endif
