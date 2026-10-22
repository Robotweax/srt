add_library(robotweax_srt_extension_prototypes STATIC congestion.cpp)
target_compile_features(robotweax_srt_extension_prototypes PUBLIC cxx_std_20)
target_include_directories(robotweax_srt_extension_prototypes PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(robotweax_srt_extension_prototypes PRIVATE /W4 /WX)
else()
    target_compile_options(robotweax_srt_extension_prototypes PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
if(ROBOTWEAX_SRT_BUILD_TESTS)
    add_executable(robotweax_srt_extension_contract_tests
        ${PROJECT_SOURCE_DIR}/tests/test_main.cpp
        test_congestion.cpp test_provider_execution.cpp)
    target_include_directories(robotweax_srt_extension_contract_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(robotweax_srt_extension_contract_tests PRIVATE
        robotweax_srt_extension_prototypes Threads::Threads)
    add_test(NAME robotweax_srt_extension_contract_tests
        COMMAND robotweax_srt_extension_contract_tests)
    set_tests_properties(robotweax_srt_extension_contract_tests PROPERTIES
        TIMEOUT 30 LABELS "experimental;extensions;controllers")
endif()
