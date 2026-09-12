#include <windows.h>
#include "wintab.h"
#include "chaparm_bridge.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
constexpr DWORD kMagic = 0x43484131;
constexpr std::uint64_t kStaleMs = 500;
constexpr std::size_t kRingSize = 2048;
constexpr LONG kAxisMax = 65535;
constexpr UINT kPressureMax = 8191;
// Qt/Krita classifies cursor IDs modulo three: 0=cursor, 1=pen, 2=eraser.
// Keep slot zero queryable but inactive, so cursor enumeration stays contiguous.
constexpr UINT kPenCursor = 1;
constexpr UINT kCursorCount = 2;
constexpr WTPKT kPacketData = PK_CONTEXT | PK_STATUS | PK_TIME | PK_CHANGED |
    PK_SERIAL_NUMBER | PK_CURSOR | PK_BUTTONS | PK_X | PK_Y | PK_Z |
    PK_NORMAL_PRESSURE | PK_TANGENT_PRESSURE | PK_ORIENTATION;
constexpr WTPKT kRelativeData = PK_BUTTONS | PK_X | PK_Y | PK_Z |
    PK_NORMAL_PRESSURE | PK_TANGENT_PRESSURE | PK_ORIENTATION;

struct Consumer { DWORD pid, contexts, enabled, reserved; std::uint64_t heartbeat; };
struct Shared {
    DWORD magic, version, owner, reserved;
    std::uint64_t sequence, last_publish;
    std::array<Consumer, 16> consumers;
    std::array<ChapArmSample, kRingSize> samples;
};

class NamedLock {
    HANDLE handle_;
    bool acquired_;
public:
    explicit NamedLock(HANDLE handle) : handle_(handle), acquired_(false) {
        const auto result = WaitForSingleObject(handle, 100);
        acquired_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    ~NamedLock() { if (acquired_) ReleaseMutex(handle_); }
    explicit operator bool() const { return acquired_; }
};

bool validSession(const std::wstring& session) {
    if (session.empty() || session.size() > 64) return false;
    for (const auto c : session)
        if (!(c >= L'a' && c <= L'z') && !(c >= L'A' && c <= L'Z') &&
            !(c >= L'0' && c <= L'9') && c != L'_' && c != L'-') return false;
    return true;
}

std::wstring sessionName(const wchar_t* requested = nullptr) {
    if (requested) return requested;
    wchar_t value[65]{};
    const auto size = GetEnvironmentVariableW(L"CHAPARM_SESSION", value, 65);
    if (size >= 65) return {};
    return size ? std::wstring(value) : L"default";
}

struct Mapping {
    HANDLE mutex = nullptr, mapping = nullptr;
    Shared* data = nullptr;
    ~Mapping() {
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (mutex) CloseHandle(mutex);
    }
    bool open(const std::wstring& session) {
        if (!validSession(session)) { SetLastError(ERROR_INVALID_NAME); return false; }
        const auto name = L"Local\\ChapArm.v1." + session;
        mutex = CreateMutexW(nullptr, FALSE, (name + L".lock").c_str());
        if (!mutex) return false;
        NamedLock lock(mutex);
        if (!lock) return false;
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, static_cast<DWORD>(sizeof(Shared)), (name + L".ring").c_str());
        if (!mapping) return false;
        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        data = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        if (!data) return false;
        if (created) {
            std::memset(data, 0, sizeof(*data));
            data->version = 1;
            data->magic = kMagic;
        }
        if (data->magic != kMagic || data->version != 1) {
            SetLastError(ERROR_REVISION_MISMATCH); return false;
        }
        return true;
    }
};

bool alive(DWORD pid) {
    if (!pid) return false;
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
    const bool result = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return result;
}

bool validSample(const ChapArmSample& s) {
    return s.size == sizeof(s) && s.version == 1 &&
        std::isfinite(s.x) && s.x >= 0 && s.x <= 1 &&
        std::isfinite(s.y) && s.y >= 0 && s.y <= 1 &&
        std::isfinite(s.pressure) && s.pressure >= 0 && s.pressure <= 1 &&
        std::isfinite(s.azimuth_deg) && s.azimuth_deg >= 0 && s.azimuth_deg <= 360 &&
        std::isfinite(s.altitude_deg) && s.altitude_deg >= 0 && s.altitude_deg <= 90 &&
        std::isfinite(s.twist_deg) && s.twist_deg >= 0 && s.twist_deg <= 360 &&
        s.proximity <= 1 && !(s.buttons & ~7u) &&
        (s.proximity || (!s.buttons && s.pressure == 0));
}

void append(Shared& data, ChapArmSample sample) {
    if (!sample.timestamp_ms) sample.timestamp_ms = GetTickCount64();
    if (!sample.proximity) { sample.pressure = 0; sample.buttons = 0; }
    // Contact determines the tip switch; callers cannot make a hover paint.
    sample.buttons = (sample.buttons & ~1u) | (sample.pressure > 0 ? 1u : 0u);
    ++data.sequence;
    data.samples[(data.sequence - 1) % kRingSize] = sample;
    data.last_publish = GetTickCount64();
}

struct Publisher { Mapping memory; };
std::mutex publisherMutex;
std::unordered_map<HANDLE, std::unique_ptr<Publisher>> publishers;

