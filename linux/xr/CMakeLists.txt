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
target_link_libraries(plezy-xr PRIVATE PkgConfig::XR_OPENXR PkgConfig::XR_MPV PkgConfig::XR_EGL
                                       PkgConfig::XR_GLES Threads::Threads)
target_include_directories(plezy-xr PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(plezy-xr PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/intermediates_do_not_run")
