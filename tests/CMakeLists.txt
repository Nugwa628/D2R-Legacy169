# Unit tests of src/patch_table.h. Also buildable on its own: cmake -S tests -B build/tests
cmake_minimum_required(VERSION 3.20)
project(legacy169_tests LANGUAGES CXX)
add_executable(legacy169_tests patch_table_test.cpp)
target_compile_features(legacy169_tests PRIVATE cxx_std_20)
if(MSVC)
	target_compile_options(legacy169_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
	target_compile_definitions(legacy169_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
