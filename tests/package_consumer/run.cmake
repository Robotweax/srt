cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        ROBOTWEAX_SRT_SOURCE_BINARY_DIR
        ROBOTWEAX_SRT_CONSUMER_SOURCE_DIR
        ROBOTWEAX_SRT_TEST_ROOT
        ROBOTWEAX_SRT_GENERATOR
        ROBOTWEAX_SRT_INSTALL_BINDIR
        ROBOTWEAX_SRT_INSTALL_DATADIR
        ROBOTWEAX_SRT_INSTALL_INCLUDEDIR
        ROBOTWEAX_SRT_INSTALL_LIBDIR
        ROBOTWEAX_SRT_INSTALL_CMAKEDIR
        ROBOTWEAX_SRT_C_COMPILER
        ROBOTWEAX_SRT_CXX_COMPILER
        ROBOTWEAX_SRT_PYTHON_EXECUTABLE
        ROBOTWEAX_SRT_PACKAGE_VERSION
        ROBOTWEAX_SRT_COMPATIBLE_SRT_VERSION)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

set(stage_directory "${ROBOTWEAX_SRT_TEST_ROOT}/stage")
cmake_path(NORMAL_PATH stage_directory)
foreach(install_directory_variable IN ITEMS
        ROBOTWEAX_SRT_INSTALL_BINDIR
        ROBOTWEAX_SRT_INSTALL_DATADIR
        ROBOTWEAX_SRT_INSTALL_INCLUDEDIR
        ROBOTWEAX_SRT_INSTALL_LIBDIR
        ROBOTWEAX_SRT_INSTALL_CMAKEDIR)
    if(IS_ABSOLUTE "${${install_directory_variable}}")
        message(FATAL_ERROR
            "${install_directory_variable} must be relative for the isolated "
            "package-consumer staging test")
    endif()
    set(install_directory "${${install_directory_variable}}")
    cmake_path(ABSOLUTE_PATH install_directory
        BASE_DIRECTORY "${stage_directory}"
        NORMALIZE
        OUTPUT_VARIABLE staged_install_directory)
    string(FIND
        "${staged_install_directory}/"
        "${stage_directory}/"
        stage_prefix_index)
    if(NOT stage_prefix_index EQUAL 0)
        message(FATAL_ERROR
            "${install_directory_variable} escapes the isolated "
            "package-consumer staging directory")
    endif()
endforeach()

function(run_checked description)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${description} failed (${result})\n${output}\n${error}")
    endif()
endfunction()

set(consumer_binary_directory "${ROBOTWEAX_SRT_TEST_ROOT}/build")
file(REMOVE_RECURSE "${ROBOTWEAX_SRT_TEST_ROOT}")

set(install_command
    "${CMAKE_COMMAND}" --install "${ROBOTWEAX_SRT_SOURCE_BINARY_DIR}"
    --prefix "${stage_directory}")
if(NOT "${ROBOTWEAX_SRT_BUILD_CONFIG}" STREQUAL "")
    list(APPEND install_command --config "${ROBOTWEAX_SRT_BUILD_CONFIG}")
endif()
run_checked("Robotweax SRT installation" ${install_command})

set(installed_include_directory
    "${stage_directory}/${ROBOTWEAX_SRT_INSTALL_INCLUDEDIR}")
foreach(public_header IN ITEMS
        robotweax_srt.h
        srt.h
        srt/logging_api.h
        srt/srt.h
        srt/version.h)
    if(NOT EXISTS "${installed_include_directory}/${public_header}")
        message(FATAL_ERROR
            "missing installed public header: ${public_header}")
    endif()
endforeach()
if(EXISTS "${installed_include_directory}/robotweax/srt")
    message(FATAL_ERROR
        "private native C++ headers were installed as public package headers")
endif()

set(documentation_directory
    "${stage_directory}/${ROBOTWEAX_SRT_INSTALL_DATADIR}/doc/robotweax_srt")
run_checked("installed documentation validation"
    "${ROBOTWEAX_SRT_PYTHON_EXECUTABLE}"
    "${ROBOTWEAX_SRT_CONSUMER_SOURCE_DIR}/check_documentation.py"
    "${documentation_directory}")

