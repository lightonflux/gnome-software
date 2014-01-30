/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
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

#include <config.h>

#include <libsoup/soup.h>
#include <string.h>
#include <sqlite3.h>
#include <stdlib.h>

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
	return "fedora-tagger";
}

#define GS_PLUGIN_FEDORA_TAGGER_OS_RELEASE_FN	"/etc/os-release"
#define GS_PLUGIN_FEDORA_TAGGER_SERVER		"https://apps.fedoraproject.org/tagger"

/* 3 months */
#define GS_PLUGIN_FEDORA_TAGGER_AGE_MAX		(60 * 60 * 24 * 7 * 4 * 3)

/**
 * gs_plugin_initialize:
 */
void
gs_plugin_initialize (GsPlugin *plugin)
{
	GError *error = NULL;
	gboolean ret;
	gchar *data = NULL;

	plugin->priv = GS_PLUGIN_GET_PRIVATE (GsPluginPrivate);
	plugin->priv->db_path = g_build_filename (g_get_home_dir (),
						  ".local",
						  "share",
						  "gnome-software",
						  "fedora-tagger.db",
						  NULL);

	/* check that we are running on Fedora */
	ret = g_file_get_contents (GS_PLUGIN_FEDORA_TAGGER_OS_RELEASE_FN,
				   &data, NULL, &error);
	if (!ret) {
		gs_plugin_set_enabled (plugin, FALSE);
		g_warning ("disabling '%s' as %s could not be read: %s",
			   plugin->name,
			   GS_PLUGIN_FEDORA_TAGGER_OS_RELEASE_FN,
			   error->message);
		g_error_free (error);
		goto out;
	}
	if (g_strstr_len (data, -1, "ID=fedora\n") == NULL) {
		gs_plugin_set_enabled (plugin, FALSE);
		g_debug ("disabling '%s' as %s suggests we're not Fedora",
			 plugin->name, GS_PLUGIN_FEDORA_TAGGER_OS_RELEASE_FN);
		goto out;
	}
out:
	g_free (data);
}

/**
 * gs_plugin_get_priority:
 */
gdouble
gs_plugin_get_priority (GsPlugin *plugin)
{
	return 1.2f;
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
 * gs_plugin_parse_json:
 *
 * This is a quick and dirty JSON parser that extracts one line from the
 * JSON formatted data. Sorry JsonGlib, you look awesome, but you're just too
 * heavy for an error message.
 */
static gchar *
gs_plugin_parse_json (const gchar *data, gsize data_len, const gchar *key)
{
	GString *string;
	gchar *key_full;
	gchar *value = NULL;
	gchar **split;
	guint i;
	gchar *tmp;
	guint len;

	/* format the key to match what JSON returns */
	key_full = g_strdup_printf ("\"%s\":", key);

	/* replace escaping with something sane */
	string = g_string_new_len (data, data_len);
	gs_string_replace (string, "\\\"", "'");

	/* find the line that corresponds to our key */
	split = g_strsplit (string->str, "\n", -1);
	for (i = 0; split[i] != NULL; i++) {
		tmp = g_strchug (split[i]);
		if (g_str_has_prefix (tmp, key_full)) {
			tmp += strlen (key_full);

			/* remove leading chars */
			tmp = g_strstrip (tmp);
			if (tmp[0] == '\"')
				tmp += 1;

			/* remove trailing chars */
			len = strlen (tmp);
			if (tmp[len-1] == ',')
				len -= 1;
			if (tmp[len-1] == '\"')
				len -= 1;
			value = g_strndup (tmp, len);
		}
	}
	g_strfreev (split);
	g_string_free (string, TRUE);
	return value;
}


/**
 * gs_plugin_setup_networking:
 */
static gboolean
gs_plugin_setup_networking (GsPlugin *plugin, GError **error)
{
	gboolean ret = TRUE;

	/* already set up */
	if (plugin->priv->session != NULL)
		goto out;

	/* set up a session */
	plugin->priv->session = soup_session_sync_new_with_options (SOUP_SESSION_USER_AGENT,
								    "gnome-software",
								    SOUP_SESSION_TIMEOUT, 5000,
								    NULL);
	if (plugin->priv->session == NULL) {
		ret = FALSE;
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "%s: failed to setup networking",
			     plugin->name);
		goto out;
	}
	soup_session_add_feature_by_type (plugin->priv->session,
					  SOUP_TYPE_PROXY_RESOLVER_DEFAULT);
out:
	return ret;
}

/**
 * gs_plugin_app_set_rating_pkg:
 */
