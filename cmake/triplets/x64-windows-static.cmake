set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

# This project is Release-only. The ffmpeg Debug backend (pulled in by
# qtmultimedia) fails to compile under the current MSVC/UCRT toolchain
# (stdlib.h getenv macro clash), so build only the Release variant.
set(VCPKG_BUILD_TYPE release)
