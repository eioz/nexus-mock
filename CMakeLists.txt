cmake_minimum_required(VERSION 3.25)

project(nexus_mock_mac_dev LANGUAGES CXX OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "This CMake entrypoint is only for the native macOS mock. Use build.ps1 or the Visual Studio solution for Windows.")
endif()

set(NEXUS_ADDON_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "Path to the Nexus Log Uploader addon repository")
get_filename_component(NEXUS_ADDON_ROOT "${NEXUS_ADDON_ROOT}" ABSOLUTE)

if(NOT EXISTS "${NEXUS_ADDON_ROOT}/log_uploader/addon.h")
    message(FATAL_ERROR "NEXUS_ADDON_ROOT must point to the nexus-log-uploader repository")
endif()

find_package(miniz CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(nexus_log_uploader_mac_mock
    imgui/backends/imgui_impl_metal.mm
    imgui/backends/imgui_impl_osx.mm
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    mac/main.mm
    mac/mock_api.cpp
    mac/platform_macos.mm
    mac/log_uploader/directory_monitor.cpp
    mac/log_uploader/dps_report_uploader.cpp
    mac/log_uploader/parser.cpp
    mac/log_uploader/wingman_uploader.cpp
    "${NEXUS_ADDON_ROOT}/log_uploader/addon.cpp"
    "${NEXUS_ADDON_ROOT}/log_uploader/evtc_parser.cpp"
    "${NEXUS_ADDON_ROOT}/log_uploader/log.cpp"
    "${NEXUS_ADDON_ROOT}/log_uploader/log_manager.cpp"
    "${NEXUS_ADDON_ROOT}/log_uploader/logs_table.cpp"
    "${NEXUS_ADDON_ROOT}/log_uploader/main.cpp"
    "${NEXUS_ADDON_ROOT}/log_uploader/settings.cpp"
    "${NEXUS_ADDON_ROOT}/log_uploader/ui.cpp"
    "${NEXUS_ADDON_ROOT}/log_uploader/ui_elements.cpp"
)

set_source_files_properties(imgui/backends/imgui_impl_metal.mm PROPERTIES
    COMPILE_OPTIONS "-Wno-deprecated-declarations"
)

target_compile_features(nexus_log_uploader_mac_mock PRIVATE cxx_std_23)
set_target_properties(nexus_log_uploader_mac_mock PROPERTIES
    CXX_EXTENSIONS OFF
    OBJCXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${NEXUS_ADDON_ROOT}/build/mac-mock-$<LOWER_CASE:$<CONFIG>>"
)

target_include_directories(nexus_log_uploader_mac_mock PRIVATE
    mac
    mac/include
    imgui
    imgui/backends
    mumble
    "${NEXUS_ADDON_ROOT}/log_uploader"
)

target_compile_definitions(nexus_log_uploader_mac_mock PRIVATE
    LOG_UPLOADER_MAC_DEV
    LOG_UPLOADER_SOURCE_DIR="${NEXUS_ADDON_ROOT}"
    NEXUS_MOCK_DATA_DIR="${NEXUS_ADDON_ROOT}/data"
)

target_compile_options(nexus_log_uploader_mac_mock PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-nontrivial-memcall>
    $<$<COMPILE_LANG_AND_ID:OBJCXX,AppleClang>:-Wno-nontrivial-memcall>
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)

target_link_libraries(nexus_log_uploader_mac_mock PRIVATE
    miniz::miniz
    nlohmann_json::nlohmann_json
    "-framework AppKit"
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
)