static gboolean
gs_plugin_app_set_rating_pkg (GsPlugin *plugin,
			      const gchar *pkgname,
			      gint rating,
			      GError **error)
{
	SoupMessage *msg = NULL;
	gchar *data = NULL;
	gchar *error_msg = NULL;
	gchar *uri = NULL;
	guint status_code;

	/* create the PUT data */
	uri = g_strdup_printf ("%s/api/v1/rating/%s/",
			       GS_PLUGIN_FEDORA_TAGGER_SERVER,
			       pkgname);
	data = g_strdup_printf ("pkgname=%s&rating=%i", pkgname, rating);
	msg = soup_message_new (SOUP_METHOD_PUT, uri);
	soup_message_set_request (msg, SOUP_FORM_MIME_TYPE_URLENCODED,
				  SOUP_MEMORY_COPY, data, strlen (data));

	/* set sync request */
	status_code = soup_session_send_message (plugin->priv->session, msg);
	if (status_code != SOUP_STATUS_OK) {
		g_debug ("Failed to set rating on fedora-tagger: %s",
			 soup_status_get_phrase (status_code));
		if (msg->response_body->data != NULL) {
			error_msg = gs_plugin_parse_json (msg->response_body->data,
							  msg->response_body->length,
							  "error");
			g_debug ("the error given was: %s", error_msg);
		}
	} else {
		g_debug ("Got response: %s", msg->response_body->data);
	}

	g_free (error_msg);
	g_free (data);
	g_free (uri);
	if (msg != NULL)
		g_object_unref (msg);
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
	GPtrArray *sources;
	const gchar *pkgname;
	gboolean ret = TRUE;
	guint i;

	/* get the package name */
	sources = gs_app_get_sources (app);
	if (sources->len == 0) {
		g_warning ("no pkgname for %s", gs_app_get_id (app));
		goto out;
	}

	/* ensure networking is set up */
	ret = gs_plugin_setup_networking (plugin, error);
	if (!ret)
		goto out;

	/* set rating for each package */
	for (i = 0; i < sources->len; i++) {
		pkgname = g_ptr_array_index (sources, i);
		ret = gs_plugin_app_set_rating_pkg (plugin,
						    pkgname,
						    gs_app_get_rating (app),
						    error);
		if (!ret)
			goto out;
	}
out:
	return ret;
}

/**
 * gs_plugin_fedora_tagger_timestamp_cb:
 **/
static gint
gs_plugin_fedora_tagger_timestamp_cb (void *data, gint argc,
				      gchar **argv, gchar **col_name)
{
	gint64 *timestamp = (gint64 *) data;
	*timestamp = g_ascii_strtoll (argv[0], NULL, 10);
	return 0;
}

typedef struct {
	gchar		*pkgname;
	gdouble		 rating;
	gdouble		 vote_count;
	gdouble		 user_count;
	gdouble		 confidence;
} FedoraTaggerItem;

/**
 * fedora_tagger_item_free:
 */
static void
fedora_tagger_item_free (FedoraTaggerItem *item)
{
	g_free (item->pkgname);
	g_slice_free (FedoraTaggerItem, item);
}

/**
 * gs_plugin_fedora_tagger_add:
 */
static gboolean
gs_plugin_fedora_tagger_add (GsPlugin *plugin,
			     FedoraTaggerItem *item,
			     GError **error)
{
	gboolean ret = TRUE;
	gchar *error_msg = NULL;
	gchar *statement = NULL;
	gint rc;

	/* insert the entry */
	statement = g_strdup_printf ("INSERT OR REPLACE INTO ratings (pkgname, rating, "
				     "vote_count, user_count, confidence) "
				     "VALUES ('%s', '%.0f', '%.0f', '%.0f', '%.0f');",
				     item->pkgname, item->rating,
				     item->vote_count, item->user_count,
				     item->confidence);
	rc = sqlite3_exec (plugin->priv->db, statement, NULL, NULL, &error_msg);
	if (rc != SQLITE_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "SQL error: %s", error_msg);
		sqlite3_free (error_msg);
		ret = FALSE;
		goto out;
	}
out:
	g_free (statement);
	return ret;

}

/**
 * gs_plugin_fedora_tagger_set_timestamp:
 */
static gboolean
gs_plugin_fedora_tagger_set_timestamp (GsPlugin *plugin,
				       const gchar *type,
				       GError **error)
{
	gboolean ret = TRUE;
	gchar *error_msg = NULL;
	gchar *statement = NULL;
	gint rc;

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
		ret = FALSE;
		goto out;
	}
out:
	g_free (statement);
	return ret;

}

/**
 * gs_plugin_fedora_tagger_download:
 */
