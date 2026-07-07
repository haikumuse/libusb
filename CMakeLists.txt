# CMake build for libusb event-abstraction-v4 (1.0.20-rc3, uwehermann/libusb fork)
#
# libusb ships autotools/msvc/xcode but no CMake. This file provides a minimal
# CMake build for static linking into libsigrok, replacing the system libusb
# 1.0.27 (MSYS2 package) which still uses the legacy pollfd model.
#
# event-abstraction-v4 adds an event abstraction layer (events_windows.c /
# threads_windows.c) using WaitForMultipleObjects() instead of pollfds, plus
# windows_hotplug.c for native Windows device hotplug notification. This
# eliminates the root cause of 24MHz streaming acquisition failures on Windows.
#
# Key differences vs libusb 1.0.30:
#   - Macro is OS_WINDOWS=1 (not PLATFORM_WINDOWS=1)
#   - OS sources: windows_usb.c + windows_hotplug.c
#     (no windows_common.c / windows_usbdk.c / windows_winusb.c)
#   - Platform sources: events_windows.c + threads_windows.c

cmake_minimum_required(VERSION 3.16)

# Let the parent project drive C/CXX standard, build type, etc.
project(libusb C)

# ---- Generate config.h (Windows-specific, based on msvc/config.h) ----
if(WIN32)
	# Mirror msvc/config.h minimal defines needed for MinGW builds.
	configure_file(
		"${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.in"
		"${CMAKE_CURRENT_BINARY_DIR}/config.h"
		ESCAPE_QUOTES
	)
endif()

# ---- Sources (see libusb/Makefile.am) ----
set(LIBUSB_CORE_SOURCES
	libusb/core.c
	libusb/descriptor.c
	libusb/hotplug.c
	libusb/io.c
	libusb/strerror.c
	libusb/sync.c
)

if(WIN32)
	list(APPEND LIBUSB_PLATFORM_SOURCES
		libusb/os/events_windows.c
		libusb/os/threads_windows.c
	)
	list(APPEND LIBUSB_OS_SOURCES
		libusb/os/windows_usb.c
		libusb/os/windows_hotplug.c
	)
else()
	list(APPEND LIBUSB_PLATFORM_SOURCES
		libusb/os/events_posix.c
		libusb/os/threads_posix.c
	)
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		list(APPEND LIBUSB_OS_SOURCES libusb/os/linux_usbfs.c)
		list(APPEND LIBUSB_OS_SOURCES libusb/os/linux_udev.c)
	elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
		list(APPEND LIBUSB_OS_SOURCES libusb/os/darwin_usb.c)
	elseif(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
		list(APPEND LIBUSB_OS_SOURCES libusb/os/netbsd_usb.c)
	elseif(CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
		list(APPEND LIBUSB_OS_SOURCES libusb/os/openbsd_usb.c)
	elseif(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
		list(APPEND LIBUSB_OS_SOURCES libusb/os/sunos_usb.c)
	else()
		list(APPEND LIBUSB_OS_SOURCES libusb/os/null_usb.c)
	endif()
endif()

# ---- Static library target ----
add_library(usb-1.0 STATIC
	${LIBUSB_CORE_SOURCES}
	${LIBUSB_PLATFORM_SOURCES}
	${LIBUSB_OS_SOURCES}
)

# event-abstraction-v4 uses OS_WINDOWS (not PLATFORM_WINDOWS) to gate the
# Windows backend. HAVE_CONFIG_H pulls in the generated config.h.
target_compile_definitions(usb-1.0 PRIVATE
	$<$<PLATFORM_ID:Windows>:
		HAVE_CONFIG_H=1
		OS_WINDOWS=1
		HAVE_OS_TIMER=1
		ENABLE_LOGGING=1
		ENABLE_DEBUG_LOGGING=1
		DEFAULT_VISIBILITY=
		_LIBUSB_INTERNAL=1
	>
)

# Use BEFORE so build/libusb/ (containing generated config.h) comes BEFORE
# parent scope's include_directories() which add PXView/ and libsigrokdecode/
# — both have their own config.h that would shadow libusb's.
target_include_directories(usb-1.0 BEFORE
	PRIVATE
		${CMAKE_CURRENT_BINARY_DIR}  # for generated config.h — MUST come first
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/libusb  # for libusb.h — before system libusb
)

# Windows backend links to setupapi + winusb
if(WIN32)
	if(MINGW)
		target_link_libraries(usb-1.0 PUBLIC setupapi winusb)
	else()
		find_library(SETUPAPI_LIB setupapi)
		find_library(WINUSB_LIB winusb)
		target_link_libraries(usb-1.0 PUBLIC ${SETUPAPI_LIB} ${WINUSB_LIB})
	endif()
endif()

# Hide symbols by default (matches -fvisibility=hidden in Makefile.am)
set_target_properties(usb-1.0 PROPERTIES
	C_VISIBILITY_PRESET hidden
	CXX_VISIBILITY_PRESET hidden
	POSITION_INDEPENDENT_CODE ON
)

# Alias target so callers can use either usb-1.0 or libusb::libusb
add_library(libusb::libusb ALIAS usb-1.0)
