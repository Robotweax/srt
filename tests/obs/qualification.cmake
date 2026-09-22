# Keep every installed consumer on the isolated dependency prefixes. OBS's
# target helpers otherwise replace CMAKE_INSTALL_RPATH on these targets.
function(robotweax_obs_qualification)
  # The pinned consumer uses POSIX resolver/address declarations without its
  # own includes. Supply them explicitly instead of relying on SRT transitive
  # includes or changing the transport's installed public header contract.
  target_compile_options(obs-ffmpeg PRIVATE "SHELL:-include netdb.h" "SHELL:-include arpa/inet.h")
  foreach(target libobs libobs-opengl obs-ffmpeg obs-x264 obs-ffmpeg-mux obs-frontend-api)
    set_property(TARGET ${target} PROPERTY INSTALL_RPATH "${CMAKE_INSTALL_RPATH}")
  endforeach()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL robotweax_obs_qualification)
