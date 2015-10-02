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
#include <string.h>
#include <libsoup/soup.h>
#include <icns.h>

#include "gs-html-utils.h"
#include "gs-utils.h"

struct GsPluginPrivate {
	SoupSession		*session;
};

/**
 * gs_plugin_get_name:
 */
const gchar *
gs_plugin_get_name (void)
{
	return "steam";
}

/**
 * gs_plugin_initialize:
 */
void
gs_plugin_initialize (GsPlugin *plugin)
{
	plugin->priv = GS_PLUGIN_GET_PRIVATE (GsPluginPrivate);
}

/**
 * gs_plugin_destroy:
 */
void
gs_plugin_destroy (GsPlugin *plugin)
{
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
 * gs_plugin_steam_html_download:
 */
static gboolean
gs_plugin_steam_html_download (GsPlugin *plugin,
			       const gchar *uri,
			       gchar **data,
			       gsize *data_len,
			       GError **error)
{
	guint status_code;
	g_autoptr(GInputStream) stream = NULL;
	g_autoptr(SoupMessage) msg = NULL;

	/* create the GET data */
	msg = soup_message_new (SOUP_METHOD_GET, uri);
	if (msg == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "%s is not a valid URL", uri);
		return FALSE;
	}

	/* ensure networking is set up */
	if (!gs_plugin_setup_networking (plugin, error))
		return FALSE;

	/* set sync request */
	status_code = soup_session_send_message (plugin->priv->session, msg);
	if (status_code != SOUP_STATUS_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Failed to download icon %s: %s",
			     uri, soup_status_get_phrase (status_code));
		return FALSE;
	}

	/* return data */
	if (data != NULL)
		*data = g_memdup (msg->response_body->data,
				  msg->response_body->length);
	if (data_len != NULL)
		*data_len = msg->response_body->length;
	return TRUE;
}

/**
 * gs_plugin_get_deps:
 */
const gchar **
gs_plugin_get_deps (GsPlugin *plugin)
{
	static const gchar *deps[] = {
		"appstream",		/* need metadata */
		NULL };
	return deps;
}

typedef enum {
	GS_PLUGIN_STEAM_TOKEN_START		= 0x00,
	GS_PLUGIN_STEAM_TOKEN_STRING		= 0x01,
	GS_PLUGIN_STEAM_TOKEN_INTEGER		= 0x02,
	GS_PLUGIN_STEAM_TOKEN_END		= 0x08,
	GS_PLUGIN_STEAM_TOKEN_LAST,
} GsPluginSteamToken;

/**
 * gs_plugin_steam_token_kind_to_str:
 **/
static const gchar *
gs_plugin_steam_token_kind_to_str (guint8 data)
{
	static gchar tmp[2] = { 0x00, 0x00 };

	if (data == GS_PLUGIN_STEAM_TOKEN_START)
		return "[SRT]";
	if (data == GS_PLUGIN_STEAM_TOKEN_STRING)
		return "[STR]";
	if (data == GS_PLUGIN_STEAM_TOKEN_INTEGER)
		return "[INT]";
	if (data == GS_PLUGIN_STEAM_TOKEN_END)
		return "[END]";

	/* guess */
	if (data == 0x03)
		return "[ETX]";
	if (data == 0x04)
		return "[EOT]";
	if (data == 0x05)
		return "[ENQ]";
	if (data == 0x06)
		return "[ACK]";
	if (data == 0x07)
		return "[BEL]";
	if (data == 0x09)
		return "[SMI]";

	/* printable */
	if (g_ascii_isprint (data)) {
		tmp[0] = data;
		return tmp;
	}
	return "[?]";
}

/**
 * gs_plugin_steam_consume_uint32:
 **/
static guint32
gs_plugin_steam_consume_uint32 (guint8 *data, gsize data_len, guint *idx)
{
	guint32 tmp = *((guint32 *) &data[*idx + 1]);
	*idx += 4;
	return tmp;
}

/**
 * gs_plugin_steam_consume_string:
 **/
static const gchar *
gs_plugin_steam_consume_string (guint8 *data, gsize data_len, guint *idx)
{
	const gchar *tmp;

	/* this may be an empty string */
	tmp = (const gchar *) &data[*idx+1];
	if (tmp[0] == '\0') {
		(*idx)++;
		return NULL;
	}
	*idx += strlen (tmp) + 1;
	return tmp;
}

