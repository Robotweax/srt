cmake_minimum_required(VERSION 3.20)
project(SRTMobileDemo LANGUAGES C CXX OBJCXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "This example requires an iOS device or simulator toolchain")
endif()
foreach(input SRT_ARCHIVE CRYPTO_ARCHIVE SRT_INCLUDE_DIR)
    if(NOT EXISTS "${${input}}")
        message(FATAL_ERROR "Provide ${input} for the same target SDK/architecture")
    endif()
endforeach()
add_executable(SRTMobileDemo MACOSX_BUNDLE main.mm)
target_include_directories(SRTMobileDemo PRIVATE "${SRT_INCLUDE_DIR}")
target_compile_definitions(SRTMobileDemo PRIVATE ENABLE_AEAD_API_PREVIEW)
target_compile_options(SRTMobileDemo PRIVATE -fobjc-arc)
target_link_libraries(SRTMobileDemo PRIVATE "${SRT_ARCHIVE}" "${CRYPTO_ARCHIVE}"
    "-framework UIKit" "-framework Foundation")
set_target_properties(SRTMobileDemo PROPERTIES
    OBJCXX_STANDARD 20
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    MACOSX_BUNDLE_GUI_IDENTIFIER "ai.robotweax.srt.mobile-demo")
