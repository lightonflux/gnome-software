/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013-2015 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2013 Matthias Clasen <mclasen@redhat.com>
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

#include "config.h"

#include <string.h>
#include <glib/gi18n.h>
#include <packagekit-glib2/packagekit.h>
#include <gsettings-desktop-schemas/gdesktop-enums.h>

#include "gs-update-monitor.h"
#include "gs-plugin-loader.h"
#include "gs-utils.h"
#include "gs-offline-updates.h"

struct _GsUpdateMonitor {
	GObject		 parent;

	GApplication	*application;
	GCancellable    *cancellable;
	GSettings	*settings;
	GsPluginLoader	*plugin_loader;

	guint		 cleanup_notifications_id;	/* at startup */
	guint		 check_startup_id;		/* 60s after startup */
	guint		 check_hourly_id;		/* and then every hour */
	guint		 check_daily_id;		/* every 3rd day */
	PkControl	*control;			/* network type detection */
	guint		 notification_blocked_id;	/* rate limit notifications */
};

G_DEFINE_TYPE (GsUpdateMonitor, gs_update_monitor, G_TYPE_OBJECT)

static gboolean
reenable_offline_update_notification (gpointer data)
{
	GsUpdateMonitor *monitor = data;
	monitor->notification_blocked_id = 0;
	return G_SOURCE_REMOVE;
}

static void
notify_offline_update_available (GsUpdateMonitor *monitor)
{
	const gchar *title;
	const gchar *body;
	guint64 elapsed_security = 0;
	guint64 security_timestamp = 0;
	g_autoptr(GNotification) n = NULL;

	if (gs_application_has_active_window (GS_APPLICATION (monitor->application)))
		return;
	if (monitor->notification_blocked_id > 0)
		return;

	/* rate limit update notifications to once per hour */
	monitor->notification_blocked_id = g_timeout_add_seconds (3600, reenable_offline_update_notification, monitor);

	/* get time in days since we saw the first unapplied security update */
	g_settings_get (monitor->settings,
			"security-timestamp", "x", &security_timestamp);
	if (security_timestamp > 0) {
		elapsed_security = g_get_monotonic_time () - security_timestamp;
		elapsed_security /= G_USEC_PER_SEC;
		elapsed_security /= 60 * 60 * 24;
	}

	/* only show the scary warning after the user has ignored
	 * security updates for a full day */
	if (elapsed_security > 1) {
		title = _("Security Updates Pending");
		body = _("It is recommended that you install important updates now");
		n = g_notification_new (title);
		g_notification_set_body (n, body);
		g_notification_add_button (n, _("Restart & Install"), "app.reboot-and-install");
		g_notification_set_default_action_and_target (n, "app.set-mode", "s", "updates");
		g_application_send_notification (monitor->application, "updates-available", n);
	} else {
		title = _("Software Updates Available");
		body = _("Important OS and application updates are ready to be installed");
		n = g_notification_new (title);
		g_notification_set_body (n, body);
		g_notification_add_button (n, _("Not Now"), "app.nop");
		g_notification_add_button_with_target (n, _("View"), "app.set-mode", "s", "updates");
		g_notification_set_default_action_and_target (n, "app.set-mode", "s", "updates");
		g_application_send_notification (monitor->application, "updates-available", n);
	}
}

static gboolean
has_important_updates (GsAppList *apps)
{
	GList *l;
	GsApp *app;

	for (l = apps; l != NULL; l = l->next) {
		app = GS_APP (l->data);
		if (gs_app_get_update_severity (app) == GS_APP_UPDATE_SEVERITY_SECURITY ||
		    gs_app_get_update_severity (app) == GS_APP_UPDATE_SEVERITY_IMPORTANT)
			return TRUE;
	}

	return FALSE;
}

static gboolean
no_updates_for_a_week (GsUpdateMonitor *monitor)
{
	GTimeSpan d;
	gint64 tmp;
	g_autoptr(GDateTime) last_update = NULL;
	g_autoptr(GDateTime) now = NULL;

	g_settings_get (monitor->settings, "install-timestamp", "x", &tmp);
	if (tmp == 0)
		return TRUE;

	last_update = g_date_time_new_from_unix_local (tmp);
	if (last_update == NULL) {
		g_warning ("failed to set timestamp %" G_GINT64_FORMAT, tmp);
		return TRUE;
	}

	now = g_date_time_new_now_local ();
	d = g_date_time_difference (now, last_update);
	if (d >= 7 * G_TIME_SPAN_DAY)
		return TRUE;

	return FALSE;
}

