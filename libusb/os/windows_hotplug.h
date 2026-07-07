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

#ifndef WINDOWS_HOTPLUG_H
#define WINDOWS_HOTPLUG_H

int windows_start_event_monitor(void);
int windows_stop_event_monitor(void);

void windows_initial_scan_devices(struct libusb_context *ctx);

/* Hotplug log callback — allows the application (PXView) to route libusb
 * hotplug backend debug messages into its own logging system.
 * level: 0=info, 1=warn, 2=err
 * The callback receives a fully-formatted, newline-terminated message. */
typedef void (*windows_hotplug_log_cb_t)(int level, const char *msg);
void windows_hotplug_set_log_cb(windows_hotplug_log_cb_t cb);

#endif