static gboolean
gs_plugin_fedora_tagger_download (GsPlugin *plugin, GError **error)
{
	FedoraTaggerItem *item;
	GPtrArray *items = NULL;
	SoupMessage *msg = NULL;
	gboolean ret = TRUE;
	gchar *error_msg = NULL;
	gchar **fields;
	gchar **split = NULL;
	gchar *uri = NULL;
	gdouble count_sum = 0;
	guint i;
	guint status_code;

	/* create the GET data */
	uri = g_strdup_printf ("%s/api/v1/rating/dump/",
			       GS_PLUGIN_FEDORA_TAGGER_SERVER);
	msg = soup_message_new (SOUP_METHOD_GET, uri);

	/* ensure networking is set up */
	ret = gs_plugin_setup_networking (plugin, error);
	if (!ret)
		goto out;

	/* set sync request */
	status_code = soup_session_send_message (plugin->priv->session, msg);
	if (status_code != SOUP_STATUS_OK) {
		ret = FALSE;
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Failed to set rating on fedora-tagger: %s",
			     soup_status_get_phrase (status_code));
		goto out;
	}

	/* process the tab-delimited data */
	items = g_ptr_array_new_with_free_func ((GDestroyNotify) fedora_tagger_item_free);
	split = g_strsplit (msg->response_body->data, "\n", -1);
	for (i = 0; split[i] != NULL; i++) {
		if (split[i][0] == '\0' ||
		    split[i][0] == '#')
			continue;
		fields = g_strsplit (split[i], "\t", -1);
		if (g_strv_length (fields) == 4) {
			item = g_slice_new0 (FedoraTaggerItem);
			item->pkgname = g_strdup (fields[0]);
			item->rating = g_strtod (fields[1], NULL);
			item->vote_count = g_strtod (fields[2], NULL);
			item->user_count = g_strtod (fields[3], NULL);
			g_ptr_array_add (items, item);
		} else {
			g_warning ("unexpected data from fedora-tagger, expected: "
				   "'pkgname\trating\tvote_count\tuser_count' and got '%s'",
				   split[i]);
		}
		g_strfreev (fields);
	}

	/* no suitable data? */
	if (items->len == 0) {
		ret = FALSE;
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_FAILED,
				     "Failed to get data from fedora-tagger");
		goto out;
	}

	/* calculate confidence */
	for (i = 0; i < items->len; i++) {
		item = g_ptr_array_index (items, i);
		count_sum += item->vote_count;
	}
	if (count_sum == 0) {
		ret = FALSE;
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_FAILED,
				     "Failed to get vote count in fedora-tagger");
		goto out;
	}
	count_sum /= (gdouble) items->len;
	g_debug ("fedora-tagger vote_count average is %.2f", count_sum);
	for (i = 0; i < items->len; i++) {
		item = g_ptr_array_index (items, i);
		item->confidence = MAX (100.0f * item->vote_count / count_sum, 100);
	}

	/* add each completed item */
	for (i = 0; i < items->len; i++) {
		item = g_ptr_array_index (items, i);
		g_debug ("adding %s: %.1f%% [%.1f] {%.1f%%}",
			 item->pkgname, item->rating,
			 item->vote_count, item->confidence);
		ret = gs_plugin_fedora_tagger_add (plugin, item, error);
		if (!ret)
			goto out;
	}

	/* reset the timestamp */
	ret = gs_plugin_fedora_tagger_set_timestamp (plugin, "mtime", error);
	if (!ret)
		goto out;
out:
	g_free (error_msg);
	g_free (uri);
	g_strfreev (split);
	if (items != NULL)
		g_ptr_array_unref (items);
	if (msg != NULL)
		g_object_unref (msg);
	return ret;
}

/**
 * gs_plugin_fedora_tagger_load_db:
 */