template<typename T> UINT copyValue(void* out, const T& value) {
    if (out) std::memcpy(out, &value, sizeof(value));
    return sizeof(value);
}
UINT copyBytes(void* out, const void* data, std::size_t size) {
    if (out) std::memcpy(out, data, size);
    return static_cast<UINT>(size);
}
UINT copyText(bool wide, void* out, const wchar_t* value) {
    if (wide) return copyBytes(out, value, (std::wcslen(value) + 1) * sizeof(wchar_t));
    const int bytes = WideCharToMultiByte(CP_ACP, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (out) WideCharToMultiByte(CP_ACP, 0, value, -1, static_cast<char*>(out), bytes, nullptr, nullptr);
    return bytes;
}
LOGCONTEXTW toWide(const LOGCONTEXTA& input) {
    LOGCONTEXTW out{};
    std::memcpy(&out.lcOptions, &input.lcOptions, sizeof(input) - sizeof(input.lcName));
    MultiByteToWideChar(CP_ACP, 0, input.lcName, LCNAMELEN, out.lcName, LCNAMELEN);
    out.lcName[LCNAMELEN - 1] = 0;
    return out;
}
LOGCONTEXTA toAnsi(const LOGCONTEXTW& input) {
    LOGCONTEXTA out{};
    std::memcpy(&out.lcOptions, &input.lcOptions, sizeof(out) - sizeof(out.lcName));
    WideCharToMultiByte(CP_ACP, 0, input.lcName, -1, out.lcName, LCNAMELEN, nullptr, nullptr);
    out.lcName[LCNAMELEN - 1] = 0;
    return out;
}

LOGCONTEXTW defaultContext(bool system) {
    LOGCONTEXTW lc{};
    const wchar_t name[] = L"ChapArm virtual right-hand pen";
    std::copy(std::begin(name), std::end(name), lc.lcName);
    lc.lcOptions = CXO_MESSAGES | CXO_CSRMESSAGES | (system ? CXO_SYSTEM : 0);
    lc.lcStatus = CXS_ONTOP;
    lc.lcMsgBase = WT_DEFBASE;
    lc.lcPktRate = 240;
    lc.lcPktData = kPacketData;
    lc.lcMoveMask = kPacketData;
    lc.lcBtnDnMask = lc.lcBtnUpMask = 7;
    lc.lcInExtX = lc.lcInExtY = kAxisMax;
    lc.lcInExtZ = 1;
    lc.lcOutExtX = kAxisMax;
    lc.lcOutExtY = kAxisMax;
    lc.lcOutExtZ = 1;
    lc.lcSensX = lc.lcSensY = lc.lcSensZ = 65536;
    lc.lcSysSensX = lc.lcSysSensY = 65536;
    lc.lcSysMode = TRUE;
    lc.lcSysOrgX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    lc.lcSysOrgY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    lc.lcSysExtX = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    lc.lcSysExtY = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (system) {
        lc.lcOutOrgX = lc.lcSysOrgX;
        lc.lcOutOrgY = lc.lcSysOrgY;
        lc.lcOutExtX = lc.lcSysExtX;
        lc.lcOutExtY = -lc.lcSysExtY;
    }
    return lc;
}

bool validate(LOGCONTEXTW& lc) {
    if (lc.lcDevice != 0 || lc.lcInExtX <= 0 || lc.lcInExtY <= 0 ||
        !lc.lcOutExtX || !lc.lcOutExtY ||
        lc.lcOutExtX == std::numeric_limits<LONG>::min() ||
        lc.lcOutExtY == std::numeric_limits<LONG>::min() ||
        lc.lcMsgBase < WM_USER || lc.lcMsgBase > 0xffff - WT_MAXOFFSET ||
        !lc.lcPktData || (lc.lcPktData & ~kPacketData) ||
        (lc.lcPktMode & ~kRelativeData) || (lc.lcPktMode & ~lc.lcPktData)) return false;
    // Bound coordinate arithmetic while allowing desktop offsets and high
    // resolution application coordinate systems far beyond ordinary displays.
    for (const LONG value : {lc.lcInOrgX, lc.lcInOrgY, lc.lcInExtX, lc.lcInExtY,
        lc.lcOutOrgX, lc.lcOutOrgY, lc.lcOutExtX, lc.lcOutExtY})
        if (std::abs(static_cast<double>(value)) > 100000000) return false;
    // This provider offers ordinary packet contexts; no OS mouse injection.
    lc.lcOptions &= CXO_SYSTEM | CXO_MESSAGES | CXO_CSRMESSAGES;
    lc.lcPktRate = 240;
    lc.lcName[LCNAMELEN - 1] = 0;
    return true;
}

struct Packet {
    UINT serial, status;
    DWORD time, buttons;
    WTPKT changed;
    LONG x, y, z;
    UINT pressure;
    ORIENTATION orientation;
    bool proximity, transition;
};
struct Context {
    HWND window;
    LOGCONTEXTW lc;
    bool enabled;
    std::size_t capacity = 128;
    std::deque<Packet> queue;
    Packet last{};
    bool haveLast = false;
};

LONG mapped(double raw, LONG origin, LONG extent, LONG outOrigin, LONG outExtent) {
    double fraction = (raw - origin) / extent;
    if (outExtent < 0) fraction = 1 - fraction;
    const double value = outOrigin + fraction * std::abs(static_cast<double>(outExtent));
    return static_cast<LONG>(std::llround(std::clamp(value,
        static_cast<double>(std::numeric_limits<LONG>::min()),
        static_cast<double>(std::numeric_limits<LONG>::max()))));
}

class Provider {
public:
    std::mutex mutex;
    std::unordered_map<HCTX, std::unique_ptr<Context>> contexts;
    std::vector<HCTX> order; // top first
    Mapping memory;
    bool started = false;
    std::uint64_t consumed = 0;
    std::uintptr_t nextHandle = 0x100;
    UINT serial = 0;
    ChapArmSample latest{sizeof(ChapArmSample), 1, 0, 0, 0, 0, 90, 0, 0, 0, 0};

    bool start() {
        if (started) return true;
        if (!memory.open(sessionName())) return false;
        // A background reader must not outlive its DLL. Pin this app-local
        // provider for the process lifetime; no worker is created in DllMain.
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&ChapArmGetStatus), &module)) return false;
        {
            NamedLock lock(memory.mutex);
            if (!lock) return false;
            consumed = memory.data->sequence;
        }
        std::thread([this] { run(); }).detach();
        started = true;
        return true;
    }
    Context* find(HCTX handle) {
        const auto it = contexts.find(handle);
        return it == contexts.end() ? nullptr : it->second.get();
    }
    void notify(Context& c, UINT offset, WPARAM w, LPARAM l) {
        if (c.window) PostMessageW(c.window, c.lc.lcMsgBase + offset, w, l);
    }
    void updateOrder() {
        bool active = false;
        for (auto handle : order) {
            auto& c = *contexts.at(handle);
            c.lc.lcStatus = !c.enabled ? CXS_DISABLED : (active ? CXS_OBSCURED : CXS_ONTOP);
            active = active || c.enabled;
            notify(c, 4, reinterpret_cast<WPARAM>(handle), c.lc.lcStatus);
        }
    }
    void emit(Context& c, HCTX handle, const ChapArmSample& s, bool error = false) {
        // Qt/Krita consumes one packet to discover the cursor on proximity
        // entry. Supply a hover before the first contact so that a down/up
        // pair arriving in one reader batch cannot lose its initial press.
        if (s.proximity && s.buttons && (!c.haveLast || !c.last.proximity)) {
            auto hover = s; hover.pressure = 0; hover.buttons = 0;
            emit(c, handle, hover, error);
        }
        if ((c.lc.lcPktMode & PK_BUTTONS) && c.haveLast) {
            for (unsigned step = 0; step < 2; ++step) {
                const auto changes = s.buttons ^ c.last.buttons;
                if (!changes || !(changes & (changes - 1))) break;
                const auto first = changes & (~changes + 1);
                auto intermediate = s;
                intermediate.buttons = c.last.buttons ^ first;
                intermediate.proximity = 1;
                // Relative Wintab buttons carry one transition per packet.
                const auto oldButtons = c.last.buttons;
                emit(c, handle, intermediate, error);
                if (c.last.buttons == oldButtons) break; // transition masked out
            }
        }
        Packet p{};
        p.serial = ++serial;
        p.time = static_cast<DWORD>(s.timestamp_ms);
        p.proximity = s.proximity != 0;
        p.status = (p.proximity ? 0 : TPS_PROXIMITY) | (error ? TPS_QUEUE_ERR : 0);
        p.buttons = p.proximity ? s.buttons : 0;
        p.pressure = p.proximity ? static_cast<UINT>(std::lround(s.pressure * kPressureMax)) : 0;
        p.x = mapped(s.x * kAxisMax, c.lc.lcInOrgX, c.lc.lcInExtX, c.lc.lcOutOrgX, c.lc.lcOutExtX);
        p.y = mapped((1 - s.y) * kAxisMax, c.lc.lcInOrgY, c.lc.lcInExtY, c.lc.lcOutOrgY, c.lc.lcOutExtY);
        p.orientation = {static_cast<int>(std::lround(s.azimuth_deg * 10)),
            static_cast<int>(std::lround(s.altitude_deg * 10)),
            static_cast<int>(std::lround(s.twist_deg * 10))};
        const Packet before = c.last;
        p.changed = !c.haveLast ? c.lc.lcPktData :
            PK_TIME | PK_SERIAL_NUMBER |
            (p.x != before.x ? PK_X : 0) | (p.y != before.y ? PK_Y : 0) |
            (p.buttons != before.buttons ? PK_BUTTONS : 0) |
            (p.pressure != before.pressure ? PK_NORMAL_PRESSURE : 0) |
            (p.status != before.status ? PK_STATUS : 0) |
            (std::memcmp(&p.orientation, &before.orientation, sizeof(ORIENTATION)) ? PK_ORIENTATION : 0);
        p.changed &= c.lc.lcPktData;
        p.transition = !c.haveLast || p.buttons != before.buttons || p.proximity != before.proximity;
        const bool entering = p.proximity && (!c.haveLast || !before.proximity);
        const bool leaving = c.haveLast && before.proximity && !p.proximity;
        const auto buttonChanges = p.buttons ^ before.buttons;
        const bool selected = !c.haveLast || entering || leaving || error ||
            (buttonChanges & p.buttons & c.lc.lcBtnDnMask) ||
            (buttonChanges & before.buttons & c.lc.lcBtnUpMask) ||
            (p.changed & c.lc.lcMoveMask & ~PK_BUTTONS);
        if (!selected) return;
        c.last = p;
        c.haveLast = true;
        // Relative data is relative to the preceding sample for this context.
        const auto difference = [](LONG value, LONG previous) {
            return static_cast<LONG>(std::clamp(static_cast<std::int64_t>(value) - previous,
                static_cast<std::int64_t>(std::numeric_limits<LONG>::min()),
                static_cast<std::int64_t>(std::numeric_limits<LONG>::max())));
        };
        if (c.lc.lcPktMode & PK_X) p.x = difference(p.x, before.x);
        if (c.lc.lcPktMode & PK_Y) p.y = difference(p.y, before.y);
        if (c.lc.lcPktMode & PK_NORMAL_PRESSURE) p.pressure -= before.pressure;
        if (c.lc.lcPktMode & PK_ORIENTATION) {
            p.orientation.orAzimuth -= before.orientation.orAzimuth;
            p.orientation.orAltitude -= before.orientation.orAltitude;
            p.orientation.orTwist -= before.orientation.orTwist;
        }
        if (c.lc.lcPktMode & PK_BUTTONS) {
            const auto changed = p.buttons ^ before.buttons;
            UINT bit = 0;
            while (bit < 3 && !(changed & (1u << bit))) ++bit;
            p.buttons = bit == 3 ? 0 : MAKELONG(bit, (p.buttons & (1u << bit)) ? TBN_DOWN : TBN_UP);
        }
        if (c.queue.size() >= c.capacity) {
            const auto motion = std::find_if(c.queue.begin(), c.queue.end(), [](const auto& old) { return !old.transition; });
            if (motion != c.queue.end()) c.queue.erase(motion);
            else c.queue.pop_front();
            p.status |= TPS_QUEUE_ERR;
        }
        c.queue.push_back(p);
        // Queue before notifying: Qt reads the first packet on proximity entry.
        if (entering) {
            notify(c, 5, reinterpret_cast<WPARAM>(handle), MAKELPARAM(1, 1));
            if (c.lc.lcOptions & CXO_CSRMESSAGES)
                notify(c, 7, p.serial, reinterpret_cast<LPARAM>(handle));
        }
        if (c.lc.lcOptions & CXO_MESSAGES) notify(c, 0, p.serial, reinterpret_cast<LPARAM>(handle));
        if (leaving) notify(c, 5, reinterpret_cast<WPARAM>(handle), MAKELPARAM(0, 1));
    }
    void dispatch(const ChapArmSample& s, bool error = false) {
        HCTX selected = nullptr;
        DWORD foregroundPid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
        if (s.proximity) {
            for (auto handle : order) {
                const auto& c = *contexts.at(handle);
                const auto x = s.x * kAxisMax, y = (1 - s.y) * kAxisMax;
                if (c.enabled && (!c.window || foregroundPid == GetCurrentProcessId()) && x >= c.lc.lcInOrgX &&
                    x <= static_cast<double>(c.lc.lcInOrgX) + c.lc.lcInExtX &&
                    y >= c.lc.lcInOrgY &&
                    y <= static_cast<double>(c.lc.lcInOrgY) + c.lc.lcInExtY) {
                    selected = handle; break;
                }
            }
        }
        for (auto handle : order) {
            auto& c = *contexts.at(handle);
            if (handle == selected) emit(c, handle, s, error);
            else if (c.enabled && c.haveLast && c.last.proximity) {
                auto up = s; up.proximity = 0; up.buttons = 0; up.pressure = 0;
                emit(c, handle, up, error);
            }
        }
        latest = s;
    }
    void heartbeat() {
        auto& slots = memory.data->consumers;
        const auto now = GetTickCount64();
        Consumer* slot = nullptr;
        for (auto& entry : slots) if (entry.pid == GetCurrentProcessId()) { slot = &entry; break; }
        if (!slot) for (auto& entry : slots)
            if (!entry.pid || now - entry.heartbeat > 2000) { slot = &entry; break; }
        if (!slot) return;
        *slot = {GetCurrentProcessId(), static_cast<DWORD>(contexts.size()), 0, 0, now};
        for (const auto& item : contexts) if (item.second->enabled) ++slot->enabled;
    }
    void run() {
        for (;;) {
            try {
                std::lock_guard<std::mutex> guard(mutex);
                std::vector<ChapArmSample> batch;
                bool overflow = false, stale = false;
                {
                    NamedLock lock(memory.mutex);
                    if (lock) {
                        heartbeat();
                        auto& data = *memory.data;
                        stale = !data.owner || GetTickCount64() - data.last_publish > kStaleMs;
                        overflow = data.sequence < consumed || data.sequence - consumed > kRingSize;
                        if (!overflow) {
                            batch.reserve(static_cast<std::size_t>(data.sequence - consumed));
                            for (auto seq = consumed; seq < data.sequence; ++seq)
                                batch.push_back(data.samples[seq % kRingSize]);
                        }
                        consumed = data.sequence;
                    } else stale = true;
                }
                if (overflow) {
                    auto up = latest; up.proximity = 0; up.pressure = 0; up.buttons = 0;
                    up.timestamp_ms = GetTickCount64(); dispatch(up, true);
                }
                for (const auto& sample : batch) if (validSample(sample)) dispatch(sample);
                if (stale && latest.proximity) {
                    auto up = latest; up.proximity = 0; up.pressure = 0; up.buttons = 0;
                    up.timestamp_ms = GetTickCount64(); dispatch(up);
                }
            } catch (...) { /* A bad consumer cannot terminate the painting app. */ }
            Sleep(4);
        }
    }
};

