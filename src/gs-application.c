/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013 Matthias Clasen <mclasen@redhat.com>
 * Copyright (C) 2013 Richard Hughes <richard@hughsie.com>
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

#include "gs-application.h"

#include <stdlib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <libsoup/soup.h>
#include <packagekit-glib2/packagekit.h>

#ifdef GDK_WINDOWING_X11
#include <gtk/gtkx.h>
#endif

#include "gs-dbus-helper.h"
#include "gs-box.h"
#include "gs-first-run-dialog.h"
#include "gs-shell.h"
#include "gs-update-monitor.h"
#include "gs-proxy-settings.h"
#include "gs-shell-search-provider.h"
#include "gs-offline-updates.h"
#include "gs-folders.h"
#include "gs-utils.h"


struct _GsApplication {
	GtkApplication	 parent;
	AsProfile	*profile;
	GCancellable	*cancellable;
	GtkApplication	*application;
	GtkCssProvider	*provider;
	GsPluginLoader	*plugin_loader;
	gint		 pending_apps;
	GsShell		*shell;
	GsUpdateMonitor *update_monitor;
	GsProxySettings *proxy_settings;
	GsDbusHelper	*dbus_helper;
	GsShellSearchProvider *search_provider;
	GNetworkMonitor *network_monitor;
	GSettings       *settings;
};

G_DEFINE_TYPE (GsApplication, gs_application, GTK_TYPE_APPLICATION);

GsPluginLoader *
gs_application_get_plugin_loader (GsApplication *application)
{
	return application->plugin_loader;
}

gboolean
gs_application_has_active_window (GsApplication *application)
{
	GList *windows;
	GList *l;

	windows = gtk_application_get_windows (GTK_APPLICATION (application));
	for (l = windows; l != NULL; l = l->next) {
		if (gtk_window_is_active (GTK_WINDOW (l->data)))
			return TRUE;
	}
	return FALSE;
}

static void
gs_application_init (GsApplication *application)
{
	const GOptionEntry options[] = {
		{ "mode", '\0', 0, G_OPTION_ARG_STRING, NULL,
		  /* TRANSLATORS: this is a command line option */
		  _("Start up mode: either ‘updates’, ‘updated’, ‘installed’ or ‘overview’"), _("MODE") },
		{ "search", '\0', 0, G_OPTION_ARG_STRING, NULL,
		  _("Search for applications"), _("SEARCH") },
		{ "details", '\0', 0, G_OPTION_ARG_STRING, NULL,
		  _("Show application details"), _("ID") },
		{ "local-filename", '\0', 0, G_OPTION_ARG_FILENAME, NULL,
		  _("Open a local package file"), _("FILENAME") },
		{ "verbose", '\0', 0, G_OPTION_ARG_NONE, NULL,
		  _("Show verbose debugging information"), NULL },
		{ "profile", 0, 0, G_OPTION_ARG_NONE, NULL,
		  _("Show profiling information for the service"), NULL },
		{ "prefer-local", '\0', 0, G_OPTION_ARG_NONE, NULL,
		  _("Prefer local file sources to AppStream"), NULL },
		{ "version", 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL },
		{ NULL }
	};

	g_application_add_main_option_entries (G_APPLICATION (application), options);

	application->profile = as_profile_new ();
}

static void
download_updates_setting_changed (GSettings     *settings,
				  const gchar   *key,
				  GsApplication *app)
{
	if (!gs_updates_are_managed () &&
	    g_settings_get_boolean (settings, key)) {
		g_debug ("Enabling update monitor");
		app->update_monitor = gs_update_monitor_new (app);
	} else {
		g_debug ("Disabling update monitor");
		g_clear_object (&app->update_monitor);
	}
}

static void
on_permission_changed (GPermission *permission,
                       GParamSpec  *pspec,
                       gpointer     data)
{
	GsApplication *app = data;

	if (app->settings)
		download_updates_setting_changed (app->settings, "download-updates", app);
}

static void
gs_application_monitor_permission (GsApplication *app)
{
	GPermission *permission;

	permission = gs_offline_updates_permission_get ();
	g_signal_connect (permission, "notify",
			  G_CALLBACK (on_permission_changed), app);
}

static void
gs_application_monitor_updates (GsApplication *app)
{
	g_signal_connect (app->settings, "changed::download-updates",
			  G_CALLBACK (download_updates_setting_changed), app);
	download_updates_setting_changed (app->settings,
					  "download-updates",
					  app);
}

