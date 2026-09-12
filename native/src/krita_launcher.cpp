#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace {
std::string utf8(const wchar_t* value) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (!size) return {};
    std::string result(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), size, nullptr, nullptr)) return {};
    return result;
}

std::wstring modulePath(HMODULE module) {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, 32768);
    return length && length < 32768 ? std::wstring(path, length) : std::wstring{};
}

int fail(int code, const std::wstring& reason, bool checkOnly, DWORD error = 0) {
    auto message = reason;
    if (error) message += L"\nWindows error: " + std::to_wstring(error);
    std::fprintf(stderr, "ChapArm Krita launcher: %s\n", utf8(message.c_str()).c_str());
    if (!checkOnly) MessageBoxW(nullptr, message.c_str(), L"ChapArm Krita launcher", MB_OK | MB_ICONERROR);
    return code;
}

bool isFile(const std::wstring& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace

int main(int argc, char** argv) {
    const bool checkOnly = argc == 2 && std::strcmp(argv[1], "--chaparm-check") == 0;
    try {
        const auto executable = modulePath(nullptr);
        if (executable.empty()) return fail(2, L"Cannot locate the launcher executable.", checkOnly, GetLastError());
        const auto directory = executable.substr(0, executable.find_last_of(L"\\/") + 1);
        const auto marker = executable + L".local";
        if (!isFile(marker))
            return fail(2, L"Missing application-local redirection marker:\n" + marker +
                L"\nDeploy with scripts/deploy-krita.ps1; the marker must be a file, not a directory.", checkOnly);
        const auto providerPath = directory + L"Wintab32.dll";
        if (!isFile(providerPath)) return fail(2, L"Missing application-local ChapArm provider:\n" + providerPath, checkOnly);
        const auto kritaPath = directory + L"krita.dll";
        if (!isFile(kritaPath)) return fail(2, L"Missing portable Krita library:\n" + kritaPath, checkOnly);

        // Qt can request an absolute system path. Test that exact loader route
        // before running Krita, and refuse to use an installed tablet driver.
        wchar_t systemDirectory[32768]{};
        const auto length = GetSystemDirectoryW(systemDirectory, 32768);
        if (!length || length >= 32768) return fail(3, L"Cannot locate the Windows system directory.", checkOnly, GetLastError());
        const auto systemProvider = std::wstring(systemDirectory, length) + L"\\Wintab32.dll";
        const auto provider = LoadLibraryW(systemProvider.c_str());
        if (!provider) return fail(3, L"Windows did not redirect Wintab32.dll to:\n" + providerPath, checkOnly, GetLastError());
        if (_wcsicmp(modulePath(provider).c_str(), providerPath.c_str()) ||
            !GetProcAddress(provider, "ChapArmGetStatus"))
            return fail(3, L"Loaded Wintab32.dll is not the selected ChapArm provider:\n" + modulePath(provider), checkOnly);

        const auto library = LoadLibraryW(kritaPath.c_str());
        if (!library) return fail(4, L"Cannot load:\n" + kritaPath +
            L"\nCheck that the portable files are complete and match the launcher's architecture.", checkOnly, GetLastError());
        const auto entry = reinterpret_cast<int(__cdecl*)(int, char**)>(GetProcAddress(library, "krita_main"));
        if (!entry) return fail(5, L"The selected krita.dll does not export krita_main:\n" + kritaPath, checkOnly);
        if (checkOnly) {
            std::fputs("PASS: application-local Wintab redirection and Krita entry point\n", stdout);
            return 0;
        }

        // Match Krita's original stub: unchanged narrow CRT arguments let Qt
        // recognize the original command line and recover Unicode with
        // GetCommandLineW. Rebuilding argv (even as UTF-8) disables that path.
        // Krita owns application initialization. Keep both DLLs loaded until
        // process exit, including any Qt/provider background-thread teardown.
        return entry(argc, argv);
    } catch (...) {
        return fail(1, L"The launcher could not initialize the selected portable Krita.", checkOnly);
    }
}
