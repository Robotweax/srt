cmake_minimum_required(VERSION 3.20)
project(crypto_provider_peer LANGUAGES C CXX)
if(DEFINED HAIVISION_SOURCE)
    set(ENABLE_APPS OFF CACHE BOOL "" FORCE)
    set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
    set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
    set(ENABLE_ENCRYPTION ON CACHE BOOL "" FORCE)
    add_subdirectory("${HAIVISION_SOURCE}" reference)
    set(PEER_LIBRARY srt_static)
else()
    find_package(RobotweaxSRT CONFIG REQUIRED)
    set(PEER_LIBRARY RobotweaxSRT::srt)
endif()
add_executable(crypto_peer ../../interop/api_peer.cpp)
target_compile_features(crypto_peer PRIVATE cxx_std_17)
target_link_libraries(crypto_peer PRIVATE ${PEER_LIBRARY})
if(WIN32)
    target_compile_definitions(crypto_peer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(crypto_peer PRIVATE ws2_32)
endif()
if(DEFINED HAIVISION_SOURCE)
    target_include_directories(crypto_peer PRIVATE "${HAIVISION_SOURCE}/srtcore" "${CMAKE_CURRENT_BINARY_DIR}/reference")
    if(WIN32)
        # Haivision logging_api.h includes win/syslog_defs.h on Windows.
        # This source-tree-only header lives under common, not srtcore.
        target_include_directories(crypto_peer PRIVATE "${HAIVISION_SOURCE}/common")
    endif()
else()
    # The DLL must live beside this peer, not be resolved through a shared PATH
    # containing two identically named Robotweax provider builds.
    add_custom_command(TARGET crypto_peer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:RobotweaxSRT::srt> $<TARGET_FILE_DIR:crypto_peer>)
endif()
