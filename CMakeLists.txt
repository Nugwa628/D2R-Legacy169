# legacy169 D2RLoader plugin. Build: tools/build.ps1 (or cmake -S . -B build -A x64 -DLEGACY169_PLUGINSDK_DIR=<sdk> &&
# cmake --build build --config Release). Output: build/Release/legacy169.dll.
cmake_minimum_required(VERSION 3.29)
project(legacy169 LANGUAGES CXX RC)

# The D2RLoader PluginSDK (https://github.com/D2RLoader/PluginSDK or a local checkout).
set(LEGACY169_PLUGINSDK_DIR "" CACHE PATH "D2RLoader PluginSDK source folder")
if(NOT EXISTS "${LEGACY169_PLUGINSDK_DIR}/CMakeLists.txt")
	message(FATAL_ERROR "Set LEGACY169_PLUGINSDK_DIR to a D2RLoader PluginSDK checkout")
endif()
set(D2RLPLUGIN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(${LEGACY169_PLUGINSDK_DIR} PluginSDK EXCLUDE_FROM_ALL)

add_library(legacy169 SHARED src/legacy169.cpp src/legacy169.rc)
target_link_libraries(legacy169 PRIVATE D2RLPlugin::D2RLPlugin)
# Test builds only: background input for unattended runs on plain D2R (src/testharness.cpp). Never on in a release build.
option(LEGACY169_TEST_HARNESS "Compile in the test harness (test builds only)" OFF)
if(LEGACY169_TEST_HARNESS)
	target_sources(legacy169 PRIVATE src/testharness.cpp)
	target_compile_definitions(legacy169 PRIVATE LEGACY169_TEST_HARNESS=1)
endif()
# Test builds only: one expected byte wrong, to check in the game that the plugin refuses everything and logs why.
option(LEGACY169_TEST_BAD_BYTES "Break one expected byte (test builds only)" OFF)
if(LEGACY169_TEST_BAD_BYTES)
	target_compile_definitions(legacy169 PRIVATE LEGACY169_TEST_BAD_BYTES=1)
endif()
target_compile_features(legacy169 PRIVATE cxx_std_20)
d2rlplugin_embed_config(TARGET legacy169 FILE "${CMAKE_CURRENT_SOURCE_DIR}/legacy169.toml")
if(MSVC)
	target_compile_options(legacy169 PRIVATE /W4 /permissive- /EHsc /utf-8)
	set_property(TARGET legacy169 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")  # no VC++ runtime dependency
endif()

# Unit tests of the patch table (no game, no Windows headers): build/tests/Release/legacy169_tests.exe
add_subdirectory(tests)
