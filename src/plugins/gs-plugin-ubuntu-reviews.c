/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015 Robert Ancell <robert.ancell@canonical.com>
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

#include <math.h>
#include <libsoup/soup.h>
#include <sqlite3.h>
#include <json-glib/json-glib.h>

#include <gs-plugin.h>
#include <gs-utils.h>

struct GsPluginPrivate {
	SoupSession		*session;
	gchar			*db_path;
	gsize			 loaded;
	sqlite3			*db;
};

/**
 * gs_plugin_get_name:
 */
const gchar *
gs_plugin_get_name (void)
{
	return "ubuntu-ratings";
}

#define GS_PLUGIN_UBUNTU_REVIEWS_SERVER		"https://reviews.ubuntu.com/reviews"

/* 3 months */
#define GS_PLUGIN_UBUNTU_REVIEWS_AGE_MAX		(60 * 60 * 24 * 7 * 4 * 3)

/**
 * gs_plugin_initialize:
 */
void
gs_plugin_initialize (GsPlugin *plugin)
{
	/* create private area */
	plugin->priv = GS_PLUGIN_GET_PRIVATE (GsPluginPrivate);

	plugin->priv->db_path = g_build_filename (g_get_user_data_dir (),
						  "gnome-software",
						  "ubuntu-reviews.db",
						  NULL);

	/* check that we are running on Ubuntu */
	if (!gs_plugin_check_distro_id (plugin, "ubuntu")) {
		gs_plugin_set_enabled (plugin, FALSE);
		g_debug ("disabling '%s' as we're not Ubuntu", plugin->name);
		return;
	}
}

/**
 * gs_plugin_get_deps:
 */
const gchar **
gs_plugin_get_deps (GsPlugin *plugin)
{
	static const gchar *deps[] = { NULL };
	return deps;
}

/**
 * gs_plugin_destroy:
 */
void
gs_plugin_destroy (GsPlugin *plugin)
{
	g_free (plugin->priv->db_path);
	if (plugin->priv->db != NULL)
		sqlite3_close (plugin->priv->db);
	if (plugin->priv->session != NULL)
		g_object_unref (plugin->priv->session);
}

/**
 * gs_plugin_setup_networking:
 */
static gboolean
gs_plugin_setup_networking (GsPlugin *plugin, GError **error)
{
	/* already set up */
	if (plugin->priv->session != NULL)
		return TRUE;

	/* set up a session */
	plugin->priv->session = soup_session_new_with_options (SOUP_SESSION_USER_AGENT,
	                                                       "gnome-software",
	                                                       NULL);
	if (plugin->priv->session == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "%s: failed to setup networking",
			     plugin->name);
		return FALSE;
	}
	return TRUE;
}

/**
 * gs_plugin_app_set_rating:
 */
gboolean
gs_plugin_app_set_rating (GsPlugin *plugin,
                          GsApp *app,
                          GCancellable *cancellable,
                          GError **error)
{
	return FALSE;
}

/**
 * gs_plugin_ubuntu_reviews_timestamp_cb:
 **/
static gint
gs_plugin_ubuntu_reviews_timestamp_cb (void *data, gint argc,
                                       gchar **argv, gchar **col_name)
{
	gint64 *timestamp = (gint64 *) data;
	*timestamp = g_ascii_strtoll (argv[0], NULL, 10);
	return 0;
}


/**
 * gs_plugin_ubuntu_reviews_set_timestamp:
 */
static gboolean
gs_plugin_ubuntu_reviews_set_timestamp (GsPlugin *plugin,
                                        const gchar *type,
                                        GError **error)
{
	char *error_msg = NULL;
	gint rc;
	g_autofree gchar *statement = NULL;

	/* insert the entry */
	statement = g_strdup_printf ("INSERT OR REPLACE INTO timestamps (key, value) "
				     "VALUES ('%s', '%" G_GINT64_FORMAT "');",
				     type,
				     g_get_real_time () / G_USEC_PER_SEC);
	rc = sqlite3_exec (plugin->priv->db, statement, NULL, NULL, &error_msg);
	if (rc != SQLITE_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "SQL error: %s", error_msg);
		sqlite3_free (error_msg);
		return FALSE;
	}
	return TRUE;
}

