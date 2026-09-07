cmake_minimum_required(VERSION 3.20)
project(robotweax_srt_package_consumer LANGUAGES C CXX)

include(CTest)
if(NOT DEFINED ROBOTWEAX_SRT_PACKAGE_VERSION
        OR ROBOTWEAX_SRT_PACKAGE_VERSION STREQUAL "")
    message(FATAL_ERROR "ROBOTWEAX_SRT_PACKAGE_VERSION is required")
endif()
find_package(RobotweaxSRT ${ROBOTWEAX_SRT_PACKAGE_VERSION} CONFIG REQUIRED)

if(ROBOTWEAX_SRT_EXPECT_AEAD_API_PREVIEW)
    add_compile_definitions(ROBOTWEAX_SRT_EXPECT_AEAD_API_PREVIEW=1)
endif()

add_executable(robotweax_srt_c_consumer c_consumer.c)
set_target_properties(robotweax_srt_c_consumer
    PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(robotweax_srt_c_consumer PRIVATE c_std_11)
target_link_libraries(robotweax_srt_c_consumer PRIVATE RobotweaxSRT::srt)

add_executable(robotweax_srt_native_c_consumer native_c_consumer.c)
set_target_properties(robotweax_srt_native_c_consumer
    PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(robotweax_srt_native_c_consumer PRIVATE c_std_11)
target_link_libraries(
    robotweax_srt_native_c_consumer PRIVATE RobotweaxSRT::srt)

add_executable(robotweax_srt_cpp_consumer cpp_consumer.cpp)
target_compile_features(robotweax_srt_cpp_consumer PRIVATE cxx_std_20)
target_link_libraries(robotweax_srt_cpp_consumer PRIVATE RobotweaxSRT::srt)

add_test(NAME robotweax_srt_c_consumer COMMAND robotweax_srt_c_consumer)
add_test(NAME robotweax_srt_native_c_consumer
    COMMAND robotweax_srt_native_c_consumer)
add_test(NAME robotweax_srt_cpp_consumer COMMAND robotweax_srt_cpp_consumer)
