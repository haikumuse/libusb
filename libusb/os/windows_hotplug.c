/*
 * windows hotplug backend for libusb 1.0
 * Copyright © 2024 Sylvain Fasel <sylvain@sonatique.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <dbt.h>

#include "libusbi.h"
#include "threads_windows.h"

/* windows_usb.h defines GUID_DEVINTERFACE_USB_DEVICE and 3 other GUIDs as
 * actual global variables (const GUID ... = {...}) guarded by
 * #if !defined(GUID_DEVINTERFACE_*). windows_usb.c already includes this
 * header and defines them; to avoid multiple-definition linker errors, we
 * define the guard macros here so the #if !defined() check skips the
 * definition, and provide extern declarations for the variables. */
#define GUID_DEVINTERFACE_USB_HOST_CONTROLLER GUID_DEVINTERFACE_USB_HOST_CONTROLLER
#define GUID_DEVINTERFACE_USB_DEVICE GUID_DEVINTERFACE_USB_DEVICE
#define GUID_DEVINTERFACE_USB_HUB GUID_DEVINTERFACE_USB_HUB
#define GUID_DEVINTERFACE_LIBUSB0_FILTER GUID_DEVINTERFACE_LIBUSB0_FILTER

#include "windows_usb.h"
#include "windows_hotplug.h"

#include "hotplug.h"

/* extern declarations for the GUIDs defined in windows_usb.c */
extern const GUID GUID_DEVINTERFACE_USB_DEVICE;

/* Hotplug log callback — set by the application (PXView) to route libusb
 * hotplug debug messages into the application's logging system. When NULL
 * (default), messages fall back to OutputDebugStringW on Windows. */
typedef void (*hotplug_log_cb_t)(int level, const char *msg);
static hotplug_log_cb_t g_hotplug_log_cb = NULL;

void windows_hotplug_set_log_cb(hotplug_log_cb_t cb)
{
	g_hotplug_log_cb = cb;
}

/* Debug output helper. level: 0=info, 1=warn, 2=err */
static void hotplug_log(int level, const char *fmt, ...)
{
	char buf[512];
	int n;
	va_list args;
	va_start(args, fmt);
	n = vsnprintf(buf, sizeof(buf)-2, fmt, args);
	va_end(args);
	if (n < 0) n = 0;
	if (n > (int)sizeof(buf)-2) n = sizeof(buf)-2;
	buf[n] = '\n';
	buf[n+1] = '\0';

	if (g_hotplug_log_cb) {
		g_hotplug_log_cb(level, buf);
	} else {
		/* Fallback: OutputDebugStringW (visible in DebugView) */
		WCHAR wbuf[600];
		MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, sizeof(wbuf)/sizeof(wbuf[0]));
		OutputDebugStringW(wbuf);
	}
}

/* event-abstraction-v4 adaptations:
 * - windows_get_device_list is the 2-param variant (ctx, &_discdevs); the
 *   static helper that delegated to usbi_backend->get_device_list has been
 *   removed because the hotplug capability check in core.c keys off
 *   usbi_backend->get_device_list being NULL.
 * - windows_error_str lives in windows_usb.c (there is no windows_common.c).
 */
extern int windows_get_device_list(struct libusb_context *ctx, struct discovered_devs **_discdevs);
extern char *windows_error_str(uint32_t retval);

/* The Windows Hotplug system is a three steps process.
 * 1. We create a monitor on GUID_DEVINTERFACE_USB_DEVICE via a hidden window.
 * 2. Upon notification of an event, we run the current windows backend to get
 *    the list of all devices.
 * 3. We use PDEV_BROADCAST_DEVICEINTERFACE->dbcc_name for finding device object
 *    in this list and generate LEFT or ARRIVED events libusb client via hotplug callbacks
 */

static HWND windows_event_hwnd;
static HANDLE windows_event_thread_handle;
static DWORD WINAPI windows_event_thread_main(LPVOID lpParam);
static LRESULT CALLBACK windows_proc_callback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

#define log_error(operation) do { \
	usbi_err(NULL, "%s failed with error: %s", operation, windows_error_str(0)); \
} while (0)

