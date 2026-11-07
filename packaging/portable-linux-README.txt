Lectern - portable build for Linux
==================================

Run ./lectern. There is nothing to install.

Your settings and the folders you have allowed Lectern to open are kept in the
"lectern-data" folder next to the "lectern" file (it is created on first run),
not in your home directory. Move or copy the whole folder, including
lectern-data, to take your setup with you. Delete both to remove every trace.

This build needs the desktop libraries to be present on the system:
GTK 4 and WebKitGTK 6.0 (Debian/Ubuntu: libgtk-4-1, libwebkitgtk-6.0-4;
Fedora: gtk4, webkitgtk6.0). It was built on Ubuntu 24.04, so it needs a
distribution at least that recent (glibc 2.39).

If you would rather not manage libraries, the .AppImage and the Flatpak are
the other portable choices; the .deb and .rpm are the installed ones.
