# The pinned OBS MPEG-TS module uses POSIX address declarations without
# including their platform headers. Supply them in this isolated build only.
function(robotweax_obs_macos_qualification)
  target_compile_options(obs-ffmpeg PRIVATE "SHELL:-include netdb.h" "SHELL:-include arpa/inet.h")
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL robotweax_obs_macos_qualification)