static void
get_updates_finished_cb (GObject *object,
			 GAsyncResult *res,
			 gpointer data)
{
	GsUpdateMonitor *monitor = data;
	GList *l;
	GsApp *app;
	guint64 security_timestamp = 0;
	guint64 security_timestamp_old = 0;
	g_autoptr(GError) error = NULL;
	g_autoptr(PkError) error_code = NULL;
	g_autoptr(GsAppList) apps = NULL;

	/* get result */
	apps = gs_plugin_loader_get_updates_finish (GS_PLUGIN_LOADER (object), res, &error);
	if (apps == NULL) {
		g_debug ("no updates; withdrawing updates-available notification");
		g_application_withdraw_notification (monitor->application,
						     "updates-available");
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("failed to get updates: %s", error->message);
		return;
	}

	/* find security updates, or clear timestamp if there are now none */
	g_settings_get (monitor->settings,
			"security-timestamp", "x", &security_timestamp_old);
	for (l = apps; l != NULL; l = l->next) {
		app = GS_APP (l->data);
		if (gs_app_get_metadata_item (app, "is-security") != NULL) {
			security_timestamp = g_get_monotonic_time ();
			break;
		}
	}
	if (security_timestamp_old != security_timestamp) {
		g_settings_set (monitor->settings,
				"security-timestamp", "x", security_timestamp);
	}

	g_debug ("Got %d updates", g_list_length (apps));

	if (has_important_updates (apps) ||
	    no_updates_for_a_week (monitor)) {
		notify_offline_update_available (monitor);
	}
}

static void
get_upgrades_finished_cb (GObject *object,
			  GAsyncResult *res,
			  gpointer data)
{
	GsUpdateMonitor *monitor = GS_UPDATE_MONITOR (data);
	GsApp *app;
	g_autofree gchar *body = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GNotification) n = NULL;
	g_autoptr(GsAppList) apps = NULL;

	/* get result */
	apps = gs_plugin_loader_get_updates_finish (GS_PLUGIN_LOADER (object), res, &error);
	if (apps == NULL) {
		g_debug ("no upgrades; withdrawing upgrades-available notification");
		g_application_withdraw_notification (monitor->application,
						     "upgrades-available");
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("failed to get upgrades: %s", error->message);
		return;
	}

	/* just get the first result : FIXME, do we sort these by date? */
	app = GS_APP (apps->data);

	/* TRANSLATORS: this is a distro upgrade, the replacement would be the
	 * distro name, e.g. 'Fedora' */
	body = g_strdup_printf (_("A new version of %s is available to install"),
				gs_app_get_name (app));

	/* TRANSLATORS: this is a distro upgrade */
	n = g_notification_new (_("Software Upgrade Available"));
	g_notification_set_body (n, body);
	g_notification_set_default_action_and_target (n, "app.set-mode", "s", "updates");
	g_application_send_notification (monitor->application, "upgrades-available", n);
}

static void
get_updates (GsUpdateMonitor *monitor)
{
	/* NOTE: this doesn't actually do any network access, instead it just
	 * returns already downloaded-and-depsolved packages */
	g_debug ("Getting updates");
	gs_plugin_loader_get_updates_async (monitor->plugin_loader,
					    GS_PLUGIN_REFINE_FLAGS_REQUIRE_UPDATE_DETAILS |
					    GS_PLUGIN_REFINE_FLAGS_REQUIRE_UPDATE_SEVERITY,
					    monitor->cancellable,
					    get_updates_finished_cb,
					    monitor);
}

static void
get_upgrades (GsUpdateMonitor *monitor)
{
	/* NOTE: this doesn't actually do any network access, it relies on the
	 * AppStream data being up to date, either by the appstream-data
	 * package being up-to-date, or the metadata being auto-downloaded */
	g_debug ("Getting updates");
	gs_plugin_loader_get_distro_upgrades_async (monitor->plugin_loader,
						    GS_PLUGIN_REFINE_FLAGS_DEFAULT,
						    monitor->cancellable,
						    get_upgrades_finished_cb,
						    monitor);
}

static void
refresh_cache_finished_cb (GObject *object,
			   GAsyncResult *res,
			   gpointer data)
{
	GsUpdateMonitor *monitor = data;
	g_autoptr(GError) error = NULL;

	if (!gs_plugin_loader_refresh_finish (GS_PLUGIN_LOADER (object), res, &error)) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("failed to refresh the cache: %s", error->message);
		return;
	}
	get_updates (monitor);
}

