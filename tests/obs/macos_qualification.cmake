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
    # The pinned OBS 32.2.2 macOS bundle embeds librist but omits its direct
    # @rpath/libmbedcrypto.dylib dependency. Embed that pinned library in this
    # isolated desktop build so obs-ffmpeg can load from the app bundle.
    set(mbedcrypto "${ROBOTWEAX_OBS_MACOS_DEPS_PREFIX}/lib/libmbedcrypto.dylib")
    if(NOT EXISTS "${mbedcrypto}")
      message(FATAL_ERROR "missing pinned OBS libmbedcrypto.dylib: ${mbedcrypto}")
    endif()
    set_property(TARGET obs-studio APPEND PROPERTY XCODE_EMBED_FRAMEWORKS "${mbedcrypto}")
  endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL robotweax_obs_macos_qualification)