static void
network_changed_cb (GNetworkMonitor *monitor,
		    gboolean available,
		    GsApplication *app)
{
	gs_plugin_loader_set_network_status (app->plugin_loader, available);
}

static void
gs_application_monitor_network (GsApplication *app)
{
	app->network_monitor = g_object_ref (g_network_monitor_get_default ());

	g_signal_connect (app->network_monitor, "network-changed",
			  G_CALLBACK (network_changed_cb), app);

	network_changed_cb (app->network_monitor,
			    g_network_monitor_get_network_available (app->network_monitor),
			    app);
}

static void
gs_application_initialize_plugins (GsApplication *app)
{
	static gboolean initialized = FALSE;
	g_autoptr(GError) error = NULL;

	if (initialized)
		return;

	initialized = TRUE;

	app->plugin_loader = gs_plugin_loader_new ();
	gs_plugin_loader_set_location (app->plugin_loader, NULL);
	if (!gs_plugin_loader_setup (app->plugin_loader, &error)) {
		g_warning ("Failed to setup plugins: %s", error->message);
		exit (1);
	}

	/* show the priority of each plugin */
	gs_plugin_loader_dump_state (app->plugin_loader);

}

static gboolean
gs_application_dbus_register (GApplication    *application,
                              GDBusConnection *connection,
                              const gchar     *object_path,
                              GError         **error)
{
	GsApplication *app = GS_APPLICATION (application);

	gs_application_initialize_plugins (app);
	app->search_provider = gs_shell_search_provider_new ();
	gs_shell_search_provider_setup (app->search_provider,
					app->plugin_loader);

	return gs_shell_search_provider_register (app->search_provider, connection, error);
}

static void
gs_application_dbus_unregister (GApplication    *application,
                                GDBusConnection *connection,
                                const gchar     *object_path)
{
	GsApplication *app = GS_APPLICATION (application);

	if (app->search_provider != NULL) {
		gs_shell_search_provider_unregister (app->search_provider);
		g_clear_object (&app->search_provider);
	}
}

static void
gs_application_show_first_run_dialog (GsApplication *app)
{
	GtkWidget *dialog;

	if (g_settings_get_boolean (app->settings, "first-run") == TRUE) {
		dialog = gs_first_run_dialog_new ();
		gtk_window_set_transient_for (GTK_WINDOW (dialog), gs_shell_get_window (app->shell));
		gtk_window_present (GTK_WINDOW (dialog));

		g_settings_set_boolean (app->settings, "first-run", FALSE);
	}
}

static void
theme_changed (GtkSettings *settings, GParamSpec *pspec, GsApplication *app)
{
	g_autoptr(GFile) file = NULL;
	g_autofree gchar *theme = NULL;

	g_object_get (settings, "gtk-theme-name", &theme, NULL);
	if (g_strcmp0 (theme, "HighContrast") == 0) {
		file = g_file_new_for_uri ("resource:///org/gnome/Software/gtk-style-hc.css");
	} else {
		file = g_file_new_for_uri ("resource:///org/gnome/Software/gtk-style.css");
	}
	gtk_css_provider_load_from_file (app->provider, file, NULL);
}

static void
gs_application_initialize_ui (GsApplication *app)
{
	static gboolean initialized = FALSE;

	if (initialized)
		return;

	initialized = TRUE;

	/* get CSS */
	app->provider = gtk_css_provider_new ();
	gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
						   GTK_STYLE_PROVIDER (app->provider),
						   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	g_signal_connect (gtk_settings_get_default (), "notify::gtk-theme-name",
			  G_CALLBACK (theme_changed), app);
	theme_changed (gtk_settings_get_default (), NULL, app);

	gs_application_initialize_plugins (app);

	/* setup UI */
	app->shell = gs_shell_new ();

	app->cancellable = g_cancellable_new ();

	gs_shell_setup (app->shell, app->plugin_loader, app->cancellable);
	gtk_application_add_window (GTK_APPLICATION (app), gs_shell_get_window (app->shell));

	g_signal_connect_swapped (app->shell, "loaded",
				  G_CALLBACK (gtk_window_present), gs_shell_get_window (app->shell));
}

static void
initialize_ui_and_present_window (GsApplication *app)
{
	GList *windows;
	GtkWindow *window;

	gs_application_initialize_ui (app);
	windows = gtk_application_get_windows (GTK_APPLICATION (app));
	if (windows) {
		window = windows->data;
		gtk_window_present (window);
	}
}

static void
sources_activated (GSimpleAction *action,
		   GVariant      *parameter,
		   gpointer       app)
{
	gs_shell_show_sources (GS_APPLICATION (app)->shell);
}