/**
 * gs_plugin_ubuntu_review_stats_download:
 **/
static gboolean
gs_plugin_ubuntu_review_stats_download (GsPlugin *plugin, GError **error)
{
	JsonParser *parser;
	JsonReader *reader;
	gdouble count_sum = 0;
	guint i;
	guint status_code;
	g_autofree gchar *uri = NULL;
	g_autoptr(SoupMessage) msg = NULL;
	g_autoptr(GPtrArray) items = NULL;
	g_auto(GStrv) split = NULL;

	/* create the GET data */
	uri = g_strdup_printf ("%s/api/1.0/review-stats/any/any/",
			       GS_PLUGIN_UBUNTU_REVIEWS_SERVER);
	msg = soup_message_new (SOUP_METHOD_GET, uri);

	/* ensure networking is set up */
	if (!gs_plugin_setup_networking (plugin, error))
		return FALSE;

	/* set sync request */
	status_code = soup_session_send_message (plugin->priv->session, msg);
	if (status_code != SOUP_STATUS_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Failed to download Ubuntu reviews dump: %s",
			     soup_status_get_phrase (status_code));
		return FALSE;
	}

	/* process the JSON data */
	parser = json_parser_new ();
	if (!json_parser_load_from_data (parser, msg->response_body->data, -1, error)) {
		g_object_unref (parser);
		return FALSE;
	}
	reader = json_reader_new (json_parser_get_root (parser));
	if (!json_reader_is_array (reader)) {
		g_object_unref (reader);
		g_object_unref (parser);
		return FALSE;
	}
	for (i = 0; i < json_reader_count_elements (reader); i++) {
		json_reader_read_element (reader, i);
		if (json_reader_is_object (reader)) {
			const gchar *package_name;
			gint rating;

			json_reader_read_member (reader, "package_name");
			package_name = json_reader_get_string_value (reader);
			json_reader_end_member (reader);

			json_reader_read_member (reader, "ratings_average");
			/* convert star ratings *->10, **->30, ***->50, ****->70, *****->90 */
			rating = round (20 * g_ascii_strtod (json_reader_get_string_value (reader), NULL) - 10);
			json_reader_end_member (reader);
		}
		json_reader_end_element (reader);
	}
	g_object_unref (reader);

	/* no suitable data? */
	if (items->len == 0) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_FAILED,
				     "Failed to get data from Ubuntu reviews");
		return FALSE;
	}

	return FALSE;
}

/**
 * gs_plugin_ubuntu_ratings_load_db
 **/
