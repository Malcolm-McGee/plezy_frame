cmake_minimum_required(VERSION 3.13)
project(plezy_xr LANGUAGES CXX)

# plezy-xr: the OpenXR VR cinema helper (Steam Frame and other OpenXR headsets).
# A standalone executable launched by the app; it shares nothing with the
# Flutter runner except libmpv. Included from linux/CMakeLists.txt only when
# OpenXR is available (see PLEZY_BUILD_XR there).
pkg_check_modules(XR_OPENXR REQUIRED IMPORTED_TARGET openxr)
pkg_check_modules(XR_MPV REQUIRED IMPORTED_TARGET mpv)
pkg_check_modules(XR_EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(XR_GLES REQUIRED IMPORTED_TARGET glesv2)
find_package(Threads REQUIRED)

add_executable(plezy-xr "xr_main.cc")
target_compile_features(plezy-xr PRIVATE cxx_std_17)
# OpenXR's `{XR_TYPE_X}` initialiser idiom leaves the other members zeroed on
# purpose, which -Wextra reports as missing initialisers.
target_compile_options(plezy-xr PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
# SteamOS ships only a stub libopenxr_loader.a, not a real loader, but SteamVR
# bundles one (/opt/steamvr/bin/linuxarm64/libopenxr_loader.so). Point this at
# it to link against that instead of whatever pkg-config reports.
set(PLEZY_OPENXR_LOADER "" CACHE FILEPATH "Path to a libopenxr_loader.so to link instead of the pkg-config one")
if(NOT PLEZY_OPENXR_LOADER)
  # No shared loader on the system (SteamOS) but SteamVR bundles one: use it.
  find_library(PLEZY_SYSTEM_OPENXR_LOADER NAMES libopenxr_loader.so)
  if(NOT PLEZY_SYSTEM_OPENXR_LOADER AND EXISTS "/opt/steamvr/bin/linuxarm64/libopenxr_loader.so")
    set(PLEZY_OPENXR_LOADER "/opt/steamvr/bin/linuxarm64/libopenxr_loader.so")
    message(STATUS "Using SteamVR's OpenXR loader: ${PLEZY_OPENXR_LOADER}")
  endif()
endif()
if(PLEZY_OPENXR_LOADER)
  target_include_directories(plezy-xr PRIVATE ${XR_OPENXR_INCLUDE_DIRS})
  target_link_libraries(plezy-xr PRIVATE "${PLEZY_OPENXR_LOADER}")
  # The installed binary's search path is rewritten to INSTALL_RPATH, which would
  # drop the loader's directory. Keep it, after the bundle's own lib/, so the
  # helper still finds SteamVR's loader once it has been copied out of the build tree.
  get_filename_component(PLEZY_OPENXR_LOADER_DIR "${PLEZY_OPENXR_LOADER}" DIRECTORY)
  set_target_properties(plezy-xr PROPERTIES INSTALL_RPATH "$ORIGIN/lib;${PLEZY_OPENXR_LOADER_DIR}")
else()
  target_link_libraries(plezy-xr PRIVATE PkgConfig::XR_OPENXR)
endif()
target_link_libraries(plezy-xr PRIVATE PkgConfig::XR_MPV PkgConfig::XR_EGL PkgConfig::XR_GLES Threads::Threads)
# DT_RPATH (unlike the default DT_RUNPATH) also applies to libmpv's own
# dependencies - Lua and MuJS - so the bundled copies are found when the helper
# is started directly rather than through plezy.sh.
target_link_options(plezy-xr PRIVATE -Wl,--disable-new-dtags)
target_include_directories(plezy-xr PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(plezy-xr PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/intermediates_do_not_run")