/**
 * gs_plugin_steam_find_next_sync_point:
 **/
static void
gs_plugin_steam_find_next_sync_point (guint8 *data, gsize data_len, guint *idx)
{
	guint i;
	for (i = *idx; i < data_len; i++) {
		if (memcmp (&data[i], "\0\x02\0common\0", 8) == 0) {
			*idx = i - 1;
			return;
		}
	}
	*idx = 0xfffffffe;
}

/**
 * gs_plugin_steam_add_app:
 **/
static GHashTable *
gs_plugin_steam_add_app (GPtrArray *apps)
{
	GHashTable *app;
	app = g_hash_table_new_full (g_str_hash, g_str_equal,
				     g_free, (GDestroyNotify) g_variant_unref);
	g_ptr_array_add (apps, app);
	return app;
}

/**
 * gs_plugin_steam_parse_appinfo_file:
 **/
static GPtrArray *
gs_plugin_steam_parse_appinfo_file (const gchar *filename, GError **error)
{
	GPtrArray *apps;
	GHashTable *app = NULL;
	const gchar *tmp;
	guint8 *data = NULL;
	gsize data_len = 0;
	guint i = 0;
	gboolean debug =  g_getenv ("GS_PLUGIN_STEAM_DEBUG") != NULL;

	/* load file */
	if (!g_file_get_contents (filename, (gchar **) &data, &data_len, error))
		return NULL;

	/* a GPtrArray of GHashTable */
	apps = g_ptr_array_new_with_free_func ((GDestroyNotify) g_hash_table_unref);

	/* find the first application and avoid header */
	gs_plugin_steam_find_next_sync_point (data, data_len, &i);
	for (i = i + 1; i < data_len; i++) {
		if (debug)
			g_debug ("%04x {0x%02x} %s", i, data[i], gs_plugin_steam_token_kind_to_str (data[i]));
		if (data[i] == GS_PLUGIN_STEAM_TOKEN_START) {

			/* this is a new application/game */
			if (data[i+1] == 0x02) {
				/* reset */
				app = gs_plugin_steam_add_app (apps);
				i++;
				continue;
			}

			/* new group */
			if (g_ascii_isprint (data[i+1])) {
				tmp = gs_plugin_steam_consume_string (data, data_len, &i);
				if (debug)
					g_debug ("[%s] {", tmp);
				continue;
			}

			/* something went wrong */
			if (debug)
				g_debug ("CORRUPTION DETECTED");
			gs_plugin_steam_find_next_sync_point (data, data_len, &i);
			continue;
		}
		if (data[i] == GS_PLUGIN_STEAM_TOKEN_END) {
			if (debug)
				g_debug ("}");
			continue;
		}
		if (data[i] == GS_PLUGIN_STEAM_TOKEN_STRING) {
			const gchar *value;
			tmp = gs_plugin_steam_consume_string (data, data_len, &i);
			value = gs_plugin_steam_consume_string (data, data_len, &i);
			if (debug)
				g_debug ("\t%s=%s", tmp, value);
			if (tmp != NULL && value != NULL) {
				if (g_hash_table_lookup (app, tmp) != NULL)
					continue;
				g_hash_table_insert (app,
						     g_strdup (tmp),
						     g_variant_new_string (value));
			}
			continue;
		}
		if (data[i] == GS_PLUGIN_STEAM_TOKEN_INTEGER) {
			guint32 value;
			tmp = gs_plugin_steam_consume_string (data, data_len, &i);
			value = gs_plugin_steam_consume_uint32 (data, data_len, &i);
			if (debug)
				g_debug ("\t%s=%i", tmp, value);
			if (tmp != NULL) {
				if (g_hash_table_lookup (app, tmp) != NULL)
					continue;
				g_hash_table_insert (app,
						     g_strdup (tmp),
						     g_variant_new_uint32 (value));
			}
			continue;
		}
	}

	return apps;
}

/**
 * gs_plugin_steam_dump_apps:
 **/