static gboolean
gs_plugin_ubuntu_review_stats_load_db (GsPlugin *plugin, GError **error)
{
	const gchar *statement;
	gboolean rebuild_ratings = FALSE;
	char *error_msg = NULL;
	gint rc;
	gint64 mtime = 0;
	gint64 now;
	g_autoptr(GError) error_local = NULL;

	g_debug ("trying to open database '%s'", plugin->priv->db_path);
	if (!gs_mkdir_parent (plugin->priv->db_path, error))
		return FALSE;
	rc = sqlite3_open (plugin->priv->db_path, &plugin->priv->db);
	if (rc != SQLITE_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Can't open ubuntu-reviews database: %s",
			     sqlite3_errmsg (plugin->priv->db));
		return FALSE;
	}

	/* we don't need to keep doing fsync */
	sqlite3_exec (plugin->priv->db, "PRAGMA synchronous=OFF",
		      NULL, NULL, NULL);

	/* create ratings if required */
	rc = sqlite3_exec (plugin->priv->db,
			   "SELECT vote_count FROM ratings LIMIT 1",
			   gs_plugin_ubuntu_reviews_timestamp_cb, &mtime,
			   &error_msg);
	if (rc != SQLITE_OK) {
		g_debug ("creating table to repair: %s", error_msg);
		sqlite3_free (error_msg);
		statement = "DROP TABLE IF EXISTS ratings;";
		sqlite3_exec (plugin->priv->db, statement, NULL, NULL, NULL);
		statement = "CREATE TABLE ratings ("
			    "pkgname TEXT PRIMARY KEY,"
			    "rating INTEGER DEFAULT 0,"
			    "vote_count INTEGER DEFAULT 0,"
			    "user_count INTEGER DEFAULT 0,"
			    "confidence INTEGER DEFAULT 0);";
		sqlite3_exec (plugin->priv->db, statement, NULL, NULL, NULL);
		rebuild_ratings = TRUE;
	}

	/* create timestamps if required */
	rc = sqlite3_exec (plugin->priv->db,
			   "SELECT value FROM timestamps WHERE key = 'mtime' LIMIT 1",
			   gs_plugin_ubuntu_reviews_timestamp_cb, &mtime,
			   &error_msg);
	if (rc != SQLITE_OK) {
		g_debug ("creating table to repair: %s", error_msg);
		sqlite3_free (error_msg);
		statement = "CREATE TABLE timestamps ("
			    "key TEXT PRIMARY KEY,"
			    "value INTEGER DEFAULT 0);";
		sqlite3_exec (plugin->priv->db, statement, NULL, NULL, NULL);

		/* reset the timestamp */
		if (!gs_plugin_ubuntu_reviews_set_timestamp (plugin, "ctime", error))
			return FALSE;
	}

	/* no data */
	now = g_get_real_time () / G_USEC_PER_SEC;
	if (mtime == 0 || rebuild_ratings) {
		g_debug ("No ubuntu-reviews data");
		/* this should not be fatal */
		if (!gs_plugin_ubuntu_review_stats_download (plugin, &error_local)) {
			g_warning ("Failed to get ubuntu-reviews data: %s",
				   error_local->message);
			return TRUE;
		}
	} else if (now - mtime > GS_PLUGIN_UBUNTU_REVIEWS_AGE_MAX) {
		g_debug ("ubuntu-reviews data was %" G_GINT64_FORMAT
			 " days old, so regetting",
			 (now - mtime) / ( 60 * 60 * 24));
		if (!gs_plugin_ubuntu_review_stats_download (plugin, error))
			return FALSE;
	} else {
		g_debug ("ubuntu-reviews data %" G_GINT64_FORMAT
			 " days old, so no need to redownload",
			 (now - mtime) / ( 60 * 60 * 24));
	}
	return TRUE;
}

/**
 * gs_plugin_refine:
 */
gboolean
gs_plugin_refine (GsPlugin *plugin,
		  GList **list,
		  GsPluginRefineFlags flags,
		  GCancellable *cancellable,
		  GError **error)
{
	GList *l;

	for (l = *list; l != NULL; l = l->next) {
		GsApp *app = GS_APP (l->data);
                g_printerr ("  %s\n", gs_app_get_id (app));
        }

	/* We only update ratings */
	if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_RATING) == 0)
		return TRUE;

	/* Load once */
	if (g_once_init_enter (&plugin->priv->loaded)) {
		gboolean ret = gs_plugin_ubuntu_review_stats_load_db (plugin, error);
		g_once_init_leave (&plugin->priv->loaded, TRUE);
		if (!ret)
			return FALSE;
	}

	for (l = *list; l != NULL; l = l->next) {
		GsApp *app = GS_APP (l->data);
		if (gs_app_get_id (app) == NULL)
			continue;
		if (gs_app_get_rating (app) != -1)
			continue;
	}

	return TRUE;
}