int windows_start_event_monitor(void)
{
	hotplug_log(0, "starting event monitor thread");
	windows_event_thread_handle = CreateThread(
		NULL, // Default security descriptor
		0, // Default stack size
		windows_event_thread_main,
		NULL, // No parameters to pass to the thread
		0, // Start immediately
		NULL // No need to keep track of thread ID
	);

	if (windows_event_thread_handle == NULL)
	{
		log_error("CreateThread");
		return LIBUSB_ERROR_OTHER;
	}

	hotplug_log(0, "event monitor thread started, handle=%p", windows_event_thread_handle);
	return LIBUSB_SUCCESS;
}

int windows_stop_event_monitor(void)
{
	if (windows_event_hwnd == NULL)
	{
		return LIBUSB_SUCCESS;
	}

	if (!SUCCEEDED(SendMessage(windows_event_hwnd, WM_CLOSE, 0, 0)))
	{
		log_error("SendMessage");
		return LIBUSB_ERROR_OTHER;
	}

	if (WaitForSingleObject(windows_event_thread_handle, INFINITE) != WAIT_OBJECT_0)
	{
		log_error("WaitForSingleObject");
		return LIBUSB_ERROR_OTHER;
	}

	if (!CloseHandle(windows_event_thread_handle))
	{
		log_error("CloseHandle");
		return LIBUSB_ERROR_OTHER;
	}

	return LIBUSB_SUCCESS;
}

void windows_initial_scan_devices(struct libusb_context *ctx)
{
	/* NOTE: callers (windows_hotplug_poll) already hold active_contexts_lock;
	 * do NOT re-lock here — usbi_mutex_static_t is non-recursive and would
	 * deadlock. This function only operates on per-context state, not on
	 * active_contexts_list, so no list lock is needed. */
	hotplug_log(0, "initial scan devices for ctx %p", ctx);
	struct discovered_devs *discdevs = discovered_devs_alloc();
	if (!discdevs) {
		usbi_err(ctx, "hotplug initial scan: failed to allocate discdevs");
		return;
	}
	const int ret = windows_get_device_list(ctx, &discdevs);
	discovered_devs_free(discdevs);
	if (ret != LIBUSB_SUCCESS)
	{
		usbi_err(ctx, "hotplug failed to retrieve initial list with error: %s", libusb_error_name(ret));
	} else {
		hotplug_log(0, "initial scan completed for ctx %p", ctx);
	}
}

/* Normalize a Windows device path for case-insensitive comparison.
 * WM_DEVICECHANGE delivers '\\?\USB#VID_xxxx&PID_xxxx#...' (symbolic link
 * path) while libusb internally stores '\\.\USB#VID_xxxx&PID_xxxx#...'
 * (WinUSB device path). The two prefixes refer to the same device, so strip
 * either prefix (4 chars) and compare the remainder case-insensitively.
 * Also handles NULL paths (returns empty string). */
static const char *normalize_device_path(const char *path)
{
	static const char empty[] = "";
	if (!path)
		return empty;
	if (strncmp(path, "\\\\?\\", 4) == 0)
		return path + 4;
	if (strncmp(path, "\\\\.\\", 4) == 0)
		return path + 4;
	return path;
}

