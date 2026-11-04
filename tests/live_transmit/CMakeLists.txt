cmake_minimum_required(VERSION 3.20)
project(robotweax_live_transmit LANGUAGES CXX)

# Standalone opt-in SDK consumer. The parent library never adds this directory.
find_package(RobotweaxSRT CONFIG REQUIRED)
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES RobotweaxSRT::srt)
check_cxx_source_compiles("#include <srt/srt.h>
int main() {
  int policy = ROBOTWEAX_SRT_FECPOLICY_ARQFIRST;
  return policy + int(SRTO_ROBOTWEAX_FECPOLICY) + int(SRTO_ROBOTWEAX_TAILPROBE);
}" ROBOTWEAX_APP_HAS_ARQFIRST_API)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT ROBOTWEAX_APP_HAS_ARQFIRST_API)
    message(FATAL_ERROR "Use a current Robotweax SDK with ARQ-first APIs and the listener capacity fix (PR #376)")
endif()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(HAIVISION_SRT_SOURCE_DIR "" CACHE PATH "Clean pinned Haivision source checkout")
if(NOT HAIVISION_SRT_SOURCE_DIR)
    message(FATAL_ERROR "Set HAIVISION_SRT_SOURCE_DIR to the pinned external checkout")
endif()
set(prepared "${CMAKE_CURRENT_BINARY_DIR}/prepared-source")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_source.py"
    "${HAIVISION_SRT_SOURCE_DIR}" "${prepared}" COMMAND_ERROR_IS_FATAL ANY)
set(app_sources apputil statswriter logsupport logsupport_appdefs socketoptions
    transmitmedia uriparser verbose srt-live-transmit)
list(TRANSFORM app_sources PREPEND "${prepared}/apps/")
list(TRANSFORM app_sources APPEND ".cpp")
add_executable(srt-live-transmit-robotweax ${app_sources})
target_link_libraries(srt-live-transmit-robotweax PRIVATE RobotweaxSRT::srt)
target_include_directories(srt-live-transmit-robotweax PRIVATE
    "${prepared}/apps" "${prepared}/srtcore")
target_compile_features(srt-live-transmit-robotweax PRIVATE cxx_std_20)
target_compile_definitions(srt-live-transmit-robotweax PRIVATE HAVE_CXX11=1 ENABLE_BONDING=0)
if(WIN32)
    target_compile_definitions(srt-live-transmit-robotweax PRIVATE
        _WIN32_WINNT=0x0601 HAVE_INET_PTON=1 NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(srt-live-transmit-robotweax PRIVATE ws2_32)
endif()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/app-path-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:srt-live-transmit-robotweax>\n")
# Intentionally no install/package target: upstream applications are local
# qualification inputs, not Robotweax SDK or downloadable workflow artifacts.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/provider-library-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:RobotweaxSRT::srt>\n")