static void
check_updates (GsUpdateMonitor *monitor)
{
	PkNetworkEnum network_state;
	gint64 tmp;
	g_autoptr(GDateTime) last_refreshed = NULL;
	g_autoptr(GDateTime) now_refreshed = NULL;

	/* never refresh when offline or on mobile connections */
	g_object_get (monitor->control, "network-state", &network_state, NULL);
	if (network_state == PK_NETWORK_ENUM_OFFLINE ||
	    network_state == PK_NETWORK_ENUM_MOBILE)
		return;

	g_settings_get (monitor->settings, "check-timestamp", "x", &tmp);
	last_refreshed = g_date_time_new_from_unix_local (tmp);
	if (last_refreshed != NULL) {
		gint now_year, now_month, now_day, now_hour;
		gint year, month, day;
		g_autoptr(GDateTime) now = NULL;

		now = g_date_time_new_now_local ();

		g_date_time_get_ymd (now, &now_year, &now_month, &now_day);
		now_hour = g_date_time_get_hour (now);

		g_date_time_get_ymd (last_refreshed, &year, &month, &day);

		/* check that it is the next day */
		if (!((now_year > year) ||
		      (now_year == year && now_month > month) ||
		      (now_year == year && now_month == month && now_day > day)))
			return;

		/* ...and past 6am */
		if (!(now_hour >= 6))
			return;
	}

	g_debug ("Daily update check due");
	now_refreshed = g_date_time_new_now_local ();
	g_settings_set (monitor->settings, "check-timestamp", "x",
			g_date_time_to_unix (now_refreshed));

	/* NOTE: this doesn't actually refresh the cache, it actually just checks
	 * for updates (which might happen to also refresh the cache as a side
	 * effect) and then downloads new packages */
	g_debug ("Refreshing cache");
	gs_plugin_loader_refresh_async (monitor->plugin_loader,
					60 * 60 * 24,
					GS_PLUGIN_REFRESH_FLAGS_UPDATES,
					monitor->cancellable,
					refresh_cache_finished_cb,
					monitor);
}

static gboolean
check_hourly_cb (gpointer data)
{
	GsUpdateMonitor *monitor = data;

	g_debug ("Hourly updates check");
	check_updates (monitor);

	return G_SOURCE_CONTINUE;
}

static gboolean
check_thrice_daily_cb (gpointer data)
{
	GsUpdateMonitor *monitor = data;

	g_debug ("Daily upgrades check");
	get_upgrades (monitor);

	return G_SOURCE_CONTINUE;
}

static gboolean
check_updates_on_startup_cb (gpointer data)
{
	GsUpdateMonitor *monitor = data;

	g_debug ("First hourly updates check");
	check_updates (monitor);
	get_upgrades (monitor);

	monitor->check_hourly_id =
		g_timeout_add_seconds (3600, check_hourly_cb, monitor);
	monitor->check_daily_id =
		g_timeout_add_seconds (3 * 86400, check_thrice_daily_cb, monitor);

	monitor->check_startup_id = 0;
	return G_SOURCE_REMOVE;
}

static void
notify_network_state_cb (PkControl *control,
			 GParamSpec *pspec,
			 GsUpdateMonitor *monitor)
{
	check_updates (monitor);
}

static void
updates_changed_cb (GsPluginLoader *plugin_loader, GsUpdateMonitor *monitor)
{
	/* when the list of downloaded-and-ready-to-go updates changes get the
	 * new list and perhaps show/hide the notification */
	get_updates (monitor);
}

static void
show_installed_updates_notification (GsUpdateMonitor *monitor, PkResults *results)
{
	const gchar *message;
	const gchar *title;
	guint64 time_last_notified;
	guint64 time_update_completed;
	g_autoptr(GNotification) notification = NULL;

	g_settings_get (monitor->settings,
			"install-timestamp", "x", &time_last_notified);

	/* have we notified about this before */
	time_update_completed = pk_offline_get_results_mtime (NULL);
	if (time_update_completed == 0) {
		/* FIXME: is this ever going to be true? */
		g_application_withdraw_notification (monitor->application,
						     "offline-updates");
		return;
	}
	if (time_last_notified >= time_update_completed)
		return;

	if (pk_results_get_exit_code (results) == PK_EXIT_ENUM_SUCCESS) {
		GPtrArray *packages;
		packages = pk_results_get_package_array (results);
		title = ngettext ("Software Update Installed",
				  "Software Updates Installed",
				  packages->len);
		/* TRANSLATORS: message when we've done offline updates */
		message = ngettext ("An important OS update has been installed.",
				    "Important OS updates have been installed.",
				    packages->len);
		g_ptr_array_unref (packages);
	} else {

		title = _("Software Updates Failed");
		/* TRANSLATORS: message when we offline updates have failed */
		message = _("An important OS update failed to be installed.");
	}

	notification = g_notification_new (title);
	g_notification_set_body (notification, message);
	if (pk_results_get_exit_code (results) == PK_EXIT_ENUM_SUCCESS) {
		g_notification_add_button_with_target (notification, _("Review"), "app.set-mode", "s", "updated");
		g_notification_set_default_action_and_target (notification, "app.set-mode", "s", "updated");
	} else {
		g_notification_add_button (notification, _("Show Details"), "app.show-offline-update-error");
		g_notification_set_default_action (notification, "app.show-offline-update-error");
	}

	g_application_send_notification (monitor->application, "offline-updates", notification);

	/* update the timestamp so we don't show again */
	g_settings_set (monitor->settings,
			"install-timestamp", "x", time_update_completed);
}