static gboolean
gs_plugin_fedora_tagger_load_db (GsPlugin *plugin, GError **error)
{
	const gchar *statement;
	gboolean ret = TRUE;
	gboolean rebuild_ratings = FALSE;
	gchar *error_msg = NULL;
	gint rc;
	gint64 mtime = 0;
	gint64 now;

	g_debug ("trying to open database '%s'", plugin->priv->db_path);
	ret = gs_mkdir_parent (plugin->priv->db_path, error);
	if (!ret)
		goto out;
	rc = sqlite3_open (plugin->priv->db_path, &plugin->priv->db);
	if (rc != SQLITE_OK) {
		ret = FALSE;
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Can't open fedora-tagger database: %s",
			     sqlite3_errmsg (plugin->priv->db));
		goto out;
	}

	/* we don't need to keep doing fsync */
	sqlite3_exec (plugin->priv->db, "PRAGMA synchronous=OFF",
		      NULL, NULL, NULL);

	/* create ratings if required */
	rc = sqlite3_exec (plugin->priv->db,
			   "SELECT vote_count FROM ratings LIMIT 1",
			   gs_plugin_fedora_tagger_timestamp_cb, &mtime,
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
			   gs_plugin_fedora_tagger_timestamp_cb, &mtime,
			   &error_msg);
	if (rc != SQLITE_OK) {
		g_debug ("creating table to repair: %s", error_msg);
		sqlite3_free (error_msg);
		statement = "CREATE TABLE timestamps ("
			    "key TEXT PRIMARY KEY,"
			    "value INTEGER DEFAULT 0);";
		sqlite3_exec (plugin->priv->db, statement, NULL, NULL, NULL);

		/* reset the timestamp */
		ret = gs_plugin_fedora_tagger_set_timestamp (plugin, "ctime", error);
		if (!ret)
			goto out;
	}

	/* no data */
	now = g_get_real_time () / G_USEC_PER_SEC;
	if (mtime == 0 || rebuild_ratings) {
		g_debug ("No fedora-tagger data");
		ret = gs_plugin_fedora_tagger_download (plugin, error);
		if (!ret)
			goto out;
	} else if (now - mtime > GS_PLUGIN_FEDORA_TAGGER_AGE_MAX) {
		g_debug ("fedora-tagger data was %li days old, so regetting",
			 (now - mtime) / ( 60 * 60 * 24));
		ret = gs_plugin_fedora_tagger_download (plugin, error);
		if (!ret)
			goto out;
	} else {
		g_debug ("fedora-tagger data %li days old, "
			 "so no need to redownload",
			 (now - mtime) / ( 60 * 60 * 24));
	}
out:
	return ret;
}

typedef struct {
	gint		 rating;
	gint		 confidence;
} FedoraTaggerHelper;

/**
 * gs_plugin_fedora_tagger_ratings_sqlite_cb:
 **/
static gint
gs_plugin_fedora_tagger_ratings_sqlite_cb (void *data,
					   gint argc,
					   gchar **argv,
					   gchar **col_name)
{
	FedoraTaggerHelper *helper = (FedoraTaggerHelper *) data;
	helper->rating = atoi (argv[0]);
	helper->confidence = atoi (argv[1]);
	return 0;
}

/**
 * gs_plugin_resolve_app:
 */
static gboolean
gs_plugin_resolve_app (GsPlugin *plugin,
		       const gchar *pkgname,
		       gint *rating,
		       gint *confidence,
		       GError **error)
{
	FedoraTaggerHelper helper;
	gboolean ret = TRUE;
	gchar *error_msg = NULL;
	gchar *statement;
	gint rc;

	/* default values */
	helper.rating = -1;
	helper.confidence = -1;

	/* query, but don't return an error if the package isn't found */
	statement = g_strdup_printf ("SELECT rating, confidence FROM ratings "
				     "WHERE pkgname = '%s'", pkgname);
	rc = sqlite3_exec (plugin->priv->db,
			   statement,
			   gs_plugin_fedora_tagger_ratings_sqlite_cb,
			   &helper,
			   &error_msg);
	if (rc != SQLITE_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "SQL error: %s", error_msg);
		sqlite3_free (error_msg);
		ret = FALSE;
		goto out;
	}

	/* success */
	if (rating != NULL)
		*rating = helper.rating;
	if (confidence != NULL)
		*confidence = helper.confidence;
out:
	g_free (statement);
	return ret;
}

/**
 * gs_plugin_refine:
 */
gboolean
gs_plugin_refine (GsPlugin *plugin,
		  GList *list,
		  GsPluginRefineFlags flags,
		  GCancellable *cancellable,
		  GError **error)
{
	GList *l;
	GPtrArray *sources;
	GsApp *app;
	const gchar *pkgname;
	gboolean ret = TRUE;
	gint rating;
	gint confidence;
	guint i;

	/* nothing to do here */
	if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_RATING) == 0)
		goto out;

	/* already loaded */
	if (g_once_init_enter (&plugin->priv->loaded)) {
		ret = gs_plugin_fedora_tagger_load_db (plugin, error);
		g_once_init_leave (&plugin->priv->loaded, TRUE);
		if (!ret)
			goto out;
	}

	/* add any missing ratings data */
	for (l = list; l != NULL; l = l->next) {
		app = GS_APP (l->data);
		if (gs_app_get_rating (app) != -1)
			continue;
		sources = gs_app_get_sources (app);
		for (i = 0; i < sources->len; i++) {
			pkgname = g_ptr_array_index (sources, i);
			ret = gs_plugin_resolve_app (plugin,
						     pkgname,
						     &rating,
						     &confidence,
						     error);
			if (!ret)
				goto out;
			if (rating != -1) {
				g_debug ("fedora-tagger setting rating on %s to %i%% [%i]",
					 pkgname, rating, confidence);
				gs_app_set_rating (app, rating);
				gs_app_set_rating_confidence (app, confidence);
				gs_app_set_rating_kind (app, GS_APP_RATING_KIND_SYSTEM);
			}
		}
	}
out:
	return ret;
}