Provider& provider() { static auto* value = new Provider; return *value; }

UINT info(bool wide, UINT category, UINT index, void* output) {
    if (!category) return sizeof(LOGCONTEXTW);
    if (category == WTI_DEFCONTEXT || category == WTI_DEFSYSCTX ||
        category == WTI_DDCTXS || category == WTI_DSCTXS) {
        const auto lc = defaultContext(category == WTI_DEFSYSCTX || category == WTI_DSCTXS);
        if (!index) return wide ? copyValue(output, lc) : copyValue(output, toAnsi(lc));
        if (index == CTX_NAME) return copyText(wide, output, lc.lcName);
        if (index >= CTX_OPTIONS && index <= CTX_MAX) {
            const auto* fields = reinterpret_cast<const unsigned char*>(&lc.lcOptions);
            return copyBytes(output, fields + (index - CTX_OPTIONS) * 4, 4);
        }
        return 0;
    }
    if (category == WTI_INTERFACE) {
        switch (index) {
        case IFC_WINTABID: return copyText(wide, output, L"ChapArm Wintab 1.4");
        case IFC_SPECVERSION: return copyValue(output, WORD{0x0104});
        case IFC_IMPLVERSION: return copyValue(output, WORD{0x0100});
        case IFC_NDEVICES: return copyValue(output, UINT{1});
        case IFC_NCURSORS: return copyValue(output, kCursorCount);
        case IFC_NCONTEXTS: return copyValue(output, UINT{32});
        case IFC_CTXOPTIONS: return copyValue(output, UINT{CXO_SYSTEM | CXO_MESSAGES | CXO_CSRMESSAGES});
        case IFC_CTXSAVESIZE: return copyValue(output, UINT{sizeof(LOGCONTEXTW)});
        case IFC_NEXTENSIONS: case IFC_NMANAGERS: return copyValue(output, UINT{0});
        default: return 0;
        }
    }
    if (category == WTI_DEVICES) {
        switch (index) {
        case DVC_NAME: return copyText(wide, output, L"ChapArm virtual pen");
        case DVC_PNPID: return copyText(wide, output, L"CHAPARM\\VIRTUAL_PEN");
        case DVC_HARDWARE: return copyValue(output, UINT{HWC_HARDPROX | HWC_PHYSID_CURSORS});
        case DVC_NCSRTYPES: return copyValue(output, kCursorCount);
        case DVC_FIRSTCSR: return copyValue(output, UINT{0});
        case DVC_PKTRATE: return copyValue(output, UINT{240});
        case DVC_PKTDATA: case DVC_CSRDATA: return copyValue(output, kPacketData);
        case DVC_PKTMODE: return copyValue(output, kRelativeData);
        case DVC_XMARGIN: case DVC_YMARGIN: case DVC_ZMARGIN: return copyValue(output, UINT{0});
        case DVC_X: case DVC_Y: return copyValue(output, AXIS{0, kAxisMax, TU_NONE, 65536});
        case DVC_Z: return copyValue(output, AXIS{0, 1, TU_NONE, 65536});
        case DVC_NPRESSURE: return copyValue(output, AXIS{0, kPressureMax, TU_NONE, 65536});
        case DVC_TPRESSURE: return copyValue(output, AXIS{0, 1, TU_NONE, 65536});
        case DVC_ORIENTATION: {
            const AXIS axes[3]{{0, 3600, TU_CIRCLE, 3600u << 16},
                {-900, 900, TU_CIRCLE, 3600u << 16}, {0, 3600, TU_CIRCLE, 3600u << 16}};
            return copyBytes(output, axes, sizeof(axes));
        }
        default: return 0;
        }
    }
    if (category >= WTI_CURSORS && category < WTI_CURSORS + kCursorCount) {
        const bool pen = category == WTI_CURSORS + kPenCursor;
        switch (index) {
        case CSR_NAME: return copyText(wide, output, pen ? L"ChapArm pen tip" : L"ChapArm inactive cursor");
        case CSR_ACTIVE: return copyValue(output, BOOL{pen});
        case CSR_PKTDATA: return copyValue(output, pen ? kPacketData : WTPKT{0});
        case CSR_BUTTONS: case CSR_BUTTONBITS: return copyValue(output, static_cast<BYTE>(pen ? 3 : 0));
        case CSR_BUTTONMAP: case CSR_SYSBTNMAP: {
            std::array<BYTE, 32> map{};
            if (pen) {
                map[0] = index == CSR_SYSBTNMAP ? SBN_LCLICK : 0;
                map[1] = index == CSR_SYSBTNMAP ? SBN_RCLICK : 1;
                map[2] = index == CSR_SYSBTNMAP ? SBN_MCLICK : 2;
            }
            return copyValue(output, map);
        }
        case CSR_NPBUTTON: return pen ? copyValue(output, BYTE{0}) : 0;
        case CSR_NPBTNMARKS: {
            const UINT marks[2]{1, 1}; return pen ? copyBytes(output, marks, sizeof(marks)) : 0;
        }
        case CSR_PHYSID: return copyValue(output, DWORD{pen ? 0x43484101u : 0x43484100u});
        case CSR_TYPE: return copyValue(output, UINT{pen ? 0x0802u : 0x0006u}); // stylus / inactive puck
        case CSR_MODE: return copyValue(output, BOOL{FALSE});
        case CSR_MINPKTDATA: return copyValue(output, pen ? WTPKT{PK_X | PK_Y | PK_BUTTONS} : WTPKT{0});
        case CSR_MINBUTTONS: return copyValue(output, UINT{pen ? 1u : 0u});
        case CSR_CAPABILITIES: return copyValue(output, UINT{0});
        default: return 0;
        }
    }
    if (category == WTI_STATUS) {
        auto& p = provider(); std::lock_guard<std::mutex> lock(p.mutex);
        switch (index) {
        case STA_CONTEXTS: return copyValue(output, UINT{static_cast<UINT>(p.contexts.size())});
        case STA_PKTRATE: return copyValue(output, UINT{240});
        case STA_PKTDATA: return copyValue(output, kPacketData);
        case STA_SYSTEM: return copyValue(output, BOOL{FALSE});
        case STA_SYSCTXS: case STA_MANAGERS: return copyValue(output, UINT{0});
        default: return 0;
        }
    }
    return 0;
}