static gboolean
cleanup_notifications_cb (gpointer user_data)
{
	GsUpdateMonitor *monitor = user_data;
	g_autoptr(PkResults) results = NULL;

	/* only show this at first-boot */
	results = pk_offline_get_results (NULL);
	if (results != NULL) {
		show_installed_updates_notification (monitor, results);
	} else {
		g_application_withdraw_notification (monitor->application,
						     "offline-update");
	}

	/* wait until first check to show */
	g_application_withdraw_notification (monitor->application,
					     "updates-available");

	monitor->cleanup_notifications_id = 0;
	return G_SOURCE_REMOVE;
}

static void
gs_update_monitor_init (GsUpdateMonitor *monitor)
{
	monitor->settings = g_settings_new ("org.gnome.software");

	/* cleanup at startup */
	monitor->cleanup_notifications_id =
		g_idle_add (cleanup_notifications_cb, monitor);

	/* do a first check 60 seconds after login, and then every hour */
	monitor->check_startup_id =
		g_timeout_add_seconds (60, check_updates_on_startup_cb, monitor);

	monitor->cancellable = g_cancellable_new ();
	monitor->control = pk_control_new ();
	g_signal_connect (monitor->control, "notify::network-state",
			  G_CALLBACK (notify_network_state_cb), monitor);
}

static void
gs_update_monitor_dispose (GObject *object)
{
	GsUpdateMonitor *monitor = GS_UPDATE_MONITOR (object);

	if (monitor->cancellable) {
		g_cancellable_cancel (monitor->cancellable);
		g_clear_object (&monitor->cancellable);
	}
	if (monitor->check_hourly_id != 0) {
		g_source_remove (monitor->check_hourly_id);
		monitor->check_hourly_id = 0;
	}
	if (monitor->check_daily_id != 0) {
		g_source_remove (monitor->check_daily_id);
		monitor->check_daily_id = 0;
	}
	if (monitor->check_startup_id != 0) {
		g_source_remove (monitor->check_startup_id);
		monitor->check_startup_id = 0;
	}
	if (monitor->notification_blocked_id != 0) {
		g_source_remove (monitor->notification_blocked_id);
		monitor->notification_blocked_id = 0;
	}
	if (monitor->cleanup_notifications_id != 0) {
		g_source_remove (monitor->cleanup_notifications_id);
		monitor->cleanup_notifications_id = 0;
	}
	if (monitor->control != NULL) {
		g_signal_handlers_disconnect_by_func (monitor->control, notify_network_state_cb, monitor);
		g_clear_object (&monitor->control);
	}
	g_clear_object (&monitor->settings);

	G_OBJECT_CLASS (gs_update_monitor_parent_class)->dispose (object);
}

static void
gs_update_monitor_finalize (GObject *object)
{
	GsUpdateMonitor *monitor = GS_UPDATE_MONITOR (object);

	g_application_release (monitor->application);

	G_OBJECT_CLASS (gs_update_monitor_parent_class)->finalize (object);
}

static void
gs_update_monitor_class_init (GsUpdateMonitorClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->dispose = gs_update_monitor_dispose;
	object_class->finalize = gs_update_monitor_finalize;
}

GsUpdateMonitor *
gs_update_monitor_new (GsApplication *application)
{
	GsUpdateMonitor *monitor;

	monitor = GS_UPDATE_MONITOR (g_object_new (GS_TYPE_UPDATE_MONITOR, NULL));
	monitor->application = G_APPLICATION (application);
	g_application_hold (monitor->application);

	monitor->plugin_loader = gs_application_get_plugin_loader (application);
	g_signal_connect (monitor->plugin_loader, "updates-changed",
			  G_CALLBACK (updates_changed_cb), monitor);

	return monitor;
}

/* vim: set noexpandtab: */
