cmake_minimum_required(VERSION 3.20)
project(ChapArmWintab LANGUAGES CXX)
if(NOT WIN32)
  message(FATAL_ERROR "The Wintab provider requires Windows and the Windows SDK")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Let the linker resolve the internal stdcall decoration on x86. Explicit
# name=_name@bytes aliases lose decorated symbols from MSVC's import library.
# Public export names and official ordinals are identical on both architectures.
set(exports
  WTInfoA,20 WTInfoW,1020 WTOpenA,21 WTOpenW,1021
  WTClose,22 WTPacketsGet,23 WTPacket,24 WTEnable,40 WTOverlap,41
  WTConfig,60 WTGetA,61 WTGetW,1061 WTSetA,62 WTSetW,1062
  WTExtGet,63 WTExtSet,64 WTSave,65 WTRestore,66
  WTPacketsPeek,80 WTDataGet,81 WTDataPeek,82
  WTQueueSizeGet,84 WTQueueSizeSet,85 WTQueuePacketsEx,200
  WTMgrOpen,100 WTMgrClose,101 WTMgrContextEnum,120
  WTMgrContextOwner,121 WTMgrDefContext,122 WTMgrDefContextEx,206
  WTMgrDeviceConfig,140 WTMgrExt,180 WTMgrCsrEnable,181
  WTMgrCsrButtonMap,182 WTMgrCsrPressureBtnMarks,183
  WTMgrCsrPressureResponse,184 WTMgrCsrExt,185
  WTMgrConfigReplaceExA,202 WTMgrConfigReplaceExW,1202
  WTMgrPacketHookExA,203 WTMgrPacketHookExW,1203
  WTMgrPacketUnhook,204 WTMgrPacketHookNext,205
  WTMgrCsrPressureBtnMarksEx,201
  ChapArmOpenPublisher,3000 ChapArmPublish,3001
  ChapArmClosePublisher,3002 ChapArmGetStatus,3003)
set(def "LIBRARY Wintab32\nEXPORTS\n")
foreach(entry IN LISTS exports)
  string(REPLACE "," ";" parts "${entry}")
  list(GET parts 0 name)
  list(GET parts 1 ordinal)
  string(APPEND def "  ${name} @${ordinal}\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wintab.def" "${def}")
add_library(Wintab32 SHARED src/provider.cpp "${CMAKE_CURRENT_BINARY_DIR}/wintab.def")
target_include_directories(Wintab32 PUBLIC include)
target_compile_definitions(Wintab32 PUBLIC WIN32 NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(Wintab32 PRIVATE user32)
set_target_properties(Wintab32 PROPERTIES PREFIX "")
if(MSVC)
  target_compile_options(Wintab32 PRIVATE /W4 /permissive-)
else()
  target_compile_options(Wintab32 PRIVATE -Wall -Wextra -Wpedantic)
  target_link_options(Wintab32 PRIVATE -static-libgcc -static-libstdc++)
endif()

add_executable(chaparm-krita WIN32 src/krita_launcher.cpp)
target_compile_definitions(chaparm-krita PRIVATE WIN32 NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(chaparm-krita PRIVATE user32)
if(MSVC)
  target_compile_options(chaparm-krita PRIVATE /W4 /permissive-)
  # DotLocal redirection is ignored for manifested executables unless a
  # machine-wide registry override is enabled. This launcher needs no override.
  # Use the original narrow CRT entry point so Qt can recognize untouched argv
  # and recover the complete Unicode command line through the Windows API.
  target_link_options(chaparm-krita PRIVATE /MANIFEST:NO /ENTRY:mainCRTStartup)
elseif(MINGW)
  target_compile_options(chaparm-krita PRIVATE -Wall -Wextra -Wpedantic)
  target_link_options(chaparm-krita PRIVATE -static-libgcc -static-libstdc++)
endif()

include(CTest)
if(BUILD_TESTING)
  # Reuse the native probe source for a small, test-only C entry point. No Krita
  # installation or third-party binaries are needed by the automated loader gate.
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/krita_fixture.def" "EXPORTS\n  krita_main\n")
  add_library(krita_launcher_fixture SHARED tests/wintab_probe.cpp "${CMAKE_CURRENT_BINARY_DIR}/krita_fixture.def")
  target_compile_definitions(krita_launcher_fixture PRIVATE CHAPARM_KRITA_FIXTURE WIN32 NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(krita_launcher_fixture PRIVATE shell32)
  set_target_properties(krita_launcher_fixture PROPERTIES PREFIX "")
  add_executable(wintab_probe tests/wintab_probe.cpp)
  target_link_libraries(wintab_probe PRIVATE Wintab32 user32)
  add_dependencies(wintab_probe chaparm-krita krita_launcher_fixture)
  if(MINGW)
    target_link_options(krita_launcher_fixture PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(wintab_probe PRIVATE -municode -static-libgcc -static-libstdc++)
  endif()
  add_test(NAME wintab_abi_and_stream COMMAND wintab_probe --selftest)
  set_tests_properties(wintab_abi_and_stream PROPERTIES TIMEOUT 20)
endif()
