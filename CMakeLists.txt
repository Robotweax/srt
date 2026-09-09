cmake_minimum_required(VERSION 3.20)
project(robotweax_srt VERSION 0.2.3 LANGUAGES C CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(ROBOTWEAX_SRT_ABI_VERSION
    "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
set(ROBOTWEAX_SRT_COMPATIBLE_SRT_VERSION "1.5.7")
set(ROBOTWEAX_SRT_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/RobotweaxSRT"
    CACHE STRING "Install directory for the Robotweax SRT CMake package")

option(ROBOTWEAX_SRT_BUILD_TESTS "Build unit tests" ON)
set(ROBOTWEAX_SRT_NATIVE_PROGRAMS_DEFAULT ON)
if(BUILD_SHARED_LIBS)
    set(ROBOTWEAX_SRT_NATIVE_PROGRAMS_DEFAULT OFF)
endif()
option(ROBOTWEAX_SRT_BUILD_BENCHMARKS "Build microbenchmarks"
    ${ROBOTWEAX_SRT_NATIVE_PROGRAMS_DEFAULT})
option(ROBOTWEAX_SRT_BUILD_FUZZERS "Build libFuzzer targets" OFF)
option(ROBOTWEAX_SRT_BUILD_TOOLS "Build protocol development tools"
    ${ROBOTWEAX_SRT_NATIVE_PROGRAMS_DEFAULT})
option(ROBOTWEAX_SRT_BUILD_EXAMPLES "Build public API examples" OFF)
option(ROBOTWEAX_SRT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(ROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT
    "Install opt-in srt.pc metadata for libsrt-compatible consumers" OFF)
option(ENABLE_AEAD_API_PREVIEW
    "Enable the version-pinned AES-GCM extension API" OFF)

if(ROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT AND NOT BUILD_SHARED_LIBS)
    message(FATAL_ERROR
        "ROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT currently requires "
        "BUILD_SHARED_LIBS=ON. FFmpeg's C-only configure probe cannot link "
        "the Robotweax static C++ archive without toolchain-specific C++ "
        "runtime flags.")
endif()

if(BUILD_SHARED_LIBS AND
        (ROBOTWEAX_SRT_BUILD_BENCHMARKS OR ROBOTWEAX_SRT_BUILD_TOOLS))
    message(FATAL_ERROR
        "Native C++ benchmarks and protocol tools use private implementation "
        "symbols. Build them in a separate static development tree; the "
        "shared artifact deliberately exports only the public C ABI.")
endif()

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_library(robotweax_srt
    src/compat/caller_handshake_events.cpp
    src/compat/caller_handshake_exchange.cpp
    src/compat/caller_handshake_sources.cpp
    src/compat/caller_handshake_steps.cpp
    src/compat/connection.cpp
    src/compat/epoll.cpp
    src/compat/error_state.cpp
    src/compat/file_io.cpp
    src/compat/group_api.cpp
    src/compat/group_config.cpp
    src/compat/group_replay_buffer.cpp
    src/compat/group_registry.cpp
    src/compat/listener_accept_queue.cpp
    src/compat/listener_connection_setup.cpp
    src/compat/listener_connection_setup_sources.cpp
    src/compat/listener_handshake_router.cpp
    src/compat/listener_handshake_sources.cpp
    src/compat/logging.cpp
    src/compat/message_io.cpp
    src/compat/platform_networking.cpp
    src/compat/process_state.cpp
    src/compat/readiness.cpp
    src/compat/runtime_scheduler.cpp
    src/compat/runtime_scheduler_service.cpp
    src/compat/runtime_work_executor.cpp
    src/compat/runtime_work_executor_service.cpp
    src/compat/socket_io.cpp
    src/compat/socket_options.cpp
    src/compat/socket_registry.cpp
    src/compat/statistics.cpp
    src/compat/srt_api.cpp
    src/compat/transport_runtime.cpp
    src/control.cpp
    src/connection_group.cpp
    src/codec.cpp
    src/crypto.cpp
    src/crypto_openssl.cpp
    src/fec.cpp
    src/file.cpp
    src/handshake.cpp
    src/handshake_datagram.cpp
    src/handshake_extensions.cpp
    src/live.cpp
    src/packet_filter.cpp
    src/receive_buffer.cpp
    src/reliability.cpp
    src/rendezvous.cpp
    src/rtt.cpp
    src/send_buffer.cpp
    src/session.cpp
    src/socket_options.cpp
    src/timing.cpp
    src/simulation.cpp
    src/udp.cpp
    src/c_api.cpp)
add_library(RobotweaxSRT::srt ALIAS robotweax_srt)

target_compile_definitions(robotweax_srt PRIVATE ROBOTWEAX_SRT_BUILDING)
if(ENABLE_AEAD_API_PREVIEW)
    target_compile_definitions(robotweax_srt
        PUBLIC ENABLE_AEAD_API_PREVIEW)
endif()
if(BUILD_SHARED_LIBS)
    target_compile_definitions(robotweax_srt
        PUBLIC ROBOTWEAX_SRT_SHARED SRT_DYNAMIC
        PRIVATE SRT_EXPORTS)
endif()

if(WIN32)
    target_compile_definitions(robotweax_srt
        PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(robotweax_srt PRIVATE ws2_32)
endif()
target_link_libraries(
    robotweax_srt PRIVATE OpenSSL::Crypto Threads::Threads)

target_compile_features(robotweax_srt PUBLIC cxx_std_20)
target_include_directories(robotweax_srt
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(ROBOTWEAX_SRT_PUBLIC_SYMBOLS_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/public-symbols.txt")
set(ROBOTWEAX_SRT_ABI_BASELINE_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/abi/robotweax-srt-${PROJECT_VERSION}.txt")
file(STRINGS
    "${ROBOTWEAX_SRT_PUBLIC_SYMBOLS_FILE}"
    ROBOTWEAX_SRT_PUBLIC_SYMBOLS
    REGEX "^[A-Za-z_][A-Za-z0-9_]*$")

set(ROBOTWEAX_SRT_ELF_EXPORTS "{\n  global:\n")
set(ROBOTWEAX_SRT_MACHO_EXPORTS "")
foreach(ROBOTWEAX_SRT_PUBLIC_SYMBOL IN LISTS ROBOTWEAX_SRT_PUBLIC_SYMBOLS)
    string(APPEND ROBOTWEAX_SRT_ELF_EXPORTS
        "    ${ROBOTWEAX_SRT_PUBLIC_SYMBOL};\n")
    string(APPEND ROBOTWEAX_SRT_MACHO_EXPORTS
        "_${ROBOTWEAX_SRT_PUBLIC_SYMBOL}\n")
endforeach()
string(APPEND ROBOTWEAX_SRT_ELF_EXPORTS "  local:\n    *;\n};\n")

set(ROBOTWEAX_SRT_EXPORTS_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/generated/exports")
file(MAKE_DIRECTORY "${ROBOTWEAX_SRT_EXPORTS_DIRECTORY}")
file(WRITE
    "${ROBOTWEAX_SRT_EXPORTS_DIRECTORY}/robotweax-srt.map"
    "${ROBOTWEAX_SRT_ELF_EXPORTS}")
file(WRITE
    "${ROBOTWEAX_SRT_EXPORTS_DIRECTORY}/robotweax-srt.exports"
    "${ROBOTWEAX_SRT_MACHO_EXPORTS}")

if(BUILD_SHARED_LIBS)
    if(WIN32)
        # Windows exports are opt-in through the public API headers'
        # __declspec(dllexport) declarations. The manifest test below proves
        # that the resulting PE export table remains exact.
        set_target_properties(robotweax_srt PROPERTIES
            WINDOWS_EXPORT_ALL_SYMBOLS OFF)
    elseif(APPLE)
        target_link_options(robotweax_srt PRIVATE
            "LINKER:-exported_symbols_list,${ROBOTWEAX_SRT_EXPORTS_DIRECTORY}/robotweax-srt.exports")
    elseif(UNIX)
        target_link_options(robotweax_srt PRIVATE
            "LINKER:--version-script=${ROBOTWEAX_SRT_EXPORTS_DIRECTORY}/robotweax-srt.map")
    endif()
endif()

set_target_properties(robotweax_srt PROPERTIES
    EXPORT_NAME srt
    OUTPUT_NAME srt
    VERSION ${PROJECT_VERSION}
    SOVERSION ${ROBOTWEAX_SRT_ABI_VERSION}
    CXX_EXTENSIONS OFF
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)

if(ROBOTWEAX_SRT_BUILD_EXAMPLES)
    add_executable(robotweax_srt_udp_bridge examples/srt_udp_bridge.cpp)
    target_link_libraries(robotweax_srt_udp_bridge PRIVATE RobotweaxSRT::srt)
    if(WIN32)
        target_link_libraries(robotweax_srt_udp_bridge PRIVATE ws2_32)
    endif()
    target_compile_features(robotweax_srt_udp_bridge PRIVATE cxx_std_20)
    set_target_properties(robotweax_srt_udp_bridge PROPERTIES CXX_EXTENSIONS OFF)

    add_executable(robotweax_srt_message_demo
        examples/srt_message_demo.cpp)
    target_link_libraries(robotweax_srt_message_demo
        PRIVATE RobotweaxSRT::srt)
    target_compile_features(robotweax_srt_message_demo PRIVATE cxx_std_20)
    set_target_properties(robotweax_srt_message_demo PROPERTIES
        CXX_EXTENSIONS OFF)

    add_executable(robotweax_srt_file_demo
        examples/srt_file_demo.cpp)
    target_link_libraries(robotweax_srt_file_demo
        PRIVATE RobotweaxSRT::srt)
    target_compile_features(robotweax_srt_file_demo PRIVATE cxx_std_20)
    set_target_properties(robotweax_srt_file_demo PROPERTIES
        CXX_EXTENSIONS OFF)

    add_executable(robotweax_srt_group_demo
        examples/srt_group_demo.cpp)
    target_link_libraries(robotweax_srt_group_demo
        PRIVATE RobotweaxSRT::srt)
    target_compile_features(robotweax_srt_group_demo PRIVATE cxx_std_20)
    set_target_properties(robotweax_srt_group_demo PROPERTIES
        CXX_EXTENSIONS OFF)
endif()

if(MSVC)
    target_compile_options(robotweax_srt PRIVATE /W4 /permissive-)
    if(ROBOTWEAX_SRT_WARNINGS_AS_ERRORS)
        target_compile_options(robotweax_srt PRIVATE /WX)
    endif()
else()
    if(BUILD_SHARED_LIBS AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # STB_GNU_UNIQUE symbols make glibc mark a DSO NODELETE, preventing
        # real dlclose/dlopen runtime generations.
        target_compile_options(robotweax_srt PRIVATE -fno-gnu-unique)
    endif()
    target_compile_options(robotweax_srt PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        -Wshadow -Wold-style-cast -Wnull-dereference)
    if(ROBOTWEAX_SRT_WARNINGS_AS_ERRORS)
        target_compile_options(robotweax_srt PRIVATE -Werror)
    endif()
endif()

include(CTest)
if(ROBOTWEAX_SRT_BUILD_TESTS)
    # Explicit CI-only negative probe: uses this build's compiler and global
    # flags, but is not built by default, installed, or registered with CTest.
    add_executable(robotweax_srt_ubsan_canary EXCLUDE_FROM_ALL
        tests/sanitizers/ubsan_canary.cpp)
    if(NOT BUILD_SHARED_LIBS)
    add_executable(robotweax_srt_tests
        tests/test_main.cpp
        tests/test_caller_handshake_events.cpp
        tests/test_caller_handshake_exchange.cpp
        tests/test_caller_handshake_sources.cpp
        tests/test_caller_handshake_steps.cpp
        tests/test_sequence.cpp
        tests/test_control.cpp
        tests/test_connection_group.cpp
        tests/test_connect_variants.cpp
        tests/test_crypto.cpp
        tests/test_fec.cpp
        tests/test_file.cpp
        tests/test_file_io.cpp
        tests/test_group_replay_buffer.cpp
        tests/test_codec.cpp
        tests/test_handshake.cpp
        tests/test_handshake_datagram.cpp
        tests/test_handshake_extensions.cpp
        tests/test_listener_accept_queue.cpp
        tests/test_listener_connection_setup.cpp
        tests/test_listener_connection_setup_sources.cpp
        tests/test_listener_handshake_router.cpp
        tests/test_listener_handshake_sources.cpp
        tests/test_live.cpp
        tests/test_logging.cpp
        tests/test_packet_filter.cpp
        tests/test_receive_buffer.cpp
        tests/test_reliability.cpp
        tests/test_rendezvous.cpp
        tests/test_runtime_scheduler.cpp
        tests/test_runtime_work_executor.cpp
        tests/test_send_buffer.cpp
        tests/test_session.cpp
        tests/test_srt_compat.cpp
        tests/test_socket_options.cpp
        tests/test_simulation.cpp
        tests/test_statistics.cpp
        tests/test_timing.cpp
        tests/test_udp.cpp
        tests/test_compat_epoll.cpp
        tests/test_compat_groups.cpp
        tests/test_compat_runtime.cpp)
    target_link_libraries(robotweax_srt_tests
        PRIVATE RobotweaxSRT::srt Threads::Threads)
    target_include_directories(robotweax_srt_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(robotweax_srt_tests PRIVATE cxx_std_20)
    add_test(NAME robotweax_srt_tests COMMAND robotweax_srt_tests)
    # The unfiltered run already includes every compatibility case. Keep the
    # binary's optional filter for diagnosis, not a second CTest invocation.
    set_tests_properties(
        robotweax_srt_tests
        PROPERTIES TIMEOUT 30)

    add_executable(robotweax_srt_scheduler_tests
        tests/test_main.cpp
        tests/test_caller_handshake_events.cpp
        tests/test_caller_handshake_sources.cpp
        tests/test_listener_connection_setup_sources.cpp
        tests/test_listener_handshake_sources.cpp
        tests/test_runtime_scheduler.cpp
        tests/test_runtime_work_executor.cpp)
    target_link_libraries(robotweax_srt_scheduler_tests
        PRIVATE RobotweaxSRT::srt Threads::Threads)
    target_include_directories(robotweax_srt_scheduler_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(
        robotweax_srt_scheduler_tests PRIVATE cxx_std_20)

    # Lifecycle tests intentionally manipulate process-wide startup and
    # cleanup state. Keep them in a separate process so their reference-count
    # assertions cannot be affected by other compatibility tests.
    add_executable(robotweax_srt_lifecycle_tests
        tests/test_main.cpp
        tests/test_lifecycle.cpp)
    target_link_libraries(robotweax_srt_lifecycle_tests
        PRIVATE RobotweaxSRT::srt Threads::Threads)
    target_include_directories(robotweax_srt_lifecycle_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(robotweax_srt_lifecycle_tests PRIVATE cxx_std_20)
    add_test(
        NAME robotweax_srt_lifecycle_tests
        COMMAND robotweax_srt_lifecycle_tests)
    set_tests_properties(
        robotweax_srt_lifecycle_tests
        PROPERTIES TIMEOUT 30)

    add_executable(
        robotweax_srt_process_exit_tests
        tests/process_exit_probe.cpp)
    target_link_libraries(
        robotweax_srt_process_exit_tests
        PRIVATE RobotweaxSRT::srt Threads::Threads)
    target_include_directories(
        robotweax_srt_process_exit_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(
        robotweax_srt_process_exit_tests PRIVATE cxx_std_20)
    add_test(
        NAME robotweax_srt_process_exit_tests
        COMMAND robotweax_srt_process_exit_tests)
    set_tests_properties(
        robotweax_srt_process_exit_tests
        PROPERTIES TIMEOUT 5)
    endif()

    add_executable(
        robotweax_srt_global_lifecycle_tests
        tests/global_lifecycle_probe.cpp)
    target_link_libraries(
        robotweax_srt_global_lifecycle_tests
        PRIVATE RobotweaxSRT::srt)
    target_compile_features(
        robotweax_srt_global_lifecycle_tests PRIVATE cxx_std_20)
    add_test(
        NAME robotweax_srt_global_lifecycle_tests
        COMMAND robotweax_srt_global_lifecycle_tests)
    set_tests_properties(
        robotweax_srt_global_lifecycle_tests
        PROPERTIES TIMEOUT 5)

    add_executable(robotweax_srt_late_cleanup_tests
        tests/late_cleanup_main.cpp tests/late_cleanup_probe.cpp)
    target_link_libraries(robotweax_srt_late_cleanup_tests PRIVATE RobotweaxSRT::srt)
    target_compile_features(robotweax_srt_late_cleanup_tests PRIVATE cxx_std_20)
    set(ROBOTWEAX_SRT_LATE_CLEANUP_MODES
        socket startup-only retained epoll group restart explicit
        implicit-socket implicit-group implicit-epoll)
    foreach(mode IN LISTS ROBOTWEAX_SRT_LATE_CLEANUP_MODES)
        add_test(NAME robotweax_srt_late_cleanup_${mode}
            COMMAND robotweax_srt_late_cleanup_tests ${mode})
        set_tests_properties(robotweax_srt_late_cleanup_${mode} PROPERTIES TIMEOUT 10)
        if(BUILD_SHARED_LIBS)
            set_tests_properties(robotweax_srt_late_cleanup_${mode}
                PROPERTIES LABELS "shared-library")
        endif()
    endforeach()
    if(NOT BUILD_SHARED_LIBS)
        # Exercise a static libsrt embedded in a downstream shared library.
        set_property(TARGET robotweax_srt PROPERTY POSITION_INDEPENDENT_CODE ON)
        add_library(robotweax_srt_late_cleanup_wrapper SHARED tests/late_cleanup_probe.cpp)
        target_link_libraries(robotweax_srt_late_cleanup_wrapper PRIVATE RobotweaxSRT::srt)
        target_compile_features(robotweax_srt_late_cleanup_wrapper PRIVATE cxx_std_20)
        set_property(TARGET robotweax_srt_late_cleanup_wrapper PROPERTY WINDOWS_EXPORT_ALL_SYMBOLS ON)
        add_executable(robotweax_srt_embedded_cleanup_tests tests/late_cleanup_main.cpp)
        target_link_libraries(robotweax_srt_embedded_cleanup_tests PRIVATE robotweax_srt_late_cleanup_wrapper)
        foreach(mode IN LISTS ROBOTWEAX_SRT_LATE_CLEANUP_MODES)
            add_test(NAME robotweax_srt_embedded_cleanup_${mode}
                COMMAND robotweax_srt_embedded_cleanup_tests ${mode})
            set_tests_properties(robotweax_srt_embedded_cleanup_${mode} PROPERTIES TIMEOUT 10)
        endforeach()
    endif()

    if(BUILD_SHARED_LIBS)
        # This probe must not link against Robotweax SRT: it loads only the
        # produced shared object and resolves the compatible C ABI at runtime.
        add_executable(
            robotweax_srt_shared_lifecycle_tests
            tests/shared_library_lifecycle_probe.cpp)
        add_dependencies(
            robotweax_srt_shared_lifecycle_tests robotweax_srt)
        target_include_directories(
            robotweax_srt_shared_lifecycle_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_features(
            robotweax_srt_shared_lifecycle_tests PRIVATE cxx_std_20)
        if(CMAKE_DL_LIBS)
            target_link_libraries(
                robotweax_srt_shared_lifecycle_tests
                PRIVATE ${CMAKE_DL_LIBS})
        endif()
        add_test(
            NAME robotweax_srt_shared_lifecycle_tests
            COMMAND robotweax_srt_shared_lifecycle_tests
                $<TARGET_FILE:robotweax_srt>)
        set_tests_properties(
            robotweax_srt_shared_lifecycle_tests
            PROPERTIES TIMEOUT 30)
    endif()

    if(UNIX AND NOT BUILD_SHARED_LIBS AND NOT OPENSSL_USE_STATIC_LIBS
        AND NOT OPENSSL_CRYPTO_LIBRARY MATCHES "\\.a$")
        # RAND_bytes interposition is confined to this separate test process.
        # RTLD_NEXT requires dynamic libcrypto; static-provider builds still
        # exercise the portable CryptoSession failure-policy unit tests.
        add_executable(robotweax_srt_crypto_setup_failure_tests
            tests/crypto_setup_failure_probe.cpp)
        target_link_libraries(robotweax_srt_crypto_setup_failure_tests
            PRIVATE RobotweaxSRT::srt Threads::Threads ${CMAKE_DL_LIBS})
        target_include_directories(robotweax_srt_crypto_setup_failure_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_features(robotweax_srt_crypto_setup_failure_tests
            PRIVATE cxx_std_20)
        set(crypto_setup_modes 0)
        if(ENABLE_AEAD_API_PREVIEW)
            list(APPEND crypto_setup_modes 1)
        endif()
        foreach(crypto_mode IN LISTS crypto_setup_modes)
            foreach(rendezvous IN ITEMS 0 1)
                # Calls 1/2 generate initial material; 3/4 and 5/6 prepare
                # independent responder and initiator DATA directions.
                foreach(failure_index IN ITEMS 0 3 4 5 6)
                    set(probe_name robotweax_srt_crypto_setup_${crypto_mode}_${rendezvous}_${failure_index})
                    add_test(NAME ${probe_name}
                        COMMAND robotweax_srt_crypto_setup_failure_tests
                            ${rendezvous} ${failure_index} ${crypto_mode})
                    set_tests_properties(${probe_name}
                        PROPERTIES TIMEOUT 15 LABELS "encryption")
                endforeach()
            endforeach()
        endforeach()
        add_test(NAME robotweax_srt_crypto_setup_optional_fallback
            COMMAND robotweax_srt_crypto_setup_failure_tests 0 99 0)
        set_tests_properties(robotweax_srt_crypto_setup_optional_fallback
            PROPERTIES TIMEOUT 15 LABELS "encryption")
    endif()

    if(UNIX AND NOT BUILD_SHARED_LIBS)
        add_executable(
            robotweax_srt_fork_tests
            tests/fork_probe.cpp)
        target_link_libraries(
            robotweax_srt_fork_tests
            PRIVATE RobotweaxSRT::srt Threads::Threads)
        target_include_directories(
            robotweax_srt_fork_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_features(
            robotweax_srt_fork_tests PRIVATE cxx_std_20)
        add_test(
            NAME robotweax_srt_fork_tests
            COMMAND robotweax_srt_fork_tests)
        set_tests_properties(
            robotweax_srt_fork_tests
            PROPERTIES TIMEOUT 15)
    endif()

    add_executable(robotweax_srt_c_api_test tests/test_c_api.c)
    target_link_libraries(robotweax_srt_c_api_test PRIVATE RobotweaxSRT::srt)
    set_target_properties(robotweax_srt_c_api_test PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME robotweax_srt_c_api_test COMMAND robotweax_srt_c_api_test)

    add_executable(robotweax_srt_compatible_header_test tests/test_srt_header.c)
    target_link_libraries(
        robotweax_srt_compatible_header_test PRIVATE RobotweaxSRT::srt)
    target_compile_features(
        robotweax_srt_compatible_header_test PRIVATE c_std_11)
    set_target_properties(
        robotweax_srt_compatible_header_test PROPERTIES LINKER_LANGUAGE CXX)
    add_test(
        NAME robotweax_srt_compatible_header_test
        COMMAND robotweax_srt_compatible_header_test)

    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    if(ROBOTWEAX_SRT_BUILD_EXAMPLES)
        add_test(NAME robotweax_srt_udp_bridge_loopback
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/examples/test_udp_bridge.py
                --demo $<TARGET_FILE:robotweax_srt_udp_bridge>)
        set_tests_properties(robotweax_srt_udp_bridge_loopback
            PROPERTIES TIMEOUT 120 LABELS "example;integration")
        foreach(message_demo_scenario IN ITEMS
                caller-listener
                rendezvous)
            add_test(
                NAME robotweax_srt_message_demo_${message_demo_scenario}
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/test_message_demo.py
                    --demo $<TARGET_FILE:robotweax_srt_message_demo>
                    --scenario ${message_demo_scenario})
            set_tests_properties(
                robotweax_srt_message_demo_${message_demo_scenario}
                PROPERTIES TIMEOUT 20 LABELS "example;integration")
        endforeach()
        foreach(file_demo_scenario IN ITEMS
                caller-listener
                rendezvous)
            add_test(
                NAME robotweax_srt_file_demo_${file_demo_scenario}
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/test_file_demo.py
                    --demo $<TARGET_FILE:robotweax_srt_file_demo>
                    --scenario ${file_demo_scenario})
            set_tests_properties(
                robotweax_srt_file_demo_${file_demo_scenario}
                PROPERTIES TIMEOUT 30 LABELS "example;integration;file")
        endforeach()
        foreach(group_demo_scenario IN ITEMS
                broadcast
                backup-failover)
            add_test(
                NAME robotweax_srt_group_demo_${group_demo_scenario}
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/test_group_demo.py
                    --demo $<TARGET_FILE:robotweax_srt_group_demo>
                    --scenario ${group_demo_scenario})
            set_tests_properties(
                robotweax_srt_group_demo_${group_demo_scenario}
                PROPERTIES TIMEOUT 35 LABELS "example;integration;group")
        endforeach()
        if(ENABLE_AEAD_API_PREVIEW)
            add_test(
                NAME robotweax_srt_message_demo_caller-listener_gcm
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/test_message_demo.py
                    --demo $<TARGET_FILE:robotweax_srt_message_demo>
                    --scenario caller-listener
                    --crypto gcm)
            set_tests_properties(
                robotweax_srt_message_demo_caller-listener_gcm
                PROPERTIES TIMEOUT 20
                    LABELS "example;integration;encryption")
            add_test(
                NAME robotweax_srt_file_demo_caller-listener_gcm
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/test_file_demo.py
                    --demo $<TARGET_FILE:robotweax_srt_file_demo>
                    --scenario caller-listener
                    --crypto gcm)
            set_tests_properties(
                robotweax_srt_file_demo_caller-listener_gcm
                PROPERTIES TIMEOUT 30
                    LABELS "example;integration;file;encryption")
            add_test(
                NAME robotweax_srt_group_demo_broadcast_gcm
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/test_group_demo.py
                    --demo $<TARGET_FILE:robotweax_srt_group_demo>
                    --scenario broadcast
                    --crypto gcm)
            set_tests_properties(
                robotweax_srt_group_demo_broadcast_gcm
                PROPERTIES TIMEOUT 35
                    LABELS "example;integration;group;encryption")
            add_test(
                NAME robotweax_srt_group_demo_backup-failover_gcm
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/test_group_demo.py
                    --demo $<TARGET_FILE:robotweax_srt_group_demo>
                    --scenario backup-failover
                    --crypto gcm)
            set_tests_properties(
                robotweax_srt_group_demo_backup-failover_gcm
                PROPERTIES TIMEOUT 35
                    LABELS "example;integration;group;encryption")
            foreach(demo_crypto_kind IN ITEMS
                    message file group-broadcast group-backup)
                if(demo_crypto_kind MATCHES "^group-")
                    set(demo_crypto_target robotweax_srt_group_demo)
                else()
                    set(demo_crypto_target robotweax_srt_${demo_crypto_kind}_demo)
                endif()
                add_test(
                    NAME robotweax_srt_demo_crypto_mismatch_${demo_crypto_kind}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/examples/test_crypto_modes.py
                        --demo $<TARGET_FILE:${demo_crypto_target}>
                        --kind ${demo_crypto_kind})
                set_tests_properties(
                    robotweax_srt_demo_crypto_mismatch_${demo_crypto_kind}
                    PROPERTIES TIMEOUT 35 LABELS "example;integration;encryption")
            endforeach()
        endif()
    endif()
    if(BUILD_SHARED_LIBS)
        if(WIN32)
            get_filename_component(
                ROBOTWEAX_SRT_LINKER_DIRECTORY
                "${CMAKE_LINKER}"
                DIRECTORY)
            set(ROBOTWEAX_SRT_SYMBOL_INSPECTION_TOOL
                "${ROBOTWEAX_SRT_LINKER_DIRECTORY}/dumpbin.exe")
            if(NOT EXISTS "${ROBOTWEAX_SRT_SYMBOL_INSPECTION_TOOL}")
                message(FATAL_ERROR
                    "dumpbin.exe was not found next to the MSVC linker")
            endif()
        else()
            set(ROBOTWEAX_SRT_SYMBOL_INSPECTION_TOOL ${CMAKE_NM})
        endif()
        add_test(
            NAME robotweax_srt_export_manifest_tests
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_shared_library_exports.py
                --library $<TARGET_FILE:robotweax_srt>
                --manifest ${ROBOTWEAX_SRT_PUBLIC_SYMBOLS_FILE}
                --header ${CMAKE_CURRENT_SOURCE_DIR}/include/robotweax_srt.h
                --header ${CMAKE_CURRENT_SOURCE_DIR}/include/srt/srt.h
                --tool ${ROBOTWEAX_SRT_SYMBOL_INSPECTION_TOOL})
    endif()

    if(BUILD_SHARED_LIBS)
        set(ROBOTWEAX_SRT_SHARED_LIBRARY_TESTS
            robotweax_srt_shared_lifecycle_tests
            robotweax_srt_global_lifecycle_tests
            robotweax_srt_c_api_test
            robotweax_srt_compatible_header_test)
        list(APPEND ROBOTWEAX_SRT_SHARED_LIBRARY_TESTS
            robotweax_srt_export_manifest_tests)
        set_tests_properties(
            ${ROBOTWEAX_SRT_SHARED_LIBRARY_TESTS}
            PROPERTIES LABELS "shared-library")
    endif()

    if(UNIX)
        add_test(
            NAME robotweax_srt_remote_harness_tests
            COMMAND ${Python3_EXECUTABLE} -m unittest discover
                -s interop/tests -p test_*.py)
        set_tests_properties(
            robotweax_srt_remote_harness_tests
            PROPERTIES
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    endif()
endif()

if(ROBOTWEAX_SRT_BUILD_BENCHMARKS)
    add_executable(robotweax_srt_codec_benchmark benchmarks/codec_benchmark.cpp)
    target_link_libraries(robotweax_srt_codec_benchmark PRIVATE RobotweaxSRT::srt)
    target_compile_features(robotweax_srt_codec_benchmark PRIVATE cxx_std_20)

    add_executable(robotweax_srt_fec_benchmark
        benchmarks/fec_benchmark.cpp)
    target_link_libraries(robotweax_srt_fec_benchmark
        PRIVATE RobotweaxSRT::srt)
    target_compile_features(robotweax_srt_fec_benchmark
        PRIVATE cxx_std_20)

    add_executable(robotweax_srt_scalability_peer
        benchmarks/scalability_peer.cpp)
    target_link_libraries(robotweax_srt_scalability_peer
        PRIVATE RobotweaxSRT::srt)
    target_compile_features(robotweax_srt_scalability_peer
        PRIVATE cxx_std_17)

    if(ROBOTWEAX_SRT_BUILD_TESTS AND Python3_Interpreter_FOUND)
        add_test(
            NAME robotweax_srt_many_socket_scalability_smoke
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/scalability_scorecard.py
                --topology many-socket
                --robotweax-peer
                    $<TARGET_FILE:robotweax_srt_scalability_peer>
                --profile robotweax-self
                --connections 1,4
                --message-sizes 188,1316
                --bytes-per-connection 131600
                --warmup 0
                --iterations 1
                --timeout-seconds 15
                --output
                    ${CMAKE_CURRENT_BINARY_DIR}/many-socket-smoke.json)
        set_tests_properties(
            robotweax_srt_many_socket_scalability_smoke
            PROPERTIES TIMEOUT 30 LABELS "benchmark;integration")
    endif()
endif()

if(ROBOTWEAX_SRT_BUILD_TOOLS)
    add_executable(robotweax_srt_handshake_probe tools/handshake_probe.cpp)
    target_link_libraries(robotweax_srt_handshake_probe PRIVATE RobotweaxSRT::srt)
    target_compile_features(robotweax_srt_handshake_probe PRIVATE cxx_std_20)

    add_executable(robotweax_srt_interop_peer interop/api_peer.cpp)
    target_link_libraries(
        robotweax_srt_interop_peer PRIVATE RobotweaxSRT::srt)
    target_compile_features(robotweax_srt_interop_peer PRIVATE cxx_std_17)

    add_executable(robotweax_srt_timing_peer interop/timing_peer.cpp)
    target_link_libraries(
        robotweax_srt_timing_peer PRIVATE RobotweaxSRT::srt)
    target_compile_features(robotweax_srt_timing_peer PRIVATE cxx_std_17)

    add_executable(robotweax_srt_group_timing_sender
        interop/group_timing_sender.cpp)
    target_link_libraries(
        robotweax_srt_group_timing_sender PRIVATE RobotweaxSRT::srt)
    target_compile_features(
        robotweax_srt_group_timing_sender PRIVATE cxx_std_17)

    add_executable(robotweax_srt_group_peer interop/group_peer.cpp)
    target_link_libraries(
        robotweax_srt_group_peer PRIVATE RobotweaxSRT::srt)
    target_compile_features(robotweax_srt_group_peer PRIVATE cxx_std_17)

    if(ROBOTWEAX_SRT_BUILD_TESTS AND Python3_Interpreter_FOUND AND UNIX)
        add_test(
            NAME robotweax_srt_live_timing_smoke
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                --profile ts-1316 --messages 16 --latency-ms 40
                --timeout-seconds 10)
        set_tests_properties(
            robotweax_srt_live_timing_smoke
            PROPERTIES TIMEOUT 20 LABELS "timing")

        foreach(timing_profile IN ITEMS binary-1200 ts-1316)
            string(REPLACE "-" "_" timing_test_suffix ${timing_profile})
            add_test(
                NAME robotweax_srt_live_timing_encrypted_${timing_test_suffix}
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                    --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                    --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                    --profile ${timing_profile} --messages 384
                    --bitrate-bps 7520000 --latency-ms 120
                    --timeout-seconds 10 --pbkeylen 32
                    --km-refresh-rate 64 --km-preannounce 20
                    --minimum-key-transitions 3
                CONFIGURATIONS Release)
            set_tests_properties(
                robotweax_srt_live_timing_encrypted_${timing_test_suffix}
                PROPERTIES TIMEOUT 20 LABELS "timing;encryption")
        endforeach()

        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            foreach(timing_profile IN ITEMS binary-1200 ts-1316)
                string(REPLACE "-" "_" timing_test_suffix ${timing_profile})
                set(timing_kernel_pcr_limit)
                if(timing_profile STREQUAL "ts-1316")
                    set(timing_kernel_pcr_limit
                        --maximum-pcr-span-rate-error-ppm 5000)
                endif()
                add_test(
                    NAME robotweax_srt_live_timing_linux_software_timestamp_${timing_test_suffix}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                        --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                        --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                        --profile ${timing_profile} --messages 96
                        --bitrate-bps 7520000 --latency-ms 120
                        --timeout-seconds 10
                        --udp-timestamp-source linux-software
                        --maximum-egress-p99-9-us 20000
                        --maximum-burst-depth 8
                        ${timing_kernel_pcr_limit}
                    CONFIGURATIONS Release)
                set_tests_properties(
                    robotweax_srt_live_timing_linux_software_timestamp_${timing_test_suffix}
                    PROPERTIES TIMEOUT 20
                        LABELS "timing;kernel-timestamp;linux")

                add_test(
                    NAME robotweax_srt_live_timing_backup_group_${timing_test_suffix}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                        --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                        --sender-peer $<TARGET_FILE:robotweax_srt_group_timing_sender>
                        --connection-mode backup-group
                        --group-failover-after 48
                        --profile ${timing_profile} --messages 96
                        --bitrate-bps 7520000 --latency-ms 120
                        --timeout-seconds 10
                    CONFIGURATIONS Release)
                set_tests_properties(
                    robotweax_srt_live_timing_backup_group_${timing_test_suffix}
                    PROPERTIES TIMEOUT 20 LABELS "timing;group")

                add_test(
                    NAME robotweax_srt_live_timing_backup_group_encrypted_${timing_test_suffix}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                        --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                        --sender-peer $<TARGET_FILE:robotweax_srt_group_timing_sender>
                        --connection-mode backup-group
                        --group-failover-after 192
                        --profile ${timing_profile} --messages 384
                        --bitrate-bps 7520000 --latency-ms 120
                        --timeout-seconds 15 --pbkeylen 32
                        --km-refresh-rate 64 --km-preannounce 20
                        --minimum-key-transitions 3
                    CONFIGURATIONS Release)
                set_tests_properties(
                    robotweax_srt_live_timing_backup_group_encrypted_${timing_test_suffix}
                    PROPERTIES TIMEOUT 25
                        LABELS "timing;group;encryption")

                set(timing_replay_pcr_limit)
                if(timing_profile STREQUAL "ts-1316")
                    set(timing_replay_pcr_limit
                        --maximum-pcr-span-rate-error-ppm 5000)
                endif()
                add_test(
                    NAME robotweax_srt_live_timing_backup_group_encrypted_replay_${timing_test_suffix}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                        --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                        --sender-peer $<TARGET_FILE:robotweax_srt_group_timing_sender>
                        --connection-mode backup-group
                        --group-failover-after 96
                        --group-unacknowledged-replay
                        --profile ${timing_profile} --messages 384
                        --bitrate-bps 7520000 --latency-ms 500
                        --timeout-seconds 15 --pbkeylen 32
                        --km-refresh-rate 32 --km-preannounce 10
                        --minimum-key-transitions 3
                        --maximum-egress-p99-9-us 20000
                        --maximum-tsbpd-phase-range-us 2000
                        --maximum-burst-depth 8
                        ${timing_replay_pcr_limit}
                    CONFIGURATIONS Release)
                set_tests_properties(
                    robotweax_srt_live_timing_backup_group_encrypted_replay_${timing_test_suffix}
                    PROPERTIES TIMEOUT 25
                        LABELS "timing;group;encryption")

                set(timing_outage_pcr_limit)
                if(timing_profile STREQUAL "ts-1316")
                    set(timing_outage_pcr_limit
                        --maximum-pcr-span-rate-error-ppm 5000)
                endif()
                add_test(
                    NAME robotweax_srt_live_timing_backup_group_encrypted_path_outage_${timing_test_suffix}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                        --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                        --sender-peer $<TARGET_FILE:robotweax_srt_group_timing_sender>
                        --connection-mode backup-group
                        --group-failover-after 48
                        --group-path-outage
                        --group-maximum-failover-delay-ms 1500
                        --profile ${timing_profile} --messages 320
                        --bitrate-bps 7520000 --latency-ms 300
                        --timeout-seconds 15 --pbkeylen 32
                        --km-refresh-rate 32 --km-preannounce 10
                        --minimum-key-transitions 3
                        --maximum-egress-p99-9-us 20000
                        --maximum-burst-depth 8
                        ${timing_outage_pcr_limit}
                    CONFIGURATIONS Release)
                set_tests_properties(
                    robotweax_srt_live_timing_backup_group_encrypted_path_outage_${timing_test_suffix}
                    PROPERTIES TIMEOUT 25
                        LABELS "timing;group;encryption;fault")

                add_test(
                    NAME robotweax_srt_live_timing_backup_group_encrypted_path_outage_ipv6_${timing_test_suffix}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                        --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                        --sender-peer $<TARGET_FILE:robotweax_srt_group_timing_sender>
                        --connection-mode backup-group
                        --group-failover-after 48
                        --group-path-outage
                        --group-maximum-failover-delay-ms 1500
                        --profile ${timing_profile} --messages 320
                        --bitrate-bps 7520000 --latency-ms 300
                        --timeout-seconds 15 --host ::1 --udp-host ::1
                        --pbkeylen 32 --km-refresh-rate 32
                        --km-preannounce 10 --minimum-key-transitions 3
                        --maximum-egress-p99-9-us 20000
                        --maximum-burst-depth 8
                        ${timing_outage_pcr_limit}
                    CONFIGURATIONS Release)
                set_tests_properties(
                    robotweax_srt_live_timing_backup_group_encrypted_path_outage_ipv6_${timing_test_suffix}
                    PROPERTIES TIMEOUT 25
                        LABELS "timing;group;encryption;fault;ipv6")

                add_test(
                    NAME robotweax_srt_live_timing_rendezvous_clear_${timing_test_suffix}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                        --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                        --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                        --connection-mode rendezvous
                        --profile ${timing_profile} --messages 96
                        --bitrate-bps 7520000 --latency-ms 120
                        --timeout-seconds 10
                    CONFIGURATIONS Release)
                set_tests_properties(
                    robotweax_srt_live_timing_rendezvous_clear_${timing_test_suffix}
                    PROPERTIES TIMEOUT 20 LABELS "timing;rendezvous")

                add_test(
                    NAME robotweax_srt_live_timing_rendezvous_encrypted_${timing_test_suffix}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                        --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                        --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                        --connection-mode rendezvous
                        --profile ${timing_profile} --messages 384
                        --bitrate-bps 7520000 --latency-ms 120
                        --timeout-seconds 10 --pbkeylen 32
                        --km-refresh-rate 64 --km-preannounce 20
                        --minimum-key-transitions 3
                    CONFIGURATIONS Release)
                set_tests_properties(
                    robotweax_srt_live_timing_rendezvous_encrypted_${timing_test_suffix}
                    PROPERTIES TIMEOUT 20
                        LABELS "timing;rendezvous;encryption")

                set(timing_rendezvous_fec_pcr_limit)
                if(timing_profile STREQUAL "ts-1316")
                    set(timing_rendezvous_fec_pcr_limit
                        --maximum-pcr-span-rate-error-ppm 5000)
                endif()
                add_test(
                    NAME robotweax_srt_live_timing_rendezvous_encrypted_fec_${timing_test_suffix}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                        --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                        --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                        --connection-mode rendezvous
                        --profile ${timing_profile} --messages 384
                        --bitrate-bps 7520000 --latency-ms 300
                        --timeout-seconds 15 --pbkeylen 32
                        --km-refresh-rate 64 --km-preannounce 20
                        --minimum-key-transitions 3
                        --fault-profile fec-source-drop
                        --fec-profile matrix
                        --maximum-egress-p99-9-us 20000
                        --maximum-burst-depth 8
                        ${timing_rendezvous_fec_pcr_limit}
                    CONFIGURATIONS Release)
                set_tests_properties(
                    robotweax_srt_live_timing_rendezvous_encrypted_fec_${timing_test_suffix}
                    PROPERTIES TIMEOUT 25
                        LABELS "timing;rendezvous;encryption;fec;fault")

                add_test(
                    NAME robotweax_srt_live_timing_ipv6_clear_${timing_test_suffix}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                        --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                        --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                        --profile ${timing_profile} --messages 96
                        --bitrate-bps 7520000 --latency-ms 120
                        --timeout-seconds 10 --host ::1 --udp-host ::1
                    CONFIGURATIONS Release)
                set_tests_properties(
                    robotweax_srt_live_timing_ipv6_clear_${timing_test_suffix}
                    PROPERTIES TIMEOUT 20 LABELS "timing;ipv6")

                add_test(
                    NAME robotweax_srt_live_timing_ipv6_encrypted_${timing_test_suffix}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                        --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                        --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                        --profile ${timing_profile} --messages 384
                        --bitrate-bps 7520000 --latency-ms 120
                        --timeout-seconds 10 --host ::1 --udp-host ::1
                        --pbkeylen 32 --km-refresh-rate 64
                        --km-preannounce 20 --minimum-key-transitions 3
                    CONFIGURATIONS Release)
                set_tests_properties(
                    robotweax_srt_live_timing_ipv6_encrypted_${timing_test_suffix}
                    PROPERTIES TIMEOUT 20
                        LABELS "timing;ipv6;encryption")

                set(timing_ipv6_fault_pcr_limit)
                if(timing_profile STREQUAL "ts-1316")
                    set(timing_ipv6_fault_pcr_limit
                        --maximum-pcr-span-rate-error-ppm 5000)
                endif()
                add_test(
                    NAME robotweax_srt_live_timing_ipv6_encrypted_fault_${timing_test_suffix}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                        --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                        --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                        --profile ${timing_profile} --messages 384
                        --bitrate-bps 7520000 --latency-ms 300
                        --timeout-seconds 15 --host ::1 --udp-host ::1
                        --pbkeylen 32 --km-refresh-rate 64
                        --km-preannounce 20 --minimum-key-transitions 3
                        --fault-profile loss-delay-reorder
                        --maximum-egress-p99-9-us 20000
                        --maximum-burst-depth 8
                        ${timing_ipv6_fault_pcr_limit}
                    CONFIGURATIONS Release)
                set_tests_properties(
                    robotweax_srt_live_timing_ipv6_encrypted_fault_${timing_test_suffix}
                    PROPERTIES TIMEOUT 25
                        LABELS "timing;ipv6;encryption;fault")

                add_test(
                    NAME robotweax_srt_live_timing_encrypted_fault_${timing_test_suffix}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                        --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                        --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                        --profile ${timing_profile} --messages 384
                        --bitrate-bps 7520000 --latency-ms 300
                        --timeout-seconds 15 --pbkeylen 32
                        --km-refresh-rate 64 --km-preannounce 20
                        --minimum-key-transitions 3
                        --fault-profile loss-delay-reorder
                    CONFIGURATIONS Release)
                set_tests_properties(
                    robotweax_srt_live_timing_encrypted_fault_${timing_test_suffix}
                    PROPERTIES TIMEOUT 25 LABELS "timing;encryption;fault")

                set(timing_fec_pcr_limit)
                if(timing_profile STREQUAL "ts-1316")
                    set(timing_fec_pcr_limit
                        --maximum-pcr-span-rate-error-ppm 5000)
                endif()
                foreach(timing_fec_profile IN ITEMS row column)
                    add_test(
                        NAME robotweax_srt_live_timing_fec_${timing_fec_profile}_${timing_test_suffix}
                        COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                            --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                            --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                            --profile ${timing_profile} --messages 384
                            --bitrate-bps 7520000 --latency-ms 300
                            --timeout-seconds 15
                            --fault-profile fec-source-drop
                            --fec-profile ${timing_fec_profile}
                            --maximum-egress-p99-9-us 20000
                            --maximum-burst-depth 8
                            ${timing_fec_pcr_limit}
                        CONFIGURATIONS Release)
                    set_tests_properties(
                        robotweax_srt_live_timing_fec_${timing_fec_profile}_${timing_test_suffix}
                        PROPERTIES TIMEOUT 25
                            LABELS "timing;fec;fault")

                    add_test(
                        NAME robotweax_srt_live_timing_encrypted_fec_${timing_fec_profile}_${timing_test_suffix}
                        COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                            --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                            --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                            --profile ${timing_profile} --messages 384
                            --bitrate-bps 7520000 --latency-ms 300
                            --timeout-seconds 15 --pbkeylen 32
                            --km-refresh-rate 64 --km-preannounce 20
                            --minimum-key-transitions 3
                            --fault-profile fec-source-drop
                            --fec-profile ${timing_fec_profile}
                            --maximum-egress-p99-9-us 20000
                            --maximum-burst-depth 8
                            ${timing_fec_pcr_limit}
                        CONFIGURATIONS Release)
                    set_tests_properties(
                        robotweax_srt_live_timing_encrypted_fec_${timing_fec_profile}_${timing_test_suffix}
                        PROPERTIES TIMEOUT 25
                            LABELS "timing;encryption;fec;fault")

                    add_test(
                        NAME robotweax_srt_live_timing_ipv6_fec_${timing_fec_profile}_${timing_test_suffix}
                        COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                            --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                            --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                            --profile ${timing_profile} --messages 384
                            --bitrate-bps 7520000 --latency-ms 300
                            --timeout-seconds 15 --host ::1 --udp-host ::1
                            --fault-profile fec-source-drop
                            --fec-profile ${timing_fec_profile}
                            --maximum-egress-p99-9-us 20000
                            --maximum-burst-depth 8
                            ${timing_fec_pcr_limit}
                        CONFIGURATIONS Release)
                    set_tests_properties(
                        robotweax_srt_live_timing_ipv6_fec_${timing_fec_profile}_${timing_test_suffix}
                        PROPERTIES TIMEOUT 25
                            LABELS "timing;ipv6;fec;fault")

                    add_test(
                        NAME robotweax_srt_live_timing_ipv6_encrypted_fec_${timing_fec_profile}_${timing_test_suffix}
                        COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                            --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                            --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                            --profile ${timing_profile} --messages 384
                            --bitrate-bps 7520000 --latency-ms 300
                            --timeout-seconds 15 --host ::1 --udp-host ::1
                            --pbkeylen 32 --km-refresh-rate 64
                            --km-preannounce 20 --minimum-key-transitions 3
                            --fault-profile fec-source-drop
                            --fec-profile ${timing_fec_profile}
                            --maximum-egress-p99-9-us 20000
                            --maximum-burst-depth 8
                            ${timing_fec_pcr_limit}
                        CONFIGURATIONS Release)
                    set_tests_properties(
                        robotweax_srt_live_timing_ipv6_encrypted_fec_${timing_fec_profile}_${timing_test_suffix}
                        PROPERTIES TIMEOUT 25
                            LABELS "timing;ipv6;encryption;fec;fault")

                    add_test(
                        NAME robotweax_srt_live_timing_rendezvous_fec_${timing_fec_profile}_${timing_test_suffix}
                        COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                            --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                            --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                            --connection-mode rendezvous
                            --profile ${timing_profile} --messages 384
                            --bitrate-bps 7520000 --latency-ms 300
                            --timeout-seconds 15
                            --fault-profile fec-source-drop
                            --fec-profile ${timing_fec_profile}
                            --maximum-egress-p99-9-us 20000
                            --maximum-burst-depth 8
                            ${timing_fec_pcr_limit}
                        CONFIGURATIONS Release)
                    set_tests_properties(
                        robotweax_srt_live_timing_rendezvous_fec_${timing_fec_profile}_${timing_test_suffix}
                        PROPERTIES TIMEOUT 25
                            LABELS "timing;rendezvous;fec;fault")

                    add_test(
                        NAME robotweax_srt_live_timing_rendezvous_encrypted_fec_${timing_fec_profile}_${timing_test_suffix}
                        COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                            --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                            --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                            --connection-mode rendezvous
                            --profile ${timing_profile} --messages 384
                            --bitrate-bps 7520000 --latency-ms 300
                            --timeout-seconds 15 --pbkeylen 32
                            --km-refresh-rate 64 --km-preannounce 20
                            --minimum-key-transitions 3
                            --fault-profile fec-source-drop
                            --fec-profile ${timing_fec_profile}
                            --maximum-egress-p99-9-us 20000
                            --maximum-burst-depth 8
                            ${timing_fec_pcr_limit}
                        CONFIGURATIONS Release)
                    set_tests_properties(
                        robotweax_srt_live_timing_rendezvous_encrypted_fec_${timing_fec_profile}_${timing_test_suffix}
                        PROPERTIES TIMEOUT 25
                            LABELS "timing;rendezvous;encryption;fec;fault")

                    add_test(
                        NAME robotweax_srt_live_timing_ipv6_rendezvous_fec_${timing_fec_profile}_${timing_test_suffix}
                        COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                            --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                            --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                            --connection-mode rendezvous
                            --profile ${timing_profile} --messages 384
                            --bitrate-bps 7520000 --latency-ms 300
                            --timeout-seconds 15 --host ::1 --udp-host ::1
                            --fault-profile fec-source-drop
                            --fec-profile ${timing_fec_profile}
                            --maximum-egress-p99-9-us 20000
                            --maximum-burst-depth 8
                            ${timing_fec_pcr_limit}
                        CONFIGURATIONS Release)
                    set_tests_properties(
                        robotweax_srt_live_timing_ipv6_rendezvous_fec_${timing_fec_profile}_${timing_test_suffix}
                        PROPERTIES TIMEOUT 25
                            LABELS "timing;ipv6;rendezvous;fec;fault")

                    add_test(
                        NAME robotweax_srt_live_timing_ipv6_rendezvous_encrypted_fec_${timing_fec_profile}_${timing_test_suffix}
                        COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                            --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                            --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                            --connection-mode rendezvous
                            --profile ${timing_profile} --messages 384
                            --bitrate-bps 7520000 --latency-ms 300
                            --timeout-seconds 15 --host ::1 --udp-host ::1
                            --pbkeylen 32 --km-refresh-rate 64
                            --km-preannounce 20 --minimum-key-transitions 3
                            --fault-profile fec-source-drop
                            --fec-profile ${timing_fec_profile}
                            --maximum-egress-p99-9-us 20000
                            --maximum-burst-depth 8
                            ${timing_fec_pcr_limit}
                        CONFIGURATIONS Release)
                    set_tests_properties(
                        robotweax_srt_live_timing_ipv6_rendezvous_encrypted_fec_${timing_fec_profile}_${timing_test_suffix}
                        PROPERTIES TIMEOUT 25
                            LABELS "timing;ipv6;rendezvous;encryption;fec;fault")
                endforeach()

                add_test(
                    NAME robotweax_srt_live_timing_encrypted_fec_${timing_test_suffix}
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                        --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                        --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                        --profile ${timing_profile} --messages 384
                        --bitrate-bps 7520000 --latency-ms 300
                        --timeout-seconds 15 --pbkeylen 32
                        --km-refresh-rate 64 --km-preannounce 20
                        --minimum-key-transitions 3
                        --fault-profile fec-source-drop
                        --fec-profile matrix
                        --maximum-egress-p99-9-us 20000
                        --maximum-burst-depth 8
                        ${timing_fec_pcr_limit}
                    CONFIGURATIONS Release)
                set_tests_properties(
                    robotweax_srt_live_timing_encrypted_fec_${timing_test_suffix}
                    PROPERTIES TIMEOUT 25
                        LABELS "timing;encryption;fec;fault")

                foreach(timing_fec_burst_profile IN ITEMS column matrix)
                    add_test(
                        NAME robotweax_srt_live_timing_fec_burst_${timing_fec_burst_profile}_${timing_test_suffix}
                        COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                            --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                            --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                            --profile ${timing_profile} --messages 384
                            --bitrate-bps 7520000 --latency-ms 300
                            --timeout-seconds 15
                            --fault-profile fec-burst-drop
                            --fec-profile ${timing_fec_burst_profile}
                            --maximum-egress-p99-9-us 20000
                            --maximum-burst-depth 8
                            ${timing_fec_pcr_limit}
                        CONFIGURATIONS Release)
                    set_tests_properties(
                        robotweax_srt_live_timing_fec_burst_${timing_fec_burst_profile}_${timing_test_suffix}
                        PROPERTIES TIMEOUT 25
                            LABELS "timing;fec;fault;burst")

                    add_test(
                        NAME robotweax_srt_live_timing_encrypted_fec_burst_${timing_fec_burst_profile}_${timing_test_suffix}
                        COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                            --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                            --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                            --profile ${timing_profile} --messages 384
                            --bitrate-bps 7520000 --latency-ms 300
                            --timeout-seconds 15 --pbkeylen 32
                            --km-refresh-rate 64 --km-preannounce 20
                            --minimum-key-transitions 3
                            --fault-profile fec-burst-drop
                            --fec-profile ${timing_fec_burst_profile}
                            --maximum-egress-p99-9-us 20000
                            --maximum-burst-depth 8
                            ${timing_fec_pcr_limit}
                        CONFIGURATIONS Release)
                    set_tests_properties(
                        robotweax_srt_live_timing_encrypted_fec_burst_${timing_fec_burst_profile}_${timing_test_suffix}
                        PROPERTIES TIMEOUT 25
                            LABELS "timing;encryption;fec;fault;burst")
                endforeach()

                foreach(timing_fec_compound_profile IN ITEMS column matrix)
                    add_test(
                        NAME robotweax_srt_live_timing_ipv6_rendezvous_fec_compound_${timing_fec_compound_profile}_${timing_test_suffix}
                        COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                            --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                            --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                            --connection-mode rendezvous
                            --profile ${timing_profile} --messages 384
                            --bitrate-bps 7520000 --latency-ms 300
                            --timeout-seconds 15 --host ::1 --udp-host ::1
                            --fault-profile fec-burst-delay-reorder
                            --fec-profile ${timing_fec_compound_profile}
                            --maximum-egress-p99-9-us 20000
                            --maximum-burst-depth 8
                            ${timing_fec_pcr_limit}
                        CONFIGURATIONS Release)
                    set_tests_properties(
                        robotweax_srt_live_timing_ipv6_rendezvous_fec_compound_${timing_fec_compound_profile}_${timing_test_suffix}
                        PROPERTIES TIMEOUT 25
                            LABELS "timing;ipv6;rendezvous;fec;fault;compound")

                    add_test(
                        NAME robotweax_srt_live_timing_ipv6_rendezvous_encrypted_fec_compound_${timing_fec_compound_profile}_${timing_test_suffix}
                        COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                            --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                            --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                            --connection-mode rendezvous
                            --profile ${timing_profile} --messages 384
                            --bitrate-bps 7520000 --latency-ms 300
                            --timeout-seconds 15 --host ::1 --udp-host ::1
                            --pbkeylen 32 --km-refresh-rate 64
                            --km-preannounce 20 --minimum-key-transitions 3
                            --fault-profile fec-burst-delay-reorder
                            --fec-profile ${timing_fec_compound_profile}
                            --maximum-egress-p99-9-us 20000
                            --maximum-burst-depth 8
                            ${timing_fec_pcr_limit}
                        CONFIGURATIONS Release)
                    set_tests_properties(
                        robotweax_srt_live_timing_ipv6_rendezvous_encrypted_fec_compound_${timing_fec_compound_profile}_${timing_test_suffix}
                        PROPERTIES TIMEOUT 25
                            LABELS "timing;ipv6;rendezvous;encryption;fec;fault;compound")
                endforeach()
            endforeach()

            add_test(
                NAME robotweax_srt_live_timing_fault_smoke
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_timing_interop.py
                    --timing-peer $<TARGET_FILE:robotweax_srt_timing_peer>
                    --sender-peer $<TARGET_FILE:robotweax_srt_interop_peer>
                    --profile ts-1316 --messages 4096
                    --bitrate-bps 7520000 --latency-ms 600
                    --timeout-seconds 20
                    --fault-profile loss-delay-reorder
                    --maximum-egress-p99-9-us 20000
                    --maximum-burst-depth 8
                    --maximum-pcr-span-rate-error-ppm 5000
                CONFIGURATIONS Release)
            set_tests_properties(
                robotweax_srt_live_timing_fault_smoke
                PROPERTIES TIMEOUT 30 LABELS "timing")
        endif()
    endif()

    add_executable(robotweax_srt_crypto_soak tools/crypto_soak.cpp)
    target_link_libraries(robotweax_srt_crypto_soak
        PRIVATE RobotweaxSRT::srt)
    target_compile_features(robotweax_srt_crypto_soak PRIVATE cxx_std_20)
    add_executable(robotweax_srt_fec_chain_soak
        tools/fec_chain_soak.cpp)
    target_link_libraries(robotweax_srt_fec_chain_soak
        PRIVATE RobotweaxSRT::srt)
    target_compile_features(robotweax_srt_fec_chain_soak PRIVATE cxx_std_20)
    if(ROBOTWEAX_SRT_BUILD_TESTS)
        add_test(
            NAME robotweax_srt_crypto_soak_smoke
            COMMAND robotweax_srt_crypto_soak
                --rotations 32 --seed 1)
        set_tests_properties(
            robotweax_srt_crypto_soak_smoke
            PROPERTIES LABELS "soak")
        add_test(
            NAME robotweax_srt_fec_chain_soak_smoke
            COMMAND robotweax_srt_fec_chain_soak
                --iterations 16 --seed 1)
        set_tests_properties(
            robotweax_srt_fec_chain_soak_smoke
            PROPERTIES TIMEOUT 30 LABELS "soak;fec")
    endif()
endif()

if(ROBOTWEAX_SRT_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "Fuzz targets require Clang")
    endif()
    target_compile_options(robotweax_srt PRIVATE -fsanitize=fuzzer-no-link,address,undefined)
    add_executable(fuzz_packet_decode fuzz/fuzz_packet_decode.cpp)
    target_link_libraries(fuzz_packet_decode PRIVATE RobotweaxSRT::srt)
    target_compile_options(fuzz_packet_decode PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_packet_decode PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(fuzz_handshake_decode fuzz/fuzz_handshake_decode.cpp)
    target_link_libraries(fuzz_handshake_decode PRIVATE RobotweaxSRT::srt)
    target_compile_options(fuzz_handshake_decode PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_handshake_decode PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(fuzz_key_material_decode
        fuzz/fuzz_key_material_decode.cpp)
    target_link_libraries(fuzz_key_material_decode
        PRIVATE RobotweaxSRT::srt)
    target_compile_options(fuzz_key_material_decode
        PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_key_material_decode
        PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(fuzz_group_membership_decode
        fuzz/fuzz_group_membership_decode.cpp)
    target_link_libraries(fuzz_group_membership_decode
        PRIVATE RobotweaxSRT::srt)
    target_compile_options(fuzz_group_membership_decode
        PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_group_membership_decode
        PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(fuzz_fec_control_decode
        fuzz/fuzz_fec_control_decode.cpp)
    target_link_libraries(fuzz_fec_control_decode
        PRIVATE RobotweaxSRT::srt)
    target_compile_options(fuzz_fec_control_decode
        PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_fec_control_decode
        PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(fuzz_aead_packet_open
        fuzz/fuzz_aead_packet_open.cpp)
    target_link_libraries(fuzz_aead_packet_open
        PRIVATE RobotweaxSRT::srt)
    target_compile_options(fuzz_aead_packet_open
        PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_aead_packet_open
        PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

configure_package_config_file(
    cmake/RobotweaxSRTConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/RobotweaxSRTConfig.cmake"
    INSTALL_DESTINATION "${ROBOTWEAX_SRT_INSTALL_CMAKEDIR}")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/RobotweaxSRTConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)

if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(ROBOTWEAX_SRT_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
    set(ROBOTWEAX_SRT_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
else()
    # Compute the relocatable prefix from native absolute paths.  Using
    # Unix-rooted placeholders (for example, "/lib/pkgconfig" and "/")
    # is not portable to CMake running on Windows.
    set(ROBOTWEAX_SRT_PC_LAYOUT_ROOT
        "${CMAKE_CURRENT_BINARY_DIR}/robotweax-srt-pc-layout")
    set(ROBOTWEAX_SRT_PC_LAYOUT_DIRECTORY
        "${ROBOTWEAX_SRT_PC_LAYOUT_ROOT}/${CMAKE_INSTALL_LIBDIR}/pkgconfig")
    file(RELATIVE_PATH ROBOTWEAX_SRT_PC_PREFIX_RELATIVE
        "${ROBOTWEAX_SRT_PC_LAYOUT_DIRECTORY}"
        "${ROBOTWEAX_SRT_PC_LAYOUT_ROOT}")
    set(ROBOTWEAX_SRT_PC_PREFIX
        "\${pcfiledir}/${ROBOTWEAX_SRT_PC_PREFIX_RELATIVE}")
    set(ROBOTWEAX_SRT_PC_LIBDIR
        "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
endif()
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(ROBOTWEAX_SRT_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
else()
    set(ROBOTWEAX_SRT_PC_INCLUDEDIR
        "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()
set(ROBOTWEAX_SRT_PC_PRIVATE_LIBS "${CMAKE_THREAD_LIBS_INIT}")
set(ROBOTWEAX_SRT_PC_CFLAGS "")
if(ENABLE_AEAD_API_PREVIEW)
    set(ROBOTWEAX_SRT_PC_CFLAGS "-DENABLE_AEAD_API_PREVIEW")
endif()
if(WIN32)
    string(APPEND ROBOTWEAX_SRT_PC_PRIVATE_LIBS " -lws2_32")
endif()
string(STRIP
    "${ROBOTWEAX_SRT_PC_PRIVATE_LIBS}"
    ROBOTWEAX_SRT_PC_PRIVATE_LIBS)
configure_file(
    cmake/robotweax-srt.pc.in
    "${CMAKE_CURRENT_BINARY_DIR}/robotweax-srt.pc"
    @ONLY)
configure_file(
    cmake/srt.pc.in
    "${CMAKE_CURRENT_BINARY_DIR}/srt.pc"
    @ONLY)

install(TARGETS robotweax_srt EXPORT RobotweaxSRTTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES
    include/robotweax_srt.h
    include/srt.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES
    include/srt/access_control.h
    include/srt/logging_api.h
    include/srt/srt.h
    include/srt/version.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/srt)
set(ROBOTWEAX_SRT_PUBLIC_COMPATIBILITY_MANIFESTS
    compat/robotweax-0.2-aead-fixtures.json
    compat/robotweax-0.2-aead.json
    compat/srt-1.5.7-api.json)
install(FILES
    LICENSE
    README.md
    CHANGELOG.md
    CONTRIBUTING.md
    SECURITY.md
    THIRD_PARTY.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/robotweax_srt)
install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/robotweax_srt/docs
    FILES_MATCHING PATTERN "*.md")
install(FILES examples/ios/README.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/robotweax_srt/examples/ios)
install(FILES examples/README.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/robotweax_srt/examples)
install(FILES
    examples/installed-consumer/CMakeLists.txt
    examples/installed-consumer/main.cpp
    examples/installed-consumer/README.md
    DESTINATION
        ${CMAKE_INSTALL_DATADIR}/doc/robotweax_srt/examples/installed-consumer)
install(FILES ${ROBOTWEAX_SRT_PUBLIC_COMPATIBILITY_MANIFESTS}
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/robotweax_srt/compat)
install(FILES ${ROBOTWEAX_SRT_ABI_BASELINE_FILE}
    DESTINATION ${CMAKE_INSTALL_DATADIR}/robotweax_srt/abi)
install(EXPORT RobotweaxSRTTargets
    NAMESPACE RobotweaxSRT::
    DESTINATION ${ROBOTWEAX_SRT_INSTALL_CMAKEDIR})
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/RobotweaxSRTConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/RobotweaxSRTConfigVersion.cmake"
    DESTINATION ${ROBOTWEAX_SRT_INSTALL_CMAKEDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/robotweax-srt.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
if(ROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/srt.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

if(ROBOTWEAX_SRT_BUILD_TESTS)
    find_program(ROBOTWEAX_SRT_PKG_CONFIG_EXECUTABLE pkg-config)
    add_test(
        NAME robotweax_srt_abi_baseline_tests
        COMMAND ${CMAKE_COMMAND} -E compare_files
            ${ROBOTWEAX_SRT_PUBLIC_SYMBOLS_FILE}
            ${ROBOTWEAX_SRT_ABI_BASELINE_FILE})
    set_tests_properties(
        robotweax_srt_abi_baseline_tests
        PROPERTIES LABELS "abi;package")

    add_test(
        NAME robotweax_srt_package_consumer_tests
        COMMAND ${CMAKE_COMMAND}
            -DROBOTWEAX_SRT_SOURCE_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DROBOTWEAX_SRT_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/package_consumer
            -DROBOTWEAX_SRT_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/package-consumer
            -DROBOTWEAX_SRT_BUILD_CONFIG=$<CONFIG>
            -DROBOTWEAX_SRT_GENERATOR=${CMAKE_GENERATOR}
            -DROBOTWEAX_SRT_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
            -DROBOTWEAX_SRT_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
            -DROBOTWEAX_SRT_OPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}
            -DROBOTWEAX_SRT_INSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}
            -DROBOTWEAX_SRT_INSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}
            -DROBOTWEAX_SRT_INSTALL_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}
            -DROBOTWEAX_SRT_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}
            -DROBOTWEAX_SRT_INSTALL_CMAKEDIR=${ROBOTWEAX_SRT_INSTALL_CMAKEDIR}
            -DROBOTWEAX_SRT_C_COMPILER=${CMAKE_C_COMPILER}
            -DROBOTWEAX_SRT_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DROBOTWEAX_SRT_PYTHON_EXECUTABLE=${Python3_EXECUTABLE}
            "-DROBOTWEAX_SRT_C_FLAGS=${CMAKE_C_FLAGS}"
            "-DROBOTWEAX_SRT_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
            "-DROBOTWEAX_SRT_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
            -DROBOTWEAX_SRT_PKG_CONFIG_EXECUTABLE=${ROBOTWEAX_SRT_PKG_CONFIG_EXECUTABLE}
            -DROBOTWEAX_SRT_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}
            -DROBOTWEAX_SRT_BUILD_SHARED_LIBS=${BUILD_SHARED_LIBS}
            -DROBOTWEAX_SRT_AEAD_API_PREVIEW=${ENABLE_AEAD_API_PREVIEW}
            -DROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT=${ROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT}
            -DROBOTWEAX_SRT_PACKAGE_VERSION=${PROJECT_VERSION}
            -DROBOTWEAX_SRT_COMPATIBLE_SRT_VERSION=${ROBOTWEAX_SRT_COMPATIBLE_SRT_VERSION}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_consumer/run.cmake)
    set(ROBOTWEAX_SRT_PACKAGE_TEST_LABELS "package")
    if(BUILD_SHARED_LIBS)
        list(APPEND ROBOTWEAX_SRT_PACKAGE_TEST_LABELS "shared-library")
    endif()
    set_tests_properties(robotweax_srt_package_consumer_tests
        PROPERTIES
            LABELS "${ROBOTWEAX_SRT_PACKAGE_TEST_LABELS}"
            TIMEOUT 120)
endif()