template<typename T> void field(unsigned char*& out, const T& value) {
    if (out) { std::memcpy(out, &value, sizeof(value)); out += sizeof(value); }
}
std::size_t packetSize(WTPKT mask) {
    std::size_t size = mask & PK_CONTEXT ? sizeof(HCTX) : 0;
    for (auto bit = PK_STATUS; bit <= PK_TANGENT_PRESSURE; bit <<= 1)
        if (mask & bit) size += 4;
    if (mask & PK_ORIENTATION) size += sizeof(ORIENTATION);
    // Match native C structure trailing alignment when the packet has HCTX.
    const auto alignment = mask & PK_CONTEXT ? alignof(HCTX) : 4;
    return (size + alignment - 1) & ~(alignment - 1);
}
void pack(Context& c, HCTX handle, const Packet& packet, unsigned char* out) {
    if (!out) return;
    std::memset(out, 0, packetSize(c.lc.lcPktData));
    const auto mask = c.lc.lcPktData;
    if (mask & PK_CONTEXT) field(out, handle);
    if (mask & PK_STATUS) field(out, packet.status);
    if (mask & PK_TIME) field(out, packet.time);
    if (mask & PK_CHANGED) field(out, packet.changed);
    if (mask & PK_SERIAL_NUMBER) field(out, packet.serial);
    if (mask & PK_CURSOR) field(out, kPenCursor);
    if (mask & PK_BUTTONS) field(out, packet.buttons);
    if (mask & PK_X) field(out, packet.x);
    if (mask & PK_Y) field(out, packet.y);
    if (mask & PK_Z) field(out, packet.z);
    if (mask & PK_NORMAL_PRESSURE) field(out, packet.pressure);
    if (mask & PK_TANGENT_PRESSURE) field(out, UINT{0});
    if (mask & PK_ORIENTATION) field(out, packet.orientation);
}
int packets(HCTX handle, int maximum, void* output, bool remove) {
    if (maximum < 0) return 0;
    auto& p = provider(); std::lock_guard<std::mutex> lock(p.mutex);
    auto* c = p.find(handle); if (!c) return 0;
    const auto count = std::min<std::size_t>(maximum, c->queue.size());
    auto* out = static_cast<unsigned char*>(output);
    for (std::size_t i = 0; i < count; ++i) {
        pack(*c, handle, c->queue[i], out);
        if (out) out += packetSize(c->lc.lcPktData);
    }
    if (remove) c->queue.erase(c->queue.begin(), c->queue.begin() + count);
    return static_cast<int>(count);
}
int rangePackets(HCTX handle, UINT first, UINT last, int maximum, void* output, int* copied, bool remove) {
    if (copied) *copied = 0;
    if (maximum < 0) return 0;
    auto& p = provider(); std::lock_guard<std::mutex> lock(p.mutex);
    auto* c = p.find(handle); if (!c) return 0;
    int found = 0, count = 0;
    auto* out = static_cast<unsigned char*>(output);
    for (auto it = c->queue.begin(); it != c->queue.end();) {
        // Unsigned subtraction preserves ranges crossing the serial wrap.
        if (it->serial - first <= last - first) {
            ++found;
            if (count < maximum) {
                pack(*c, handle, *it, out);
                if (out) out += packetSize(c->lc.lcPktData);
                ++count;
                if (remove) { it = c->queue.erase(it); continue; }
            }
        }
        ++it;
    }
    if (copied) *copied = count;
    return found;
}
} // namespace

