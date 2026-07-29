set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# Release-only. This is the host triplet used for Qt build tools
# (qtdeclarative, etc.) pulled in transitively by qtmultimedia. Their
# Debug builds fail to link under the current toolchain
# (LNK1168 on qmllsquickplugind.dll); the project consumes only Release
# libs, so skip Debug here too.
set(VCPKG_BUILD_TYPE release)
