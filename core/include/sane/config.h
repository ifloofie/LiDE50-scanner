/* Hand-written config.h for the LiDE 50 Windows x64 build (MinGW-w64). */
#ifndef LIDE50_SANE_CONFIG_H
#define LIDE50_SANE_CONFIG_H

#define PACKAGE "sane-backends"
#define PACKAGE_STRING "sane-backends 1.2.1"
#define VERSION "1.2.1"
#define SANE_DLL_V_MAJOR 1
#define V_MAJOR 1
#define V_MINOR 0
#define SANE_CONFIG_DIR "."
#define PATH_SANE_CONFIG_DIR "."
#define PATH_SEP '\\'
#define DIR_SEP ";"

#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIBUSB 1

#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 260
#endif

#endif
