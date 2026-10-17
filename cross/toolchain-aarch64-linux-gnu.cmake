# CMake cross-toolchain file for building shell_app for aarch64-linux-gnu
# from an x86_64 host, using the compiler/libs cross/Dockerfile installs.
#
# By default resolves headers/libraries against the image's own Ubuntu
# :arm64 multiarch packages. If TARGET_ROOTFS is set (an already-extracted
# directory -- e.g. the real target device's rootfs, which may not match
# Ubuntu's library versions/ABI), that takes priority instead.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(_triple aarch64-linux-gnu)

set(CMAKE_C_COMPILER ${_triple}-gcc)
set(CMAKE_CXX_COMPILER ${_triple}-g++)
set(CMAKE_LIBRARY_ARCHITECTURE ${_triple})

# Ubuntu/Debian don't ship a "<triple>-pkg-config" cross wrapper by default
# (verified: crossbuild-essential-arm64 alone doesn't provide one) -- plain
# pkg-config/pkgconf works fine for cross targets as long as
# PKG_CONFIG_LIBDIR points it at the right arch's .pc files instead of the
# host's, and PKG_CONFIG_PATH is cleared so nothing host-side leaks in.
if(DEFINED ENV{TARGET_ROOTFS} AND NOT "$ENV{TARGET_ROOTFS}" STREQUAL "")
  set(CMAKE_SYSROOT "$ENV{TARGET_ROOTFS}")
  set(CMAKE_FIND_ROOT_PATH "$ENV{TARGET_ROOTFS}")
  set(ENV{PKG_CONFIG_SYSROOT_DIR} "$ENV{TARGET_ROOTFS}")
  set(ENV{PKG_CONFIG_LIBDIR}
      "$ENV{TARGET_ROOTFS}/usr/lib/${_triple}/pkgconfig:$ENV{TARGET_ROOTFS}/usr/lib/pkgconfig:$ENV{TARGET_ROOTFS}/usr/share/pkgconfig")
else()
  set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/${_triple}/pkgconfig:/usr/share/pkgconfig")
endif()
set(ENV{PKG_CONFIG_PATH} "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