static void windows_refresh_device_list(struct libusb_context *ctx, const bool device_arrived, const char* device_name)
{
	hotplug_log(0, "refresh device list, arrived=%d, device_name='%s'", device_arrived, device_name);

	struct discovered_devs *discdevs = discovered_devs_alloc();
	if (!discdevs) {
		usbi_err(ctx, "hotplug refresh: failed to allocate discdevs");
		return;
	}
	const int ret = windows_get_device_list(ctx, &discdevs);
	discovered_devs_free(discdevs);
	if (ret != LIBUSB_SUCCESS)
	{
		usbi_err(ctx, "hotplug failed to retrieve current list with error: %s", libusb_error_name(ret));
		return;
	}

	const char *norm_target = normalize_device_path(device_name);
	hotplug_log(0, "normalized target path='%s'", norm_target);

	struct libusb_device *dev, *next_dev;
	struct windows_device_priv *priv;
	int dev_count = 0;
	int matched = 0;

	usbi_mutex_lock(&ctx->usb_devs_lock);
	list_for_each_entry_safe(dev, next_dev, &ctx->usb_devs, list, struct libusb_device)
	{
		priv = _device_priv(dev);
		dev_count++;

		/* Skip NULL-path entries (uninitialized or stale slots). */
		if (!priv->path)
			continue;

		hotplug_log(0, "ctx->usb_devs[%d] path='%s'", dev_count-1, priv->path);

		const char *norm_dev = normalize_device_path(priv->path);
		if (_stricmp(norm_dev, norm_target) != 0)
		{
			continue;
		}

		matched++;
		hotplug_log(0, "matched device path='%s', arrived=%d", priv->path, device_arrived);

		if (device_arrived)
		{
			/* Notify without holding usb_devs_lock to avoid lock ordering issues
			 * with the hotplug callback path. */
			hotplug_log(0, "ARRIVED: calling usbi_hotplug_notification for dev=%p", dev);
			usbi_mutex_unlock(&ctx->usb_devs_lock);
			usbi_hotplug_notification(ctx, dev, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED);
			usbi_mutex_lock(&ctx->usb_devs_lock);
			hotplug_log(0, "ARRIVED: usbi_hotplug_notification done for dev=%p", dev);
		}
		else
		{
			/* usbi_disconnect_device() takes care of its own locking; drop the
			 * usb_devs_lock around the call to avoid recursive locking. */
			hotplug_log(0, "LEFT: calling usbi_disconnect_device for dev=%p", dev);
			usbi_mutex_unlock(&ctx->usb_devs_lock);
			usbi_disconnect_device(dev);
			usbi_mutex_lock(&ctx->usb_devs_lock);
			hotplug_log(0, "LEFT: usbi_disconnect_device done for dev=%p", dev);
		}
	}
	usbi_mutex_unlock(&ctx->usb_devs_lock);

	if (!matched) {
		hotplug_log(1, "WARNING: no device matched '%s' (checked %d devices)", device_name, dev_count);
	} else {
		hotplug_log(0, "matched %d device(s) for '%s'", matched, device_name);
	}
}

static void windows_refresh_device_list_for_all_ctx(const bool device_arrived, const char* device_name)
{
	usbi_mutex_static_lock(&active_contexts_lock);

	struct libusb_context *ctx;
	list_for_each_entry(ctx, &active_contexts_list, list, struct libusb_context)
	{
		windows_refresh_device_list(ctx, device_arrived, device_name);
	}

	usbi_mutex_static_unlock(&active_contexts_lock);
}

#define WND_CLASS_NAME TEXT("libusb-1.0-windows-hotplug")

static bool init_wnd_class(void)
{
	WNDCLASS wndClass = { 0 };
	wndClass.lpfnWndProc = windows_proc_callback;
	wndClass.hInstance = GetModuleHandle(NULL);
	wndClass.lpszClassName = WND_CLASS_NAME;

	if (!RegisterClass(&wndClass))
	{
		log_error("event thread: RegisterClass");
		return false;
	}

	return true;
}

static DWORD WINAPI windows_event_thread_main(LPVOID lpParam)
{
	UNUSED(lpParam);

	hotplug_log(0, "windows event thread entering");

	if (!init_wnd_class())
	{
		hotplug_log(2, "ERROR: init_wnd_class failed");
		return (DWORD)-1;
	}

	windows_event_hwnd = CreateWindow(
		WND_CLASS_NAME,
		TEXT(""),
		0,
		0, 0, 0, 0,
		NULL, NULL,
		GetModuleHandle(NULL),
		NULL);

	if (windows_event_hwnd == NULL)
	{
		log_error("event thread: CreateWindow");
		return (DWORD)-1;
	}

	hotplug_log(0, "event window created, hwnd=%p", windows_event_hwnd);

	MSG msg;
	BOOL ret_val;

	while ((ret_val = GetMessage(&msg, windows_event_hwnd, 0, 0)) != 0)
	{
		if (ret_val == -1)
		{
			log_error("event thread: GetMessage");
			break;
		}

		if (!SUCCEEDED(TranslateMessage(&msg)))
		{
			log_error("event thread: TranslateMessage");
		}

		if (!SUCCEEDED(DispatchMessage(&msg)))
		{
			log_error("event thread: DispatchMessage");
		}
	}

	usbi_dbg("windows event thread exiting");

	return 0;
}