static void
about_activated (GSimpleAction *action,
		 GVariant      *parameter,
		 gpointer       app)
{
	const gchar *authors[] = {
		"Richard Hughes",
		"Matthias Clasen",
		"Allan Day",
		"Ryan Lerch",
		"William Jon McCann",
		NULL
	};
	const gchar *copyright = "Copyright \xc2\xa9 2013 Richard Hughes, Matthias Clasen";
	GList *windows;
	GtkWindow *parent = NULL;

	gs_application_initialize_ui (app);

	windows = gtk_application_get_windows (GTK_APPLICATION (app));
	if (windows)
		parent = windows->data;

	gtk_show_about_dialog (parent,
			       /* TRANSLATORS: this is the title of the about window */
			       "title", _("About Software"),
			       /* TRANSLATORS: this is the application name */
			       "program-name", _("Software"),
			       "authors", authors,
			       /* TRANSLATORS: well, we seem to think so, anyway */
			       "comments", _("A nice way to manage the software on your system."),
			       "copyright", copyright,
			       "license-type", GTK_LICENSE_GPL_2_0,
			       "logo-icon-name", "gnome-software",
			       "translator-credits", _("translator-credits"),
			       "version", VERSION,
			       NULL);
}

static void
profile_activated (GSimpleAction *action,
		   GVariant      *parameter,
		   gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	as_profile_dump (app->profile);
}

static void
offline_updates_cancel (void)
{
	g_autoptr(GError) error = NULL;
	if (!pk_offline_cancel (NULL, &error))
		g_warning ("failed to cancel the offline update: %s", error->message);
}

/**
 * offline_update_cb:
 **/
static void
offline_update_cb (GsPluginLoader *plugin_loader,
		   GAsyncResult *res,
		   GsApplication *app)
{
	g_autoptr(GError) error = NULL;
	if (!gs_plugin_loader_offline_update_finish (plugin_loader, res, &error)) {
		g_warning ("Failed to trigger offline update: %s", error->message);
		return;
	}
	gs_reboot (offline_updates_cancel);
}

static void
reboot_and_install (GSimpleAction *action,
		    GVariant      *parameter,
		    gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	gs_application_initialize_plugins (app);
	gs_plugin_loader_offline_update_async (app->plugin_loader,
	                                       NULL,
	                                       app->cancellable,
	                                       (GAsyncReadyCallback) offline_update_cb,
	                                       app);
}

static void
quit_activated (GSimpleAction *action,
		GVariant      *parameter,
		gpointer       app)
{
	GApplicationFlags flags;
	GList *windows;
	GtkWidget *window;

	flags = g_application_get_flags (app);

	if (flags & G_APPLICATION_IS_SERVICE) {
		windows = gtk_application_get_windows (GTK_APPLICATION (app));
		if (windows) {
			window = windows->data;
			gtk_widget_hide (window);
		}

		return;
	}

	g_application_quit (G_APPLICATION (app));
}

static void
set_mode_activated (GSimpleAction *action,
		    GVariant      *parameter,
		    gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	const gchar *mode;

	initialize_ui_and_present_window (app);

	mode = g_variant_get_string (parameter, NULL);
	if (g_strcmp0 (mode, "updates") == 0) {
		gs_shell_set_mode (app->shell, GS_SHELL_MODE_UPDATES);
	} else if (g_strcmp0 (mode, "installed") == 0) {
		gs_shell_set_mode (app->shell, GS_SHELL_MODE_INSTALLED);
	} else if (g_strcmp0 (mode, "overview") == 0) {
		gs_shell_set_mode (app->shell, GS_SHELL_MODE_OVERVIEW);
	} else if (g_strcmp0 (mode, "updated") == 0) {
		gs_shell_set_mode (app->shell, GS_SHELL_MODE_UPDATES);
		gs_shell_show_installed_updates (app->shell);
	} else {
		g_warning ("Mode '%s' not recognised", mode);
	}
}

static void
search_activated (GSimpleAction *action,
		  GVariant      *parameter,
		  gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	const gchar *search;

	initialize_ui_and_present_window (app);

	search = g_variant_get_string (parameter, NULL);
	gs_shell_show_search (app->shell, search);
}

static void
details_activated (GSimpleAction *action,
		   GVariant      *parameter,
		   gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	const gchar *id;
	const gchar *search;

	initialize_ui_and_present_window (app);

	g_variant_get (parameter, "(&s&s)", &id, &search);
	if (search != NULL && search[0] != '\0')
		gs_shell_show_search_result (app->shell, id, search);
	else
		gs_shell_show_details (app->shell, id);
}

