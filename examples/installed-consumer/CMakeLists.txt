cmake_minimum_required(VERSION 3.20)

project(robotweax_srt_installed_consumer LANGUAGES CXX)

include(CTest)

find_package(RobotweaxSRT 0.2 CONFIG REQUIRED)

add_executable(robotweax_srt_installed_consumer main.cpp)
target_compile_features(robotweax_srt_installed_consumer PRIVATE cxx_std_17)
set_target_properties(robotweax_srt_installed_consumer PROPERTIES
    CXX_EXTENSIONS OFF)
target_link_libraries(robotweax_srt_installed_consumer
    PRIVATE RobotweaxSRT::srt)

if(BUILD_TESTING)
    add_test(NAME robotweax_srt_installed_consumer
        COMMAND robotweax_srt_installed_consumer)
endif()