static void
gs_plugin_steam_dump_apps (GPtrArray *apps)
{
	guint i;
	GHashTable *app;

	for (i = 0; i < apps->len; i++) {
		g_autoptr(GList) keys = NULL;
		GList *l;
		app = g_ptr_array_index (apps, i);
		keys = g_hash_table_get_keys (app);
		for (l = keys; l != NULL; l = l->next) {
			const gchar *tmp;
			GVariant *value;
			tmp = l->data;
			value = g_hash_table_lookup (app, tmp);
			if (g_strcmp0 (g_variant_get_type_string (value), "s") == 0)
				g_print ("%s=%s\n", tmp, g_variant_get_string (value, NULL));
			else if (g_strcmp0 (g_variant_get_type_string (value), "u") == 0)
				g_print ("%s=%u\n", tmp, g_variant_get_uint32 (value));
		}
		g_print ("\n");
	}
}

/**
 * gs_plugin_steam_capture:
 *
 * Returns: A string between @start and @end, or %NULL
 **/
static gchar *
gs_plugin_steam_capture (const gchar *html,
			 const gchar *start,
			 const gchar *end,
			 guint *offset)
{
	guint i;
	guint j;
	guint start_len;
	guint end_len;

	/* find @start */
	start_len = strlen (start);
	for (i = *offset; html[i] != '\0'; i++) {
		if (memcmp (&html[i], start, start_len) != 0)
			continue;
		/* find @end */
		end_len = strlen (end);
		for (j = i + start_len; html[j] != '\0'; j++) {
			if (memcmp (&html[j], end, end_len) != 0)
				continue;
			*offset = j + end_len;
			return g_strndup (&html[i + start_len],
					  j - i - start_len);
		}
	}
	return NULL;
}

/**
 * gs_plugin_steam_update_screenshots:
 **/
static gboolean
gs_plugin_steam_update_screenshots (AsApp *app, const gchar *html, GError **error)
{
	const gchar *gameid_str;
	gchar *tmp1;
	guint i = 0;
	guint idx = 0;

	/* find all the screenshots */
	gameid_str = as_app_get_metadata_item (app, "X-Steam-GameID");
	while ((tmp1 = gs_plugin_steam_capture (html, "data-screenshotid=\"", "\"", &i))) {
		g_autoptr(AsImage) im = NULL;
		g_autoptr(AsScreenshot) ss = NULL;
		g_autofree gchar *cdn_uri = NULL;

		/* create an image */
		im = as_image_new ();
		as_image_set_kind (im, AS_IMAGE_KIND_SOURCE);
		cdn_uri = g_strdup_printf ("http://cdn.akamai.steamstatic.com/steam/apps/%s/%s", gameid_str, tmp1);
		as_image_set_url (im, cdn_uri);

		/* create screenshot with no caption */
		ss = as_screenshot_new ();
		as_screenshot_set_kind (ss, idx == 0 ? AS_SCREENSHOT_KIND_DEFAULT :
						       AS_SCREENSHOT_KIND_NORMAL);
		as_screenshot_add_image (ss, im);
		as_app_add_screenshot (app, ss);
		g_free (tmp1);

		/* limit this to a sane number */
		if (idx++ >= 4)
			break;
	}
	return TRUE;
}

/**
 * gs_plugin_steam_update_description:
 **/
static gboolean
gs_plugin_steam_update_description (AsApp *app, const gchar *html, GError **error)
{
	guint i = 0;
	g_autofree gchar *desc = NULL;
	g_autofree gchar *subsect = NULL;
	g_autoptr(GError) error_local = NULL;

	/* get the game description div section */
	subsect = gs_plugin_steam_capture (html,
			"<div id=\"game_area_description\" class=\"game_area_description\">",
			"</div>", &i);

	/* fall back gracefully */
	if (subsect == NULL) {
		subsect = gs_plugin_steam_capture (html,
				"<meta name=\"Description\" content=\"",
				"\">", &i);
	}
	if (subsect == NULL) {
		g_warning ("Failed to get description for %s [%s]",
			   as_app_get_name (app, NULL),
			   as_app_get_id (app));
		return TRUE;
	}
	desc = gs_html_utils_parse_description (subsect, &error_local);
	if (desc == NULL) {
		g_warning ("Failed to parse description for %s [%s]: %s",
			   as_app_get_name (app, NULL),
			   as_app_get_id (app),
			   error_local->message);
		return TRUE;
	}
	as_app_set_description (app, NULL, desc);
	return TRUE;
}