static void
filename_activated (GSimpleAction *action,
		    GVariant      *parameter,
		    gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	const gchar *filename;

	gs_application_initialize_ui (app);

	g_variant_get (parameter, "(&s)", &filename);
	gs_shell_show_filename (app->shell, filename);
}

static void
launch_activated (GSimpleAction *action,
		  GVariant      *parameter,
		  gpointer       data)
{
	const gchar *desktop_id;
	GdkDisplay *display;
	g_autoptr(GError) error = NULL;
	g_autoptr(GAppInfo) appinfo = NULL;
	g_autoptr(GAppLaunchContext) context = NULL;

	desktop_id = g_variant_get_string (parameter, NULL);
	display = gdk_display_get_default ();
	appinfo = G_APP_INFO (g_desktop_app_info_new (desktop_id));
	if (appinfo == NULL) {
		g_warning ("no such desktop file: %s", desktop_id);
		return;
	}

	context = G_APP_LAUNCH_CONTEXT (gdk_display_get_app_launch_context (display));
	if (!g_app_info_launch (appinfo, NULL, context, &error)) {
		g_warning ("launching %s failed: %s", desktop_id, error->message);
	}
}

static void
show_offline_updates_error (GSimpleAction *action,
			    GVariant      *parameter,
			    gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);

	initialize_ui_and_present_window (app);

	gs_shell_set_mode (app->shell, GS_SHELL_MODE_UPDATES);
	gs_offline_updates_show_error (app->shell);
}

static void
install_resources_activated (GSimpleAction *action,
                             GVariant      *parameter,
                             gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
#ifdef GDK_WINDOWING_X11
	GdkDisplay *display;
#endif
	const gchar *mode;
	const gchar *startup_id;
	gchar **resources;

	g_variant_get (parameter, "(&s^as&s)", &mode, &resources, &startup_id);

#ifdef GDK_WINDOWING_X11
	display = gdk_display_get_default ();

	if (GDK_IS_X11_DISPLAY (display)) {
		if (startup_id != NULL && startup_id[0] != '\0')
			gdk_x11_display_set_startup_notification_id (display,
			                                             startup_id);
	}
#endif

	initialize_ui_and_present_window (app);

	gs_shell_show_extras_search (app->shell, mode, resources);
}

static GActionEntry actions[] = {
	{ "about", about_activated, NULL, NULL, NULL },
	{ "sources", sources_activated, NULL, NULL, NULL },
	{ "quit", quit_activated, NULL, NULL, NULL },
	{ "profile", profile_activated, NULL, NULL, NULL },
	{ "reboot-and-install", reboot_and_install, NULL, NULL, NULL },
	{ "set-mode", set_mode_activated, "s", NULL, NULL },
	{ "search", search_activated, "s", NULL, NULL },
	{ "details", details_activated, "(ss)", NULL, NULL },
	{ "filename", filename_activated, "(s)", NULL, NULL },
	{ "launch", launch_activated, "s", NULL, NULL },
	{ "show-offline-update-error", show_offline_updates_error, NULL, NULL, NULL },
	{ "install-resources", install_resources_activated, "(sass)", NULL, NULL },
	{ "nop", NULL, NULL, NULL }
};

static void
gs_application_startup (GApplication *application)
{
	G_APPLICATION_CLASS (gs_application_parent_class)->startup (application);

	g_type_ensure (GS_TYPE_BOX);

	g_action_map_add_action_entries (G_ACTION_MAP (application),
					 actions, G_N_ELEMENTS (actions),
					 application);

	GS_APPLICATION (application)->proxy_settings = gs_proxy_settings_new ();
	GS_APPLICATION (application)->dbus_helper = gs_dbus_helper_new ();
	GS_APPLICATION (application)->settings = g_settings_new ("org.gnome.software");
	gs_application_monitor_permission (GS_APPLICATION (application));
	gs_application_monitor_updates (GS_APPLICATION (application));
	gs_application_monitor_network (GS_APPLICATION (application));
	gs_folders_convert ();
}

static void
gs_application_activate (GApplication *application)
{
	gs_application_initialize_ui (GS_APPLICATION (application));
	gs_shell_set_mode (GS_APPLICATION (application)->shell, GS_SHELL_MODE_OVERVIEW);
	gs_shell_activate (GS_APPLICATION (application)->shell);
	gs_application_show_first_run_dialog (GS_APPLICATION (application));
}

