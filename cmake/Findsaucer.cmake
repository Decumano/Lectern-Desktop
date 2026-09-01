# vcpkg's saucer port installs headers and saucer.lib but ships no CMake
# package config, so `find_package(saucer CONFIG)` finds nothing. This module
# builds the imported target by hand, including the transitive pieces saucer's
# own CMakeLists would have propagated.
#
# Defines: saucer::saucer

find_path(SAUCER_INCLUDE_DIR
  NAMES saucer/smartview.hpp
)

# vcpkg keeps the debug build under `debug/lib`. A plain find_library picks
# whichever it hits first, which on a Release build means linking the debug
# runtime — so locate both explicitly and let CMake choose per configuration.
get_filename_component(_saucer_prefix "${SAUCER_INCLUDE_DIR}" DIRECTORY)

find_library(SAUCER_LIBRARY_RELEASE
  NAMES saucer
  PATHS "${_saucer_prefix}/lib"
  NO_DEFAULT_PATH
)

find_library(SAUCER_LIBRARY_DEBUG
  NAMES saucer
  PATHS "${_saucer_prefix}/debug/lib"
  NO_DEFAULT_PATH
)

# Fall back to a plain search for non-vcpkg installs.
if(NOT SAUCER_LIBRARY_RELEASE AND NOT SAUCER_LIBRARY_DEBUG)
  find_library(SAUCER_LIBRARY_RELEASE NAMES saucer)
endif()

set(SAUCER_LIBRARY "${SAUCER_LIBRARY_RELEASE}")
if(NOT SAUCER_LIBRARY)
  set(SAUCER_LIBRARY "${SAUCER_LIBRARY_DEBUG}")
endif()

# saucer's public headers include these directly. ereignis, poolparty, glaze
# and the boost header libraries land in the plain `include/` directory that
# SAUCER_INCLUDE_DIR already points at; the four below install into versioned
# subdirectories (include/eraser-2.3.0/...), so their targets are needed for
# the include path to resolve.
find_package(fmt CONFIG REQUIRED)
find_package(glaze CONFIG REQUIRED)
find_package(eraser CONFIG REQUIRED)
find_package(flagpp CONFIG REQUIRED)
find_package(lockpp CONFIG REQUIRED)
find_package(rebind CONFIG REQUIRED)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(saucer
  REQUIRED_VARS SAUCER_LIBRARY SAUCER_INCLUDE_DIR
)

if(saucer_FOUND AND NOT TARGET saucer::saucer)
  add_library(saucer::saucer UNKNOWN IMPORTED)
  set_target_properties(saucer::saucer PROPERTIES
    IMPORTED_LOCATION "${SAUCER_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SAUCER_INCLUDE_DIR}"
  )

  if(SAUCER_LIBRARY_RELEASE)
    set_property(TARGET saucer::saucer APPEND PROPERTY
      IMPORTED_CONFIGURATIONS RELEASE)
    set_target_properties(saucer::saucer PROPERTIES
      IMPORTED_LOCATION_RELEASE "${SAUCER_LIBRARY_RELEASE}")
  endif()

  if(SAUCER_LIBRARY_DEBUG)
    set_property(TARGET saucer::saucer APPEND PROPERTY
      IMPORTED_CONFIGURATIONS DEBUG)
    set_target_properties(saucer::saucer PROPERTIES
      IMPORTED_LOCATION_DEBUG "${SAUCER_LIBRARY_DEBUG}")
  endif()

  target_link_libraries(saucer::saucer INTERFACE
    fmt::fmt
    glaze::glaze
    cr::eraser
    cr::flagpp
    cr::lockpp
    cr::rebind
  )

  if(WIN32)
    find_package(unofficial-webview2 CONFIG REQUIRED)
    # WebView2 is the Windows backend; the rest are what its COM and shell
    # calls resolve against.
    target_link_libraries(saucer::saucer INTERFACE
      unofficial::webview2::webview2
      # saucer's Win32 icon/window code decodes images through GDI+.
      Gdiplus
      Shlwapi
      Shcore
      Dwmapi
      Ole32
      OleAut32
      Gdi32
      User32
    )
  elseif(APPLE)
    target_link_libraries(saucer::saucer INTERFACE
      "-framework WebKit"
      "-framework Cocoa"
    )
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBKITGTK REQUIRED IMPORTED_TARGET webkitgtk-6.0)
    target_link_libraries(saucer::saucer INTERFACE PkgConfig::WEBKITGTK)

    # saucer's GTK backend calls into libadwaita (adw_application_new,
    # adw_header_bar_new, adw_style_manager_get_default). webkitgtk-6.0.pc
    # brings in its headers but not the library, so the final link has to name
    # it. Optional rather than REQUIRED: a saucer built without libadwaita
    # present references none of those symbols.
    pkg_check_modules(ADWAITA IMPORTED_TARGET libadwaita-1)
    if(TARGET PkgConfig::ADWAITA)
      target_link_libraries(saucer::saucer INTERFACE PkgConfig::ADWAITA)
    endif()
  endif()
endif()

mark_as_advanced(SAUCER_INCLUDE_DIR SAUCER_LIBRARY)