/**
 * gs_plugin_steam_new_pixbuf_from_icns:
 **/
static GdkPixbuf *
gs_plugin_steam_new_pixbuf_from_icns (const gchar *fn, GError **error)
{
	GdkPixbuf *pb = NULL;
	FILE *datafile;
	guint i;
	icns_family_t *icon_family = NULL;
	icns_image_t im;
	int rc;
	icns_type_t preference[] = {
		ICNS_128X128_32BIT_DATA,
		ICNS_256x256_32BIT_ARGB_DATA,
		ICNS_48x48_32BIT_DATA,
		0 };

	/* open file */
	datafile = fopen (fn, "rb");
	rc = icns_read_family_from_file (datafile, &icon_family);
	if (rc != 0) {
		g_set_error (error, 1, 0, "Failed to read icon %s", fn);
		return NULL;
	}

	/* libicns 'helpfully' frees the @arg */
	im.imageData = NULL;

	/* get the best sized icon */
	for (i = 0; preference[i] != 0; i++) {
		rc = icns_get_image32_with_mask_from_family (icon_family,
							     preference[i],
							     &im);
		if (rc == 0) {
			gchar buf[5];
			icns_type_str (preference[i], buf);
			g_debug ("using ICNS %s for %s", buf, fn);
			break;
		}
	}
	if (im.imageData == NULL) {
		g_set_error (error, 1, 0, "Failed to get icon %s", fn);
		return NULL;
	}

	/* create the pixbuf */
	pb = gdk_pixbuf_new_from_data (im.imageData,
				       GDK_COLORSPACE_RGB,
				       TRUE,
				       im.imagePixelDepth,
				       im.imageWidth,
				       im.imageHeight,
				       im.imageWidth * im.imageChannels,
				       NULL, //??
				       NULL);
	g_assert (pb != NULL);

	fclose (datafile);
	return pb;
}

/**
 * gs_plugin_steam_download_icns:
 **/
static gboolean
gs_plugin_steam_download_icns (GsPlugin *plugin, AsApp *app, const gchar *uri, GError **error)
{
	const gchar *gameid_str;
	gsize data_len;
	g_autofree gchar *cache_basename = NULL;
	g_autofree gchar *cache_fn = NULL;
	g_autofree gchar *cache_png = NULL;
	g_autofree gchar *data = NULL;
	g_autoptr(AsIcon) icon = NULL;
	g_autoptr(GdkPixbuf) pb = NULL;

	/* download icons from the cdn */
	gameid_str = as_app_get_metadata_item (app, "X-Steam-GameID");
	cache_basename = g_strdup_printf ("%s-icons.icns", gameid_str);
	cache_fn = g_build_filename (g_get_user_cache_dir (),
				     "gnome-software",
				     "steam",
				     cache_basename,
				     NULL);
	if (g_file_test (cache_fn, G_FILE_TEST_EXISTS)) {
		if (!g_file_get_contents (cache_fn, &data, &data_len, error))
			return FALSE;
	} else {
		if (!gs_plugin_steam_html_download (plugin, uri, &data, &data_len, error))
			return FALSE;
		if (!gs_mkdir_parent (cache_fn, error))
			return FALSE;
		if (!g_file_set_contents (cache_fn, data, data_len, error))
			return FALSE;
	}

	/* check the icns file is not just a png/ico/jpg file in disguise */
	if (memcmp (data + 1, "\x89PNG", 4) == 0 ||
	    memcmp (data, "\x00\x00\x01\x00", 4) == 0 ||
	    memcmp (data, "\xff\xd8\xff", 3) == 0) {
		g_debug ("using fallback for %s\n", cache_fn);
		pb = gdk_pixbuf_new_from_file (cache_fn, error);
		if (pb == NULL)
			return FALSE;
	} else {
		pb = gs_plugin_steam_new_pixbuf_from_icns (cache_fn, error);
		if (pb == NULL)
			return FALSE;
	}

	/* save to cache */
	memcpy (cache_basename + strlen (gameid_str) + 6, ".png\0", 5);
	cache_png = g_build_filename (g_get_user_cache_dir (),
				      "gnome-software",
				      "steam",
				      cache_basename,
				      NULL);
	if (!gdk_pixbuf_save (pb, cache_png, "png", error, NULL))
		return FALSE;

	/* add an icon */
	icon = as_icon_new ();
	as_icon_set_kind (icon, AS_ICON_KIND_LOCAL);
	as_icon_set_filename (icon, cache_png);
	as_app_add_icon (app, icon);
	return TRUE;
}