static void
gs_application_dispose (GObject *object)
{
	GsApplication *app = GS_APPLICATION (object);

	if (app->cancellable != NULL) {
		g_cancellable_cancel (app->cancellable);
		g_clear_object (&app->cancellable);
	}

	g_clear_object (&app->plugin_loader);
	g_clear_object (&app->shell);
	g_clear_object (&app->provider);
	g_clear_object (&app->update_monitor);
	g_clear_object (&app->proxy_settings);
	g_clear_object (&app->profile);
	g_clear_object (&app->network_monitor);
	g_clear_object (&app->dbus_helper);
	g_clear_object (&app->settings);

	G_OBJECT_CLASS (gs_application_parent_class)->dispose (object);
}

static int
gs_application_handle_local_options (GApplication *app, GVariantDict *options)
{
	const gchar *id;
	const gchar *local_filename;
	const gchar *mode;
	const gchar *search;
	g_autoptr(GError) error = NULL;

	if (g_variant_dict_contains (options, "verbose"))
		g_setenv ("G_MESSAGES_DEBUG", "all", TRUE);

	/* prefer local sources */
	if (g_variant_dict_contains (options, "prefer-local"))
		g_setenv ("GNOME_SOFTWARE_PREFER_LOCAL", "true", TRUE);

	if (g_variant_dict_contains (options, "version")) {
		g_print ("gnome-software " VERSION "\n");
		return 0;
	}

	if (!g_application_register (app, NULL, &error)) {
		g_printerr ("%s\n", error->message);
		return 1;
	}

	if (g_variant_dict_contains (options, "profile")) {
		g_action_group_activate_action (G_ACTION_GROUP (app),
						"profile",
						NULL);
		return 0;
	}

	if (g_variant_dict_lookup (options, "mode", "&s", &mode)) {
		g_action_group_activate_action (G_ACTION_GROUP (app),
						"set-mode",
						g_variant_new_string (mode));
		return 0;
	} else if (g_variant_dict_lookup (options, "search", "&s", &search)) {
		g_action_group_activate_action (G_ACTION_GROUP (app),
						"search",
						g_variant_new_string (search));
		return 0;
	} else if (g_variant_dict_lookup (options, "details", "&s", &id)) {
		g_action_group_activate_action (G_ACTION_GROUP (app),
						"details",
						g_variant_new ("(ss)", id, ""));
		return 0;
	} else if (g_variant_dict_lookup (options, "local-filename", "^&ay", &local_filename)) {
		g_action_group_activate_action (G_ACTION_GROUP (app),
						"filename",
						g_variant_new ("(s)", local_filename));
		return 0;
	}

	return -1;
}

static void
gs_application_open (GApplication  *application,
                     GFile        **files,
                     gint           n_files,
                     const gchar   *hint)
{
	GsApplication *app = GS_APPLICATION (application);
	gint i;

	for (i = 0; i < n_files; i++) {
		g_autofree gchar *str = g_file_get_uri (files[i]);
		g_autoptr(SoupURI) uri = NULL;

		uri = soup_uri_new (str);
		if (!SOUP_URI_IS_VALID (uri))
			continue;

		if (g_strcmp0 (soup_uri_get_scheme (uri), "appstream") == 0) {
			const gchar *path = soup_uri_get_path (uri);

			/* trim any leading slashes */
			while (*path == '/')
				path++;

			g_action_group_activate_action (G_ACTION_GROUP (app),
			                                "details",
			                                g_variant_new ("(ss)", path, ""));
		}
	}
}

static void
gs_application_class_init (GsApplicationClass *class)
{
	G_OBJECT_CLASS (class)->dispose = gs_application_dispose;
	G_APPLICATION_CLASS (class)->startup = gs_application_startup;
	G_APPLICATION_CLASS (class)->activate = gs_application_activate;
	G_APPLICATION_CLASS (class)->handle_local_options = gs_application_handle_local_options;
	G_APPLICATION_CLASS (class)->open = gs_application_open;
	G_APPLICATION_CLASS (class)->dbus_register = gs_application_dbus_register;
	G_APPLICATION_CLASS (class)->dbus_unregister = gs_application_dbus_unregister;
}

GsApplication *
gs_application_new (void)
{
	g_set_prgname("org.gnome.Software");
	return g_object_new (GS_APPLICATION_TYPE,
			     "application-id", "org.gnome.Software",
			     "flags", G_APPLICATION_HANDLES_OPEN,
			     "inactivity-timeout", 12000,
			     NULL);
}

/* vim: set noexpandtab: */
