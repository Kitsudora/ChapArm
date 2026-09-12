#pragma once
#include <windows.h>
#include <cstdint>

// Publisher coordinates cover the Windows virtual desktop, top-left origin.
// Angles are degrees; pressure is actual normalized contact force, not intent.
struct ChapArmSample {
    std::uint32_t size;
    std::uint32_t version;
    float x, y, pressure;
    float azimuth_deg, altitude_deg, twist_deg;
    std::uint32_t buttons;
    std::uint32_t proximity;
    std::uint64_t timestamp_ms;
};
static_assert(sizeof(ChapArmSample) == 48, "Publisher ABI must remain stable");

struct ChapArmStatus {
    std::uint32_t size;
    std::uint32_t version;
    std::uint32_t publisher_pid;
    std::uint32_t consumer_pid;
    std::uint32_t context_count;
    std::uint32_t enabled_context_count;
    std::uint64_t last_publish_tick_ms;
    std::uint64_t last_consumer_tick_ms;
    std::uint64_t published_samples;
};
static_assert(sizeof(ChapArmStatus) == 48, "Status ABI must remain stable");

extern "C" {
HANDLE WINAPI ChapArmOpenPublisher(const wchar_t* session);
BOOL WINAPI ChapArmPublish(HANDLE publisher, const ChapArmSample* sample);
BOOL WINAPI ChapArmClosePublisher(HANDLE publisher);
BOOL WINAPI ChapArmGetStatus(const wchar_t* session, ChapArmStatus* status);
}