/**
 * gs_plugin_steam_update_store_app:
 **/
static gboolean
gs_plugin_steam_update_store_app (GsPlugin *plugin,
				  AsStore *store,
				  GHashTable *app,
				  GError **error)
{
	GVariant *tmp;
	guint32 gameid;
	gchar *app_id;
	g_autofree gchar *cache_basename = NULL;
	g_autofree gchar *cache_fn = NULL;
	g_autofree gchar *gameid_str = NULL;
	g_autofree gchar *html = NULL;
	g_autofree gchar *uri = NULL;
	g_autoptr(AsApp) item = NULL;

	/* this is the key */
	tmp = g_hash_table_lookup (app, "gameid");
	if (tmp == NULL)
		return TRUE;
	gameid = g_variant_get_uint32 (tmp);
	app_id = g_strdup_printf ("com.valve.steam-%u.desktop", gameid);
	g_debug ("processing %s", app_id);

	/* already exists */
	if (as_store_get_app_by_id (store, app_id) != NULL) {
		g_debug ("%s already exists, skipping", app_id);
		return TRUE;
	}

	/* create application with the gameid as the key */
	item = as_app_new ();
	as_app_set_project_license (item, "Steam");
	as_app_set_id (item, app_id);
	as_app_add_category (item, "Game");
	as_app_add_kudo_kind (item, AS_KUDO_KIND_MODERN_TOOLKIT);
	as_app_set_comment (item, NULL, "Available on Steam");

	/* this is for the GNOME Software plugin */
	gameid_str = g_strdup_printf ("%" G_GUINT32_FORMAT, gameid);
	as_app_add_metadata (item, "X-Steam-GameID", gameid_str);

	/* name */
	tmp = g_hash_table_lookup (app, "name");
	if (tmp != NULL) {
		const gchar *name = g_variant_get_string (tmp, NULL);
		if (g_strstr_len (name, -1, "Dedicated Server") != NULL) {
			as_app_add_veto (item, "Dedicated Server");
		} else {
			as_app_set_name (item, NULL, name);
		}
	} else {
		as_app_add_veto (item, "No name");
	}

	/* oslist */
	tmp = g_hash_table_lookup (app, "oslist");
	if (tmp == NULL) {
		as_app_add_veto (item, "No operating systems listed");
	} else if (g_strstr_len (g_variant_get_string (tmp, NULL), -1, "linux") == NULL) {
		as_app_add_veto (item, "No Linux support");
	}

	/* url: homepage */
	tmp = g_hash_table_lookup (app, "homepage");
	if (tmp != NULL)
		as_app_add_url (item, AS_URL_KIND_HOMEPAGE, g_variant_get_string (tmp, NULL));

	/* developer name */
	tmp = g_hash_table_lookup (app, "developer");
	if (tmp != NULL)
		as_app_set_developer_name (item, NULL, g_variant_get_string (tmp, NULL));

	/* type */
	tmp = g_hash_table_lookup (app, "type");
	if (tmp != NULL) {
		const gchar *kind = g_variant_get_string (tmp, NULL);
		if (g_strcmp0 (kind, "DLC") == 0 ||
		    g_strcmp0 (kind, "Config") == 0 ||
		    g_strcmp0 (kind, "Tool") == 0)
			as_app_add_veto (item, "type is %s", kind);
	}

	/* don't bother saving apps with failures */
	if (as_app_get_vetos(item)->len > 0)
		return TRUE;

	/* icons */
	tmp = g_hash_table_lookup (app, "clienticns");
	if (tmp != NULL) {
		g_autoptr(GError) error_local = NULL;
		g_autofree gchar *zip_uri = NULL;
		zip_uri = g_strdup_printf ("https://steamcdn-a.akamaihd.net/steamcommunity/public/images/apps/%i/%s.icns",
					   gameid, g_variant_get_string (tmp, NULL));
		if (!gs_plugin_steam_download_icns (plugin, item, zip_uri, &error_local)) {
			g_warning ("Failed to parse clienticns: %s",
				   error_local->message);
		}
	}

	/* fall back to a resized logo */
	if (as_app_get_icons(item)->len == 0) {
		tmp = g_hash_table_lookup (app, "logo");
		if (tmp != NULL) {
			AsIcon *icon = NULL;
			g_autofree gchar *ic_uri = NULL;
			ic_uri = g_strdup_printf ("http://cdn.akamai.steamstatic.com/steamcommunity/public/images/apps/%i/%s.jpg",
						  gameid, g_variant_get_string (tmp, NULL));
			icon = as_icon_new ();
			as_icon_set_kind (icon, AS_ICON_KIND_REMOTE);
			as_icon_set_url (icon, ic_uri);
			as_app_add_icon (item, icon);
		}
	}

	/* size */
	tmp = g_hash_table_lookup (app, "maxsize");
	if (tmp != NULL) {
		/* string when over 16Gb... :/ */
		if (g_strcmp0 (g_variant_get_type_string (tmp), "u") == 0) {
			g_autofree gchar *val = NULL;
			val = g_strdup_printf ("%" G_GUINT32_FORMAT,
					       g_variant_get_uint32 (tmp));
			as_app_add_metadata (item, "X-Steam-Size", val);
		} else {
			as_app_add_metadata (item, "X-Steam-Size",
					     g_variant_get_string (tmp, NULL));
		}
	}

	/* download page from the store */
	cache_basename = g_strdup_printf ("%s.html", gameid_str);
	cache_fn = g_build_filename (g_get_user_cache_dir (),
				     "gnome-software",
				     "steam",
				     cache_basename,
				     NULL);
	if (g_file_test (cache_fn, G_FILE_TEST_EXISTS)) {
		if (!g_file_get_contents (cache_fn, &html, NULL, error))
			return FALSE;
	} else {
		uri = g_strdup_printf ("http://store.steampowered.com/app/%s/", gameid_str);
		if (!gs_plugin_steam_html_download (plugin, uri, &html, NULL, error))
			return FALSE;
		if (!gs_mkdir_parent (cache_fn, error))
			return FALSE;
		if (!g_file_set_contents (cache_fn, html, -1, error))
			return FALSE;
	}

	/* get screenshots and descriptions */
	if (!gs_plugin_steam_update_screenshots (item, html, error))
		return FALSE;
	if (!gs_plugin_steam_update_description (item, html, error))
		return FALSE;

	/* add */
	as_store_add_app (store, item);
	return TRUE;
}

