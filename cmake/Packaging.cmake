# CPack configuration: the installers the Tauri build used to produce.
#
#   Windows   NSIS  -> Lectern-<version>-windows-x64-setup.exe
#             WIX   -> Lectern-<version>-windows-x64.msi
#             ZIP   -> Lectern-<version>-windows-x64-portable.zip
#   Linux     DEB   -> lectern_<version>_amd64.deb
#             RPM   -> lectern-<version>.x86_64.rpm
#             TGZ   -> lectern-<version>-linux-x86_64.tar.gz
#   macOS     DragNDrop -> Lectern-<version>-macos.dmg
#
# AppImage is not a CPack generator; scripts/build-appimage.sh builds it from
# the staged install tree.

set(CPACK_PACKAGE_NAME "Lectern")
set(CPACK_PACKAGE_VENDOR "Decumano")
set(CPACK_PACKAGE_CONTACT "https://github.com/Decumano/Lectern-Desktop-Cpp")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Markdown-native office suite for worldbuilding")
set(CPACK_PACKAGE_HOMEPAGE_URL
    "https://github.com/Decumano/Lectern-Desktop-Cpp")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Lectern")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_EXECUTABLES "lectern" "Lectern")
set(CPACK_STRIP_FILES ON)

# Source archives would ship the build tree; only binary packages are wanted.
set(CPACK_SOURCE_GENERATOR "")

if(WIN32)
  set(CPACK_GENERATOR "NSIS;WIX;ZIP")
  set(CPACK_SYSTEM_NAME "windows-x64")

  # ── NSIS ──
  set(CPACK_NSIS_PACKAGE_NAME "Lectern")
  set(CPACK_NSIS_DISPLAY_NAME "Lectern")
  set(CPACK_NSIS_INSTALLED_ICON_NAME "lectern.exe")
  set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/lectern.ico")
  set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/lectern.ico")
  set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
  # So the updater's silent handoff replaces an existing install in place
  # rather than stacking a second copy beside it.
  set(CPACK_NSIS_MODIFY_PATH OFF)
  set(CPACK_NSIS_CREATE_ICONS_EXTRA
      "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Lectern.lnk' '$INSTDIR\\\\lectern.exe'")
  set(CPACK_NSIS_DELETE_ICONS_EXTRA
      "Delete '$SMPROGRAMS\\\\$START_MENU\\\\Lectern.lnk'")

  # ── WiX (MSI) ──
  # A stable UpgradeCode is what lets an .msi upgrade a previous install
  # instead of installing alongside it. Generated once; never change it.
  set(CPACK_WIX_UPGRADE_GUID "6F3B1A64-6E5E-4B2C-9A47-2E0B4C7D8E51")
  set(CPACK_WIX_PRODUCT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/lectern.ico")
  set(CPACK_WIX_PROPERTY_ARPHELPLINK "${CPACK_PACKAGE_HOMEPAGE_URL}")

elseif(APPLE)
  set(CPACK_GENERATOR "DragNDrop")
  set(CPACK_SYSTEM_NAME "macos")
  set(CPACK_DMG_VOLUME_NAME "Lectern")

else()
  set(CPACK_GENERATOR "DEB;RPM;TGZ")
  set(CPACK_SYSTEM_NAME "linux-x86_64")

  # ── Debian ──
  set(CPACK_DEBIAN_PACKAGE_NAME "lectern")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_SECTION "office")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Decumano")
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
  # Shared libraries are resolved from the built binary rather than listed by
  # hand, so a dependency can't silently go missing when vcpkg changes.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  # The webview itself is not a vcpkg dependency — it is the system's.
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "libwebkitgtk-6.0-4 | libwebkit2gtk-4.1-0")
  set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "zenity | kdialog")

  # ── RPM ──
  set(CPACK_RPM_PACKAGE_NAME "lectern")
  set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
  set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
  set(CPACK_RPM_PACKAGE_GROUP "Applications/Productivity")
  set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
  set(CPACK_RPM_PACKAGE_REQUIRES "webkitgtk6.0")
  # Directories owned by the base filesystem package must not be claimed.
  set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
      /usr/share/applications
      /usr/share/icons
      /usr/share/icons/hicolor
      /usr/share/icons/hicolor/32x32
      /usr/share/icons/hicolor/32x32/apps
      /usr/share/icons/hicolor/128x128
      /usr/share/icons/hicolor/128x128/apps
      /usr/share/icons/hicolor/256x256
      /usr/share/icons/hicolor/256x256/apps
      /usr/share/metainfo)
endif()

include(CPack)
