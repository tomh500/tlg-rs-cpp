cmake_minimum_required(VERSION 3.24)
project(tlg_cpp VERSION 1.0.0 LANGUAGES CXX)

option(TLG_BUILD_TOOLS "Build the raw pixel command-line tool" ON)
option(TLG_BUILD_BENCHMARKS "Build reproducible codec benchmarks" OFF)
option(TLG_ENABLE_SANITIZERS "Enable address sanitizer (and UB sanitizer on Clang/GCC)" OFF)
option(TLG_ENABLE_SIMD "Enable baseline SSE2 acceleration on supported targets" ON)
include(CTest)
include(GNUInstallDirs)

add_library(tlg STATIC src/tlg.cpp src/tlg5.cpp src/tlg6.cpp src/slide.cpp)
add_library(tlg::tlg ALIAS tlg)
target_include_directories(tlg PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(tlg PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT TLG_ENABLE_SIMD)
    target_compile_definitions(tlg PRIVATE TLG_FORCE_SCALAR)
endif()
if(MSVC)
    # Explicitly use the newest mode supplied by the installed MSVC toolchain.
    # Do not also set CXX_STANDARD: that could append a conflicting /std option.
    target_compile_options(tlg PUBLIC /std:c++latest /permissive- /Zc:__cplusplus /utf-8)
    target_compile_options(tlg PRIVATE /W4 /EHsc)
    if(TLG_ENABLE_SANITIZERS)
        # MSVC's Debug preset enables /RTC1; runtime checks conflict with ASan.
        string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        target_compile_options(tlg PUBLIC /fsanitize=address)
        target_link_options(tlg PUBLIC /INCREMENTAL:NO)
    endif()
else()
    target_compile_features(tlg PUBLIC cxx_std_23)
    target_compile_options(tlg PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(TLG_ENABLE_SANITIZERS)
        target_compile_options(tlg PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(tlg PUBLIC -fsanitize=address,undefined)
    endif()
endif()

if(TLG_BUILD_TOOLS)
    add_executable(tlg_tool tools/tlg_tool.cpp)
    target_link_libraries(tlg_tool PRIVATE tlg::tlg)
endif()

if(BUILD_TESTING)
    add_executable(tlg_tests tests/tests.cpp)
    target_link_libraries(tlg_tests PRIVATE tlg::tlg)
    target_compile_definitions(tlg_tests PRIVATE TLG_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_test(NAME tlg_tests COMMAND tlg_tests)
    add_executable(tlg_slide_tests tests/slide_tests.cpp)
    target_link_libraries(tlg_slide_tests PRIVATE tlg::tlg)
    target_include_directories(tlg_slide_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME tlg_slide_tests COMMAND tlg_slide_tests)
    add_executable(tlg_allocation_tests tests/allocation_tests.cpp)
    target_link_libraries(tlg_allocation_tests PRIVATE tlg::tlg)
    target_include_directories(tlg_allocation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME tlg_allocation_tests COMMAND tlg_allocation_tests)
    if(TLG_BUILD_TOOLS)
        add_test(NAME tlg_tool_help COMMAND tlg_tool --help)
        add_test(NAME tlg_tool_roundtrip COMMAND ${CMAKE_COMMAND}
            -DTOOL=$<TARGET_FILE:tlg_tool>
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli-test
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.cmake)
    endif()
endif()

if(TLG_BUILD_BENCHMARKS)
    add_executable(tlg_benchmark bench/benchmark.cpp)
    target_link_libraries(tlg_benchmark PRIVATE tlg::tlg)
endif()

install(TARGETS tlg EXPORT tlgTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT tlgTargets NAMESPACE tlg:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tlg)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/tlgConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/tlgConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tlg)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/tlgConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/tlgConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/tlgConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tlg)