static bool register_device_interface_to_window_handle(
	IN GUID interface_class_guid,
	IN HWND hwnd,
	OUT HDEVNOTIFY* device_notify_handle)
{
	DEV_BROADCAST_DEVICEINTERFACE notificationFilter = { 0 };
	notificationFilter.dbcc_size = sizeof(notificationFilter);
	notificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
	notificationFilter.dbcc_classguid = interface_class_guid;

	*device_notify_handle = RegisterDeviceNotification(
		hwnd,
		&notificationFilter,
		DEVICE_NOTIFY_WINDOW_HANDLE
	);

	if (*device_notify_handle == NULL)
	{
		log_error("register_device_interface_to_window_handle");
		return false;
	}

	return true;
}

static LRESULT CALLBACK windows_proc_callback(
	HWND hwnd,
	UINT message,
	WPARAM wParam,
	LPARAM lParam)
{
	UNUSED(lParam);

	static HDEVNOTIFY device_notify_handle;

	switch (message)
	{
	case WM_CREATE:
		if (!register_device_interface_to_window_handle(
			GUID_DEVINTERFACE_USB_DEVICE,
			hwnd,
			&device_notify_handle))
		{
			return -1;
		}
		return 0;

	case WM_DEVICECHANGE:
		switch (wParam)
		{
		case DBT_DEVICEARRIVAL:
		case DBT_DEVICEREMOVECOMPLETE:
			hotplug_log(0, "WM_DEVICECHANGE wParam=%s", (wParam == DBT_DEVICEARRIVAL) ? "ARRIVAL" : "REMOVE");
			if (((PDEV_BROADCAST_HDR)lParam)->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
			{
#ifdef UNICODE
				char* device_name = NULL;
				const WCHAR* w_dbcc_name = ((PDEV_BROADCAST_DEVICEINTERFACE)lParam)->dbcc_name;

				const int len = WideCharToMultiByte(CP_UTF8, 0, w_dbcc_name, -1, NULL, 0, NULL, NULL);
			    if (len == 0)
				{
			        log_error("Conversion length calculation failed for ((PDEV_BROADCAST_DEVICEINTERFACE)lParam)->dbcc_name conversion from wchar to char");
			    }
				else
				{
					device_name = (char *)malloc(len);
				    if (device_name == NULL)
					{
				        log_error("Memory allocation failed for ((PDEV_BROADCAST_DEVICEINTERFACE)lParam)->dbcc_name conversion from wchar to char");
				    }
					else
					{
					    const int result = WideCharToMultiByte(CP_UTF8, 0, w_dbcc_name, -1, device_name, len, NULL, NULL);
					    if (result == 0)
						{
					        log_error("Conversion failed for ((PDEV_BROADCAST_DEVICEINTERFACE)lParam)->dbcc_name conversion from wchar to char");
					        free(device_name);
							device_name = NULL;
					    }
					}
    			}
#else
				const char* device_name = ((PDEV_BROADCAST_DEVICEINTERFACE)lParam)->dbcc_name;
#endif

				windows_refresh_device_list_for_all_ctx(wParam == DBT_DEVICEARRIVAL ? true : false, device_name);

#ifdef UNICODE
				free(device_name);
#endif
				return TRUE;
			}
			break;
		}
		return BROADCAST_QUERY_DENY;

	case WM_CLOSE:
		if (!UnregisterDeviceNotification(device_notify_handle))
		{
			log_error("UnregisterDeviceNotification");
		}
		if (!DestroyWindow(hwnd))
		{
			log_error("DestroyWindow");
		}
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	default:
		return DefWindowProc(hwnd, message, wParam, lParam);
	}
}
