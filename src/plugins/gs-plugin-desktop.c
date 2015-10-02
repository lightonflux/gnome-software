/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <config.h>

#include <gs-plugin.h>
#include <gio/gdesktopappinfo.h>

/**
 * gs_plugin_get_name:
 */
const gchar *
gs_plugin_get_name (void)
{
	return "desktop";
}

/**
 * gs_plugin_get_deps:
 */
const gchar **
gs_plugin_get_deps (GsPlugin *plugin)
{
	static const gchar *deps[] = {
		"steam",		/* this might handle */
		NULL };
	return deps;
}

/**
 * gs_plugin_app_launch:
 */
static gboolean
gs_plugin_desktop_app_launch_cb (gpointer user_data)
{
	GAppInfo *appinfo = G_APP_INFO (user_data);
	GdkDisplay *display;
	g_autoptr(GAppLaunchContext) context = NULL;
	g_autoptr(GError) error = NULL;

	display = gdk_display_get_default ();
	context = G_APP_LAUNCH_CONTEXT (gdk_display_get_app_launch_context (display));
	if (!g_app_info_launch (appinfo, NULL, context, &error))
		g_warning ("Failed to launch: %s", error->message);
	return G_SOURCE_REMOVE;
}

/**
 * gs_plugin_app_launch:
 */
gboolean
gs_plugin_app_launch (GsPlugin *plugin, GsApp *app,
		      GCancellable *cancellable, GError **error)
{
	const gchar *desktop_id;
	g_autoptr(GAppInfo) appinfo = NULL;

	/* not us */
	desktop_id = gs_app_get_id (app);
	if (desktop_id == NULL)
		return TRUE;

	/* not found */
	appinfo = G_APP_INFO (g_desktop_app_info_new (desktop_id));
	if (appinfo == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "no such desktop file: %s", desktop_id);
		return FALSE;
	}

	/* do this idle on the main thread */
	g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
			 gs_plugin_desktop_app_launch_cb,
			 g_object_ref (appinfo),
			 (GDestroyNotify) g_object_unref);
	return TRUE;
}