extern "C" UINT WINAPI WTInfoA(UINT category, UINT index, void* out) { return info(false, category, index, out); }
extern "C" UINT WINAPI WTInfoW(UINT category, UINT index, void* out) { return info(true, category, index, out); }

extern "C" HCTX WINAPI WTOpenW(HWND window, LOGCONTEXTW* lc, BOOL enabled) {
    if (!lc || (window && !IsWindow(window))) return nullptr;
    auto checked = *lc;
    if (!validate(checked)) { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
    try {
        auto& p = provider(); std::lock_guard<std::mutex> lock(p.mutex);
        if (p.contexts.size() >= 32 || !p.start()) return nullptr;
        auto c = std::make_unique<Context>(); c->window = window; c->lc = checked; c->enabled = enabled != FALSE;
        const auto handle = reinterpret_cast<HCTX>(++p.nextHandle);
        auto* context = c.get();
        p.contexts.emplace(handle, std::move(c)); p.order.insert(p.order.begin(), handle);
        p.notify(*context, 1, reinterpret_cast<WPARAM>(handle), 0);
        p.updateOrder(); *lc = context->lc;
        return handle;
    } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
}
extern "C" HCTX WINAPI WTOpenA(HWND window, LOGCONTEXTA* lc, BOOL enabled) {
    if (!lc) return nullptr; auto wide = toWide(*lc);
    const auto handle = WTOpenW(window, &wide, enabled); if (handle) *lc = toAnsi(wide); return handle;
}
extern "C" BOOL WINAPI WTClose(HCTX handle) {
    auto& p = provider(); std::lock_guard<std::mutex> lock(p.mutex);
    auto* c = p.find(handle); if (!c) return FALSE;
    p.notify(*c, 2, reinterpret_cast<WPARAM>(handle), 0);
    p.contexts.erase(handle); p.order.erase(std::remove(p.order.begin(), p.order.end(), handle), p.order.end());
    p.updateOrder(); return TRUE;
}
extern "C" BOOL WINAPI WTEnable(HCTX handle, BOOL enabled) {
    auto& p = provider(); std::lock_guard<std::mutex> lock(p.mutex);
    auto* c = p.find(handle); if (!c) return FALSE;
    if (c->enabled == (enabled != FALSE)) return TRUE;
    c->enabled = enabled != FALSE;
    if (!enabled) {
        if (c->haveLast && c->last.proximity) p.notify(*c, 5, reinterpret_cast<WPARAM>(handle), MAKELPARAM(0, 1));
        c->queue.clear(); c->haveLast = false;
    }
    p.updateOrder(); return TRUE;
}
extern "C" BOOL WINAPI WTOverlap(HCTX handle, BOOL top) {
    auto& p = provider(); std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.find(handle)) return FALSE;
    p.order.erase(std::remove(p.order.begin(), p.order.end(), handle), p.order.end());
    if (top) p.order.insert(p.order.begin(), handle); else p.order.push_back(handle);
    p.updateOrder(); return TRUE;
}
extern "C" BOOL WINAPI WTGetW(HCTX handle, LOGCONTEXTW* lc) {
    if (!lc) return FALSE;
    auto& p = provider(); std::lock_guard<std::mutex> lock(p.mutex);
    const auto* c = p.find(handle); if (!c) return FALSE; *lc = c->lc; return TRUE;
}
extern "C" BOOL WINAPI WTGetA(HCTX handle, LOGCONTEXTA* lc) {
    if (!lc) return FALSE; LOGCONTEXTW wide{};
    if (!WTGetW(handle, &wide)) return FALSE; *lc = toAnsi(wide); return TRUE;
}
extern "C" BOOL WINAPI WTSetW(HCTX handle, LOGCONTEXTW* lc) {
    if (!lc) return FALSE; auto checked = *lc;
    if (!validate(checked)) return FALSE;
    auto& p = provider(); std::lock_guard<std::mutex> lock(p.mutex);
    auto* c = p.find(handle); if (!c) return FALSE;
    checked.lcStatus = c->lc.lcStatus; c->lc = checked; c->queue.clear(); c->haveLast = false;
    *lc = c->lc; p.notify(*c, 3, reinterpret_cast<WPARAM>(handle), c->lc.lcStatus); return TRUE;
}
extern "C" BOOL WINAPI WTSetA(HCTX handle, LOGCONTEXTA* lc) {
    if (!lc) return FALSE; auto wide = toWide(*lc);
    if (!WTSetW(handle, &wide)) return FALSE; *lc = toAnsi(wide); return TRUE;
}
extern "C" int WINAPI WTPacketsGet(HCTX h, int n, void* out) { return packets(h, n, out, true); }
extern "C" int WINAPI WTPacketsPeek(HCTX h, int n, void* out) { return packets(h, n, out, false); }
extern "C" int WINAPI WTDataGet(HCTX h, UINT a, UINT b, int n, void* out, int* count) { return rangePackets(h, a, b, n, out, count, true); }
extern "C" int WINAPI WTDataPeek(HCTX h, UINT a, UINT b, int n, void* out, int* count) { return rangePackets(h, a, b, n, out, count, false); }
extern "C" BOOL WINAPI WTPacket(HCTX handle, UINT serial, void* out) {
    auto& p = provider(); std::lock_guard<std::mutex> lock(p.mutex);
    auto* c = p.find(handle); if (!c) return FALSE;
    const auto it = std::find_if(c->queue.begin(), c->queue.end(), [serial](const auto& item) { return item.serial == serial; });
    if (it == c->queue.end()) return FALSE;
    pack(*c, handle, *it, static_cast<unsigned char*>(out)); c->queue.erase(c->queue.begin(), it + 1); return TRUE;
}
extern "C" int WINAPI WTQueueSizeGet(HCTX handle) {
    auto& p = provider(); std::lock_guard<std::mutex> lock(p.mutex);
    const auto* c = p.find(handle); return c ? static_cast<int>(c->capacity) : 0;
}
extern "C" BOOL WINAPI WTQueueSizeSet(HCTX handle, int capacity) {
    if (capacity < 1 || capacity > 4096) return FALSE;
    auto& p = provider(); std::lock_guard<std::mutex> lock(p.mutex);
    auto* c = p.find(handle); if (!c) return FALSE;
    // Do not lose a queued release by shrinking a live queue.
    if (c->queue.size() > static_cast<std::size_t>(capacity)) return FALSE;
    c->capacity = capacity; return TRUE;
}
extern "C" BOOL WINAPI WTQueuePacketsEx(HCTX handle, UINT* first, UINT* last) {
    auto& p = provider(); std::lock_guard<std::mutex> lock(p.mutex);
    const auto* c = p.find(handle); if (!c || c->queue.empty()) return FALSE;
    if (first) *first = c->queue.front().serial; if (last) *last = c->queue.back().serial; return TRUE;
}
extern "C" BOOL WINAPI WTSave(HCTX handle, void* data) { return WTGetW(handle, static_cast<LOGCONTEXTW*>(data)); }
extern "C" HCTX WINAPI WTRestore(HWND window, void* data, BOOL enabled) {
    if (!data) return nullptr; auto lc = *static_cast<LOGCONTEXTW*>(data); return WTOpenW(window, &lc, enabled);
}

