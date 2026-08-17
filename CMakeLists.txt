cmake_minimum_required(VERSION 3.21)
project(startup_manager VERSION 0.1.0 LANGUAGES CXX)

add_library(startup-manager-domain INTERFACE)
target_include_directories(startup-manager-domain INTERFACE src)
target_compile_features(startup-manager-domain INTERFACE cxx_std_20)

enable_testing()
add_executable(startup-manager-tests tests/startup_entry_test.cpp)
target_link_libraries(startup-manager-tests PRIVATE startup-manager-domain)
target_compile_definitions(startup-manager-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
  target_compile_options(startup-manager-tests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
endif()
add_test(NAME startup-manager.domain COMMAND startup-manager-tests)