set(public_consumer_source_directory
    "${documentation_directory}/examples/installed-consumer")
set(public_consumer_binary_directory
    "${ROBOTWEAX_SRT_TEST_ROOT}/installed-consumer-build")
set(public_consumer_configure_command
    "${CMAKE_COMMAND}"
    -S "${public_consumer_source_directory}"
    -B "${public_consumer_binary_directory}"
    -G "${ROBOTWEAX_SRT_GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${stage_directory}")
if(NOT "${ROBOTWEAX_SRT_GENERATOR_PLATFORM}" STREQUAL "")
    list(APPEND public_consumer_configure_command
        -A "${ROBOTWEAX_SRT_GENERATOR_PLATFORM}")
endif()
if(NOT "${ROBOTWEAX_SRT_GENERATOR_TOOLSET}" STREQUAL "")
    list(APPEND public_consumer_configure_command
        -T "${ROBOTWEAX_SRT_GENERATOR_TOOLSET}")
endif()
if(NOT "${ROBOTWEAX_SRT_BUILD_CONFIG}" STREQUAL "")
    list(APPEND public_consumer_configure_command
        "-DCMAKE_BUILD_TYPE=${ROBOTWEAX_SRT_BUILD_CONFIG}")
endif()
if(NOT "${ROBOTWEAX_SRT_OPENSSL_ROOT_DIR}" STREQUAL "")
    list(APPEND public_consumer_configure_command
        "-DOPENSSL_ROOT_DIR=${ROBOTWEAX_SRT_OPENSSL_ROOT_DIR}")
endif()
foreach(flag_variable IN ITEMS
        CXX_FLAGS
        EXE_LINKER_FLAGS)
    if(NOT "${ROBOTWEAX_SRT_${flag_variable}}" STREQUAL "")
        list(APPEND public_consumer_configure_command
            "-DCMAKE_${flag_variable}=${ROBOTWEAX_SRT_${flag_variable}}")
    endif()
endforeach()
run_checked("installed public consumer configuration"
    ${public_consumer_configure_command})

set(public_consumer_build_command
    "${CMAKE_COMMAND}" --build "${public_consumer_binary_directory}"
    --parallel)
if(NOT "${ROBOTWEAX_SRT_BUILD_CONFIG}" STREQUAL "")
    list(APPEND public_consumer_build_command
        --config "${ROBOTWEAX_SRT_BUILD_CONFIG}")
endif()
run_checked("installed public consumer build"
    ${public_consumer_build_command})

set(public_consumer_test_command
    "${CMAKE_CTEST_COMMAND}"
    --test-dir "${public_consumer_binary_directory}"
    --output-on-failure)
if(NOT "${ROBOTWEAX_SRT_BUILD_CONFIG}" STREQUAL "")
    list(APPEND public_consumer_test_command
        -C "${ROBOTWEAX_SRT_BUILD_CONFIG}")
endif()
if(WIN32)
    set(original_path "$ENV{PATH}")
    set(ENV{PATH}
        "${stage_directory}/${ROBOTWEAX_SRT_INSTALL_BINDIR};${original_path}")
    run_checked("installed public consumer execution"
        ${public_consumer_test_command})
    set(ENV{PATH} "${original_path}")
else()
    run_checked("installed public consumer execution"
        ${public_consumer_test_command})
endif()

set(pkgconfig_directory
    "${stage_directory}/${ROBOTWEAX_SRT_INSTALL_LIBDIR}/pkgconfig")
set(libsrt_compatibility_metadata "${pkgconfig_directory}/srt.pc")
if(ROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT)
    if(NOT EXISTS "${libsrt_compatibility_metadata}")
        message(FATAL_ERROR "missing opt-in srt.pc compatibility metadata")
    endif()
elseif(EXISTS "${libsrt_compatibility_metadata}")
    message(FATAL_ERROR
        "srt.pc was installed without the explicit compatibility option")
endif()

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${ROBOTWEAX_SRT_CONSUMER_SOURCE_DIR}"
    -B "${consumer_binary_directory}"
    -G "${ROBOTWEAX_SRT_GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${stage_directory}"
    "-DROBOTWEAX_SRT_PACKAGE_VERSION=${ROBOTWEAX_SRT_PACKAGE_VERSION}")
