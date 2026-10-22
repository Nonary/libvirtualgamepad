cmake_minimum_required(VERSION 3.20)
project(LibVirtualGamepadTests LANGUAGES CXX)
enable_testing()
add_executable(test_ds4_usb test_ds4_usb.cpp)
target_compile_features(test_ds4_usb PRIVATE cxx_std_20)
target_include_directories(test_ds4_usb PRIVATE ../../include)
add_test(NAME ds4_usb_contract COMMAND test_ds4_usb)

# Compile the firmware initializer from the shipping kernel source into the
# portable contract test, so either transport drifting independently fails.
set(_ds5_gadget_source "${CMAKE_CURRENT_LIST_DIR}/../../linux/vibeshine-ds5/vibeshine_ds5_gadget.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ds5_gadget_source}")
file(READ "${_ds5_gadget_source}" _ds5_gadget_text)
string(REGEX MATCH "static const u8 ds5_firmware\\[\\] = \\{[^}]*\\};" _ds5_gadget_firmware "${_ds5_gadget_text}")
if(NOT _ds5_gadget_firmware)
    message(FATAL_ERROR "The Linux DualSense firmware initializer is missing")
endif()
string(REPLACE "static const u8" "inline constexpr std::uint8_t" _ds5_gadget_firmware "${_ds5_gadget_firmware}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ds5_gadget_firmware.h"
    "#pragma once\n#include <cstdint>\nnamespace kernel_fixture {\n${_ds5_gadget_firmware}\n}\n")
add_executable(test_ds5_usb test_ds5_usb.cpp)
target_include_directories(test_ds5_usb PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(test_ds5_usb PRIVATE cxx_std_20)
target_include_directories(test_ds5_usb PRIVATE ../../include)
add_test(NAME ds5_usb_contract COMMAND test_ds5_usb)

add_executable(test_pid_descriptor test_pid_descriptor.cpp
    ../src/pid_ff.cpp ../src/profile.cpp ../src/report_pump.cpp
    ../src/dualshock4.cpp ../src/dualsense.cpp ../src/switch_pro.cpp
    ../src/xbox_one.cpp ../src/xbox_series.cpp)
target_compile_features(test_pid_descriptor PRIVATE cxx_std_20)
target_include_directories(test_pid_descriptor PRIVATE ../src ../../include)
add_test(NAME controller_descriptors COMMAND test_pid_descriptor)

if(WIN32)
    # Manual, opt-in only: this probe suspends the machine. Never a CTest.
    add_executable(probe_sleep probe_sleep.cpp ../../client/client.cpp)
    target_compile_features(probe_sleep PRIVATE cxx_std_20)
    target_include_directories(probe_sleep PRIVATE ../../include)
    target_link_libraries(probe_sleep PRIVATE setupapi hid cfgmgr32 powrprof wtsapi32 xinput)
    add_executable(probe_ds4_usb probe_ds4_usb.cpp ../../client/client.cpp)
    target_compile_features(probe_ds4_usb PRIVATE cxx_std_20)
    target_include_directories(probe_ds4_usb PRIVATE ../../include)
    target_link_libraries(probe_ds4_usb PRIVATE setupapi hid)
    add_executable(probe_ds5_usb probe_ds5_usb.cpp ../../client/client.cpp)
    target_compile_features(probe_ds5_usb PRIVATE cxx_std_20)
    target_include_directories(probe_ds5_usb PRIVATE ../../include)
    target_link_libraries(probe_ds5_usb PRIVATE setupapi hid)
    # Deliberately not a CTest: creates a controller on the installed driver.
endif()

# Include native PnP identity checks in the existing CI test entry point.
add_subdirectory(gameinput)

# Compile the shipping lifecycle and callback bodies against deterministic,
# reentrant WDF/VHF boundaries. No driver is loaded by this contract test.
find_package(Threads REQUIRED)
add_executable(test_vhf_lifecycle test_vhf_lifecycle.cpp
    ../src/pid_ff.cpp ../src/profile.cpp ../src/report_pump.cpp
    ../src/dualshock4.cpp ../src/dualsense.cpp ../src/switch_pro.cpp
    ../src/xbox_one.cpp ../src/xbox_series.cpp)
target_compile_features(test_vhf_lifecycle PRIVATE cxx_std_20)
target_include_directories(test_vhf_lifecycle BEFORE PRIVATE support/vhf ../src ../../include)
target_link_libraries(test_vhf_lifecycle PRIVATE Threads::Threads)
add_test(NAME vhf_lifecycle_contract COMMAND test_vhf_lifecycle)
add_test(NAME vhf_initialization_behavior COMMAND test_vhf_lifecycle --initialization-only)
set_tests_properties(vhf_lifecycle_contract vhf_initialization_behavior PROPERTIES TIMEOUT 30)

if(NOT WIN32)
    # These protocol-only tests need the fixed Windows ABI declarations, never
    # an installed Windows SDK. Keep the stubs out of native driver/probe builds.
    target_include_directories(test_pid_descriptor PRIVATE support/vhf)
    target_include_directories(test_profile_identity PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support/vhf")
endif()

# Compile the actual manual-probe control flow against inert boundaries. This
# target cannot invoke Windows power APIs, even on Windows CI.
set(_sleep_probe_source "${CMAKE_CURRENT_LIST_DIR}/probe_sleep.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sleep_probe_source}")
file(READ "${_sleep_probe_source}" _sleep_probe_text)
string(FIND "${_sleep_probe_text}" "int main(" _sleep_probe_main_offset)
if(_sleep_probe_main_offset LESS 0)
    message(FATAL_ERROR "Manual sleep probe main entry point is missing")
endif()
string(SUBSTRING "${_sleep_probe_text}" ${_sleep_probe_main_offset} -1 _sleep_probe_main)
string(REPLACE "int main(" "int probe_main(" _sleep_probe_main "${_sleep_probe_main}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sleep_probe_main.h" "${_sleep_probe_main}")
add_executable(test_sleep_probe_cleanup test_sleep_probe_cleanup.cpp)
target_compile_features(test_sleep_probe_cleanup PRIVATE cxx_std_20)
target_include_directories(test_sleep_probe_cleanup PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(test_sleep_probe_cleanup PRIVATE Threads::Threads)
add_test(NAME sleep_probe_cleanup_contract COMMAND test_sleep_probe_cleanup)
set_tests_properties(sleep_probe_cleanup_contract PROPERTIES TIMEOUT 10)
# Exercise shipping touch allocation and encoded reports, including ID wrap.
add_executable(test_playstation_touch test_playstation_touch.cpp
    ../src/dualshock4.cpp ../src/dualsense.cpp)
target_compile_features(test_playstation_touch PRIVATE cxx_std_20)
target_include_directories(test_playstation_touch PRIVATE ../src ../../include)
if(NOT WIN32)
    target_include_directories(test_playstation_touch PRIVATE support/vhf)
endif()
add_test(NAME playstation_touch_contract COMMAND test_playstation_touch)
