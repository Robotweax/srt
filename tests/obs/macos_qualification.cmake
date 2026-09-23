# The pinned OBS MPEG-TS module uses POSIX address declarations without
# including their platform headers. Supply them in this isolated build only.
function(robotweax_obs_macos_qualification)
  target_compile_options(obs-ffmpeg PRIVATE "SHELL:-include netdb.h" "SHELL:-include arpa/inet.h")
  if(ROBOTWEAX_OBS_MACOS_DESKTOP)
    # OBS 32.2.2 uses CVDisplayLink in its Metal renderer. Xcode 26 marks
    # these calls deprecated and turns Swift warnings into build errors.
    # Keep every other target's warning policy and the complete GUI test intact.
    set_target_properties(
      libobs-metal PROPERTIES COMPILE_WARNING_AS_ERROR OFF
                              XCODE_ATTRIBUTE_GCC_TREAT_WARNINGS_AS_ERRORS NO
                              XCODE_ATTRIBUTE_SWIFT_TREAT_WARNINGS_AS_ERRORS NO
    )
  endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL robotweax_obs_macos_qualification)