/**
 * gs_plugin_steam_update_store:
 */
static gboolean
gs_plugin_steam_update_store (GsPlugin *plugin, AsStore *store, GPtrArray *apps, GError **error)
{
	guint i;
	GHashTable *app;

	for (i = 0; i < apps->len; i++) {
		app = g_ptr_array_index (apps, i);
		if (!gs_plugin_steam_update_store_app (plugin, store, app, error))
			return FALSE;
	}
	return TRUE;
}

/**
 * gs_plugin_refresh:
 */
gboolean
gs_plugin_refresh (GsPlugin *plugin,
		   guint cache_age,
		   GsPluginRefreshFlags flags,
		   GCancellable *cancellable,
		   GError **error)
{
	g_autoptr(AsStore) store = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GPtrArray) apps = NULL;
	g_autofree gchar *fn = NULL;
	g_autofree gchar *fn_xml = NULL;

	/* check if exists */
	fn = g_build_filename (g_get_user_data_dir (),
			       "Steam", "appcache", "appinfo.vdf", NULL);
	if (!g_file_test (fn, G_FILE_TEST_EXISTS)) {
		g_debug ("no %s, so skipping", fn);
		return TRUE;
	}

	/* test cache age */
	fn_xml = g_build_filename (g_get_user_data_dir (),
				   "app-info", "xmls", "steam.xml.gz", NULL);
	if (cache_age > 0) {
		guint tmp;
		tmp = gs_utils_get_file_age (fn_xml);
		if (tmp < cache_age) {
			g_debug ("%s is only %i seconds old, so ignoring refresh",
				 fn_xml, tmp);
			return TRUE;
		}
	}

	/* parse it */
	apps = gs_plugin_steam_parse_appinfo_file (fn, error);
	if (apps == NULL)
		return FALSE;

	/* debug */
	if (g_getenv ("GS_PLUGIN_STEAM_DEBUG") != NULL)
		gs_plugin_steam_dump_apps (apps);

	/* load existing AppStream XML */
	store = as_store_new ();
	file = g_file_new_for_path (fn_xml);
	if (g_file_query_exists (file, cancellable)) {
		if (!as_store_from_file (store, file, NULL, cancellable, error))
			return FALSE;
	}

	/* update any new applications */
	if (!gs_plugin_steam_update_store (plugin, store, apps, error))
		return FALSE;

	/* save new file */
	return as_store_to_file (store, file,
				 AS_NODE_TO_XML_FLAG_FORMAT_INDENT |
				 AS_NODE_TO_XML_FLAG_FORMAT_MULTILINE,
				 NULL,
				 error);
}

