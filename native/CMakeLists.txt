cmake_minimum_required(VERSION 3.20)
project(ChapArmWintab LANGUAGES CXX)
if(NOT WIN32)
  message(FATAL_ERROR "The Wintab provider requires Windows and the Windows SDK")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The third column is the x86 stdcall parameter byte count. Export names and
# official ordinals are unchanged on both architectures.
set(exports
  WTInfoA,20,12 WTInfoW,1020,12 WTOpenA,21,12 WTOpenW,1021,12
  WTClose,22,4 WTPacketsGet,23,12 WTPacket,24,12 WTEnable,40,8 WTOverlap,41,8
  WTConfig,60,8 WTGetA,61,8 WTGetW,1061,8 WTSetA,62,8 WTSetW,1062,8
  WTExtGet,63,12 WTExtSet,64,12 WTSave,65,8 WTRestore,66,12
  WTPacketsPeek,80,12 WTDataGet,81,24 WTDataPeek,82,24
  WTQueueSizeGet,84,4 WTQueueSizeSet,85,8 WTQueuePacketsEx,200,12
  WTMgrOpen,100,8 WTMgrClose,101,4 WTMgrContextEnum,120,12
  WTMgrContextOwner,121,8 WTMgrDefContext,122,8 WTMgrDefContextEx,206,12
  WTMgrDeviceConfig,140,12 WTMgrExt,180,12 WTMgrCsrEnable,181,12
  WTMgrCsrButtonMap,182,16 WTMgrCsrPressureBtnMarks,183,16
  WTMgrCsrPressureResponse,184,16 WTMgrCsrExt,185,16
  WTMgrConfigReplaceExA,202,16 WTMgrConfigReplaceExW,1202,16
  WTMgrPacketHookExA,203,16 WTMgrPacketHookExW,1203,16
  WTMgrPacketUnhook,204,4 WTMgrPacketHookNext,205,16
  WTMgrCsrPressureBtnMarksEx,201,16
  ChapArmOpenPublisher,3000,4 ChapArmPublish,3001,8
  ChapArmClosePublisher,3002,4 ChapArmGetStatus,3003,8)
set(def "LIBRARY Wintab32\nEXPORTS\n")
foreach(entry IN LISTS exports)
  string(REPLACE "," ";" parts "${entry}")
  list(GET parts 0 name)
  list(GET parts 1 ordinal)
  list(GET parts 2 bytes)
  if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    string(APPEND def "  ${name}=_${name}@${bytes} @${ordinal}\n")
  else()
    string(APPEND def "  ${name} @${ordinal}\n")
  endif()
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

include(CTest)
if(BUILD_TESTING)
  add_executable(wintab_probe tests/wintab_probe.cpp)
  target_link_libraries(wintab_probe PRIVATE Wintab32 user32)
  if(MINGW)
    target_link_options(wintab_probe PRIVATE -municode -static-libgcc -static-libstdc++)
  endif()
  add_test(NAME wintab_abi_and_stream COMMAND wintab_probe --selftest)
  set_tests_properties(wintab_abi_and_stream PROPERTIES TIMEOUT 20)
endif()