if(NOT "${ROBOTWEAX_SRT_GENERATOR_PLATFORM}" STREQUAL "")
    list(APPEND configure_command
        -A "${ROBOTWEAX_SRT_GENERATOR_PLATFORM}")
endif()
if(NOT "${ROBOTWEAX_SRT_GENERATOR_TOOLSET}" STREQUAL "")
    list(APPEND configure_command
        -T "${ROBOTWEAX_SRT_GENERATOR_TOOLSET}")
endif()
if(NOT "${ROBOTWEAX_SRT_BUILD_CONFIG}" STREQUAL "")
    list(APPEND configure_command
        "-DCMAKE_BUILD_TYPE=${ROBOTWEAX_SRT_BUILD_CONFIG}")
endif()
if(NOT "${ROBOTWEAX_SRT_OPENSSL_ROOT_DIR}" STREQUAL "")
    list(APPEND configure_command
        "-DOPENSSL_ROOT_DIR=${ROBOTWEAX_SRT_OPENSSL_ROOT_DIR}")
endif()
if(ROBOTWEAX_SRT_AEAD_API_PREVIEW)
    list(APPEND configure_command
        -DROBOTWEAX_SRT_EXPECT_AEAD_API_PREVIEW=ON)
endif()
foreach(flag_variable IN ITEMS
        C_FLAGS
        CXX_FLAGS
        EXE_LINKER_FLAGS)
    if(NOT "${ROBOTWEAX_SRT_${flag_variable}}" STREQUAL "")
        list(APPEND configure_command
            "-DCMAKE_${flag_variable}=${ROBOTWEAX_SRT_${flag_variable}}")
    endif()
endforeach()
run_checked("installed CMake package configuration" ${configure_command})

set(build_command
    "${CMAKE_COMMAND}" --build "${consumer_binary_directory}" --parallel)
if(NOT "${ROBOTWEAX_SRT_BUILD_CONFIG}" STREQUAL "")
    list(APPEND build_command --config "${ROBOTWEAX_SRT_BUILD_CONFIG}")
endif()
run_checked("installed CMake package consumer build" ${build_command})

set(runtime_directory
    "${stage_directory}/${ROBOTWEAX_SRT_INSTALL_BINDIR}")
set(test_command
    "${CMAKE_CTEST_COMMAND}" --test-dir "${consumer_binary_directory}"
    --output-on-failure)
if(NOT "${ROBOTWEAX_SRT_BUILD_CONFIG}" STREQUAL "")
    list(APPEND test_command -C "${ROBOTWEAX_SRT_BUILD_CONFIG}")
endif()
if(WIN32)
    set(original_path "$ENV{PATH}")
    set(ENV{PATH} "${runtime_directory};${original_path}")
    run_checked("installed CMake package consumer execution" ${test_command})
    set(ENV{PATH} "${original_path}")
else()
    run_checked("installed CMake package consumer execution"
        ${test_command})
endif()