extern "C" HANDLE WINAPI ChapArmOpenPublisher(const wchar_t* session) {
    try {
        auto publisher = std::make_unique<Publisher>();
        if (!publisher->memory.open(sessionName(session))) return nullptr;
        std::lock_guard<std::mutex> guard(publisherMutex);
        NamedLock lock(publisher->memory.mutex); if (!lock) return nullptr;
        auto& shared = *publisher->memory.data;
        if (alive(shared.owner)) { SetLastError(ERROR_BUSY); return nullptr; }
        shared.owner = GetCurrentProcessId();
        shared.last_publish = 0;
        const auto handle = reinterpret_cast<HANDLE>(publisher.get());
        publishers.emplace(handle, std::move(publisher)); return handle;
    } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
}
extern "C" BOOL WINAPI ChapArmPublish(HANDLE handle, const ChapArmSample* sample) {
    if (!sample || !validSample(*sample)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    std::lock_guard<std::mutex> guard(publisherMutex);
    const auto it = publishers.find(handle); if (it == publishers.end()) return FALSE;
    auto& memory = it->second->memory; NamedLock lock(memory.mutex); if (!lock) return FALSE;
    if (memory.data->owner != GetCurrentProcessId()) return FALSE;
    append(*memory.data, *sample); return TRUE;
}
extern "C" BOOL WINAPI ChapArmClosePublisher(HANDLE handle) {
    std::unique_ptr<Publisher> publisher;
    {
        std::lock_guard<std::mutex> guard(publisherMutex);
        const auto it = publishers.find(handle); if (it == publishers.end()) return FALSE;
        NamedLock lock(it->second->memory.mutex); if (!lock) return FALSE;
        auto& data = *it->second->memory.data;
        ChapArmSample release{sizeof(ChapArmSample), 1, 0, 0, 0, 0, 90, 0, 0, 0, 0};
        if (data.sequence) release = data.samples[(data.sequence - 1) % kRingSize];
        release.proximity = 0; release.buttons = 0; release.pressure = 0; release.timestamp_ms = GetTickCount64();
        append(data, release); data.owner = 0;
        // Keep the handle valid when locking fails, and release the named
        // mutex before the mapping object closes its Windows handles.
        publisher = std::move(it->second); publishers.erase(it);
    }
    return TRUE;
}
extern "C" BOOL WINAPI ChapArmGetStatus(const wchar_t* session, ChapArmStatus* status) {
    if (!status || status->size != sizeof(*status) || status->version != 1) return FALSE;
    try {
        Mapping memory; if (!memory.open(sessionName(session))) return FALSE;
        NamedLock lock(memory.mutex); if (!lock) return FALSE;
        ChapArmStatus value{sizeof(ChapArmStatus), 1, 0, 0, 0, 0, 0, 0, 0};
        const auto& data = *memory.data;
        value.publisher_pid = alive(data.owner) ? data.owner : 0;
        value.last_publish_tick_ms = data.last_publish;
        value.published_samples = data.sequence;
        const auto now = GetTickCount64();
        for (const auto& entry : data.consumers) {
            if (!entry.pid || !entry.contexts || now - entry.heartbeat > 1000 || !alive(entry.pid)) continue;
            value.context_count += entry.contexts; value.enabled_context_count += entry.enabled;
            if (entry.heartbeat >= value.last_consumer_tick_ms) {
                value.last_consumer_tick_ms = entry.heartbeat; value.consumer_pid = entry.pid;
            }
        }
        *status = value; return TRUE;
    } catch (...) { return FALSE; }
}

// Optional manager/configuration/extensions are exported for clients that
// resolve the complete ABI, and consistently report unsupported capability.
extern "C" BOOL WINAPI WTConfig(HCTX, HWND) { return FALSE; }
extern "C" BOOL WINAPI WTExtGet(HCTX, UINT, void*) { return FALSE; }
extern "C" BOOL WINAPI WTExtSet(HCTX, UINT, void*) { return FALSE; }
extern "C" HMGR WINAPI WTMgrOpen(HWND, UINT) { return nullptr; }
extern "C" BOOL WINAPI WTMgrClose(HMGR) { return FALSE; }
extern "C" BOOL WINAPI WTMgrContextEnum(HMGR, WTENUMPROC, LPARAM) { return FALSE; }
extern "C" HWND WINAPI WTMgrContextOwner(HMGR, HCTX) { return nullptr; }
extern "C" HCTX WINAPI WTMgrDefContext(HMGR, BOOL) { return nullptr; }
extern "C" HCTX WINAPI WTMgrDefContextEx(HMGR, UINT, BOOL) { return nullptr; }
extern "C" UINT WINAPI WTMgrDeviceConfig(HMGR, UINT, HWND) { return WTDC_NONE; }
extern "C" BOOL WINAPI WTMgrExt(HMGR, UINT, void*) { return FALSE; }
extern "C" BOOL WINAPI WTMgrCsrEnable(HMGR, UINT, BOOL) { return FALSE; }
extern "C" BOOL WINAPI WTMgrCsrButtonMap(HMGR, UINT, LPBYTE, LPBYTE) { return FALSE; }
extern "C" BOOL WINAPI WTMgrCsrPressureBtnMarks(HMGR, UINT, DWORD, DWORD) { return FALSE; }
extern "C" BOOL WINAPI WTMgrCsrPressureResponse(HMGR, UINT, UINT*, UINT*) { return FALSE; }
extern "C" BOOL WINAPI WTMgrCsrExt(HMGR, UINT, UINT, void*) { return FALSE; }
extern "C" BOOL WINAPI WTMgrConfigReplaceExA(HMGR, BOOL, LPSTR, LPSTR) { return FALSE; }
extern "C" BOOL WINAPI WTMgrConfigReplaceExW(HMGR, BOOL, LPWSTR, LPSTR) { return FALSE; }
extern "C" HWTHOOK WINAPI WTMgrPacketHookExA(HMGR, int, LPSTR, LPSTR) { return nullptr; }
extern "C" HWTHOOK WINAPI WTMgrPacketHookExW(HMGR, int, LPWSTR, LPSTR) { return nullptr; }
extern "C" BOOL WINAPI WTMgrPacketUnhook(HWTHOOK) { return FALSE; }
extern "C" LRESULT WINAPI WTMgrPacketHookNext(HWTHOOK, int, WPARAM, LPARAM) { return 0; }
extern "C" BOOL WINAPI WTMgrCsrPressureBtnMarksEx(HMGR, UINT, UINT*, UINT*) { return FALSE; }