/**
 * gs_plugin_steam_refine_app:
 */
static gboolean
gs_plugin_steam_refine_app (GsPlugin *plugin,
			    GsApp *app,
			    GsPluginRefineFlags flags,
			    GCancellable *cancellable,
			    GError **error)
{
	const gchar *gameid;
	const gchar *tmp;

	/* check is us */
	gameid = gs_app_get_metadata_item (app, "X-Steam-GameID");
	if (gameid == NULL)
		return TRUE;

	/* size */
	tmp = gs_app_get_metadata_item (app, "X-Steam-Size");
	if (tmp != NULL) {
		guint64 sz;
		sz = g_ascii_strtoull (tmp, NULL, 10);
		if (sz > 0)
			gs_app_set_size (app, sz);
	}

	/* FIXME */
	gs_app_set_state (app, AS_APP_STATE_AVAILABLE);

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
	GsApp *app;

	for (l = *list; l != NULL; l = l->next) {
		app = GS_APP (l->data);
		if (!gs_plugin_steam_refine_app (plugin, app, flags,
						 cancellable, error))
			return FALSE;
	}
	return TRUE;
}

/**
 * gs_plugin_app_install:
 */
gboolean
gs_plugin_app_install (GsPlugin *plugin, GsApp *app,
		       GCancellable *cancellable, GError **error)
{
	const gchar *gameid;
	g_autofree gchar *cmdline = NULL;

	/* check is us */
	gameid = gs_app_get_metadata_item (app, "X-Steam-GameID");
	if (gameid == NULL)
		return TRUE;

	/* this is async as steam is a different process: FIXME: use D-Bus */
	gs_app_set_state (app, AS_APP_STATE_INSTALLING);
	cmdline = g_strdup_printf ("steam steam://install/%s", gameid);
	return g_spawn_command_line_sync (cmdline, NULL, NULL, NULL, error);
}

/**
 * gs_plugin_app_remove:
 */
gboolean
gs_plugin_app_remove (GsPlugin *plugin, GsApp *app,
		      GCancellable *cancellable, GError **error)
{
	const gchar *gameid;
	g_autofree gchar *cmdline = NULL;

	/* check is us */
	gameid = gs_app_get_metadata_item (app, "X-Steam-GameID");
	if (gameid == NULL)
		return TRUE;

	/* this is async as steam is a different process: FIXME: use D-Bus */
	gs_app_set_state (app, AS_APP_STATE_REMOVING);
	cmdline = g_strdup_printf ("steam steam://uninstall/%s", gameid);
	return g_spawn_command_line_sync (cmdline, NULL, NULL, NULL, error);
}

/**
 * gs_plugin_app_launch:
 */
gboolean
gs_plugin_app_launch (GsPlugin *plugin, GsApp *app,
		      GCancellable *cancellable, GError **error)
{
	const gchar *gameid;
	g_autofree gchar *cmdline = NULL;

	/* check is us */
	gameid = gs_app_get_metadata_item (app, "X-Steam-GameID");
	if (gameid == NULL)
		return TRUE;

	/* this is async as steam is a different process: FIXME: use D-Bus */
	cmdline = g_strdup_printf ("steam steam://run/%s", gameid);
	return g_spawn_command_line_sync (cmdline, NULL, NULL, NULL, error);
}