if((ROBOTWEAX_SRT_SYSTEM_NAME STREQUAL "Linux" OR
        ROBOTWEAX_SRT_SYSTEM_NAME STREQUAL "Darwin") AND
        NOT "${ROBOTWEAX_SRT_PKG_CONFIG_EXECUTABLE}" STREQUAL "")
    set(pkgconfig_environment
        "PKG_CONFIG_PATH=${pkgconfig_directory}:$ENV{PKG_CONFIG_PATH}")
    run_checked("pkg-config metadata validation"
        "${CMAKE_COMMAND}" -E env "${pkgconfig_environment}"
        "${ROBOTWEAX_SRT_PKG_CONFIG_EXECUTABLE}"
        --validate robotweax-srt)
    run_checked("pkg-config version validation"
        "${CMAKE_COMMAND}" -E env "${pkgconfig_environment}"
        "${ROBOTWEAX_SRT_PKG_CONFIG_EXECUTABLE}"
        --atleast-version=${ROBOTWEAX_SRT_PACKAGE_VERSION} robotweax-srt)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "${pkgconfig_environment}"
            "${ROBOTWEAX_SRT_PKG_CONFIG_EXECUTABLE}"
            --cflags --libs --static robotweax-srt
        RESULT_VARIABLE pkgconfig_result
        OUTPUT_VARIABLE pkgconfig_flags
        ERROR_VARIABLE pkgconfig_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT pkgconfig_result EQUAL 0)
        message(FATAL_ERROR
            "pkg-config static flags failed (${pkgconfig_result})\n"
            "${pkgconfig_error}")
    endif()
    separate_arguments(pkgconfig_arguments UNIX_COMMAND "${pkgconfig_flags}")
    separate_arguments(consumer_cxx_arguments UNIX_COMMAND
        "${ROBOTWEAX_SRT_CXX_FLAGS}")
    separate_arguments(consumer_link_arguments UNIX_COMMAND
        "${ROBOTWEAX_SRT_EXE_LINKER_FLAGS}")
    if(ROBOTWEAX_SRT_AEAD_API_PREVIEW)
        list(APPEND consumer_cxx_arguments
            -DROBOTWEAX_SRT_EXPECT_AEAD_API_PREVIEW=1)
    endif()
    set(pkgconfig_consumer
        "${ROBOTWEAX_SRT_TEST_ROOT}/robotweax_srt_pkgconfig_consumer")
    run_checked("pkg-config consumer build"
        "${ROBOTWEAX_SRT_CXX_COMPILER}"
        ${consumer_cxx_arguments}
        "${ROBOTWEAX_SRT_CONSUMER_SOURCE_DIR}/pkgconfig_consumer.cpp"
        -o "${pkgconfig_consumer}"
        ${pkgconfig_arguments}
        ${consumer_link_arguments})
    if(ROBOTWEAX_SRT_SYSTEM_NAME STREQUAL "Darwin")
        set(pkgconfig_runtime_environment
            "DYLD_LIBRARY_PATH=${stage_directory}/${ROBOTWEAX_SRT_INSTALL_LIBDIR}:$ENV{DYLD_LIBRARY_PATH}")
    else()
        set(pkgconfig_runtime_environment
            "LD_LIBRARY_PATH=${stage_directory}/${ROBOTWEAX_SRT_INSTALL_LIBDIR}:$ENV{LD_LIBRARY_PATH}")
    endif()
    run_checked("pkg-config consumer execution"
        "${CMAKE_COMMAND}" -E env "${pkgconfig_runtime_environment}"
        "${pkgconfig_consumer}")

    # Exercise the public C recipe directly, without CMake's imported target
    # selecting the C++ linker or filling in missing dependencies for us.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "${pkgconfig_environment}"
            "${ROBOTWEAX_SRT_PKG_CONFIG_EXECUTABLE}"
            --cflags robotweax-srt
        RESULT_VARIABLE cflags_result
        OUTPUT_VARIABLE cflags
        ERROR_VARIABLE cflags_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT cflags_result EQUAL 0)
        message(FATAL_ERROR "pkg-config C flags failed: ${cflags_error}")
    endif()
    set(c_link_options --libs)
    set(c_link_driver "${ROBOTWEAX_SRT_C_COMPILER}")
    set(c_link_driver_flags "${ROBOTWEAX_SRT_C_FLAGS}")
    if(NOT ROBOTWEAX_SRT_BUILD_SHARED_LIBS)
        list(PREPEND c_link_options --static)
        set(c_link_driver "${ROBOTWEAX_SRT_CXX_COMPILER}")
        set(c_link_driver_flags "${ROBOTWEAX_SRT_CXX_FLAGS}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "${pkgconfig_environment}"
            "${ROBOTWEAX_SRT_PKG_CONFIG_EXECUTABLE}"
            ${c_link_options} robotweax-srt
        RESULT_VARIABLE c_libs_result
        OUTPUT_VARIABLE c_libs
        ERROR_VARIABLE c_libs_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT c_libs_result EQUAL 0)
        message(FATAL_ERROR "pkg-config C link flags failed: ${c_libs_error}")
    endif()
    separate_arguments(c_compile_arguments UNIX_COMMAND "${cflags}")
    separate_arguments(c_private_link_arguments UNIX_COMMAND "${c_libs}")
    separate_arguments(c_driver_arguments UNIX_COMMAND "${ROBOTWEAX_SRT_C_FLAGS}")
    separate_arguments(c_link_driver_arguments UNIX_COMMAND "${c_link_driver_flags}")
    if(ROBOTWEAX_SRT_AEAD_API_PREVIEW)
        list(APPEND c_driver_arguments -DROBOTWEAX_SRT_EXPECT_AEAD_API_PREVIEW=1)
    endif()
    set(c_object "${ROBOTWEAX_SRT_TEST_ROOT}/pkgconfig-c-consumer.o")
    set(c_executable "${ROBOTWEAX_SRT_TEST_ROOT}/pkgconfig-c-consumer")
    run_checked("pkg-config C source compilation"
        "${ROBOTWEAX_SRT_C_COMPILER}" ${c_driver_arguments}
        -std=c11 ${c_compile_arguments}
        -c "${ROBOTWEAX_SRT_CONSUMER_SOURCE_DIR}/c_consumer.c"
        -o "${c_object}")
    run_checked("pkg-config C object linkage"
        "${c_link_driver}" ${c_link_driver_arguments}
        "${c_object}" ${c_private_link_arguments} ${consumer_link_arguments}
        -o "${c_executable}")
    run_checked("pkg-config C consumer execution"
        "${CMAKE_COMMAND}" -E env "${pkgconfig_runtime_environment}"
        "${c_executable}")

    if(ROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT)
        run_checked("libsrt compatibility metadata validation"
            "${CMAKE_COMMAND}" -E env "${pkgconfig_environment}"
            "${ROBOTWEAX_SRT_PKG_CONFIG_EXECUTABLE}"
            --validate srt)
        run_checked("libsrt compatibility API version validation"
            "${CMAKE_COMMAND}" -E env "${pkgconfig_environment}"
            "${ROBOTWEAX_SRT_PKG_CONFIG_EXECUTABLE}"
            --exact-version=${ROBOTWEAX_SRT_COMPATIBLE_SRT_VERSION} srt)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "${pkgconfig_environment}"
                "${ROBOTWEAX_SRT_PKG_CONFIG_EXECUTABLE}"
                --variable=robotweax_release srt
            RESULT_VARIABLE release_result
            OUTPUT_VARIABLE reported_release
            ERROR_VARIABLE release_error
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT release_result EQUAL 0
                OR NOT reported_release STREQUAL
                    "${ROBOTWEAX_SRT_PACKAGE_VERSION}")
            message(FATAL_ERROR
                "srt.pc Robotweax release validation failed "
                "(${release_result}): '${reported_release}'\n${release_error}")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "${pkgconfig_environment}"
                "${ROBOTWEAX_SRT_PKG_CONFIG_EXECUTABLE}"
                --cflags --libs srt
            RESULT_VARIABLE compatibility_flags_result
            OUTPUT_VARIABLE compatibility_flags
            ERROR_VARIABLE compatibility_flags_error
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT compatibility_flags_result EQUAL 0)
            message(FATAL_ERROR
                "libsrt compatibility flags failed "
                "(${compatibility_flags_result})\n"
                "${compatibility_flags_error}")
        endif()
        separate_arguments(compatibility_arguments UNIX_COMMAND
            "${compatibility_flags}")
        separate_arguments(consumer_c_arguments UNIX_COMMAND
            "${ROBOTWEAX_SRT_C_FLAGS}")
        set(ffmpeg_configure_probe
            "${ROBOTWEAX_SRT_TEST_ROOT}/ffmpeg_configure_probe")
        run_checked("FFmpeg-style libsrt configure probe"
            "${ROBOTWEAX_SRT_C_COMPILER}"
            ${consumer_c_arguments}
            "${ROBOTWEAX_SRT_CONSUMER_SOURCE_DIR}/ffmpeg_configure_probe.c"
            -o "${ffmpeg_configure_probe}"
            ${compatibility_arguments}
            ${consumer_link_arguments})
        run_checked("FFmpeg-style libsrt configure probe execution"
            "${CMAKE_COMMAND}" -E env "${pkgconfig_runtime_environment}"
            "${ffmpeg_configure_probe}")
    endif()
endif()
