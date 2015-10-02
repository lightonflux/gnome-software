/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2014 Richard Hughes <richard@hughsie.com>
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
#include <glib.h>
#include <appstream-glib.h>

#include "gs-html-utils.h"

typedef enum {
	GS_HTML_UTILS_ACTION_IGNORE,
	GS_HTML_UTILS_ACTION_PARA,
	GS_HTML_UTILS_ACTION_LI,
	GS_HTML_UTILS_ACTION_LAST
} GsHtmlUtilsAction;

typedef struct {
	GsHtmlUtilsAction	 action;
	GString			*str;
} GsHtmlUtilsHelper;

/**
 * gs_html_utils_start_cb:
 **/
static void
gs_html_utils_start_cb (GMarkupParseContext *context,
			  const gchar *element_name,
			  const gchar **attribute_names,
			  const gchar **attribute_values,
			  gpointer user_data,
			  GError **error)
{
	GsHtmlUtilsHelper *helper = (GsHtmlUtilsHelper *) user_data;
	if (g_strcmp0 (element_name, "book") == 0)
		return;
	if (g_strcmp0 (element_name, "li") == 0) {
		helper->action = GS_HTML_UTILS_ACTION_LI;
		return;
	}
	if (g_strcmp0 (element_name, "p") == 0) {
		helper->action = GS_HTML_UTILS_ACTION_PARA;
		return;
	}
	if (g_strcmp0 (element_name, "ul") == 0 ||
	    g_strcmp0 (element_name, "ol") == 0) {
		g_string_append (helper->str, "<ul>");
		return;
	}
	g_warning ("unhandled START %s", element_name);
}

/**
 * gs_html_utils_end_cb:
 **/
static void
gs_html_utils_end_cb (GMarkupParseContext *context,
			const gchar *element_name,
			gpointer user_data,
			GError **error)
{
	GsHtmlUtilsHelper *helper = (GsHtmlUtilsHelper *) user_data;
	if (g_strcmp0 (element_name, "book") == 0 ||
	    g_strcmp0 (element_name, "li") == 0) {
		return;
	}
	if (g_strcmp0 (element_name, "p") == 0) {
		helper->action = GS_HTML_UTILS_ACTION_IGNORE;
		return;
	}
	if (g_strcmp0 (element_name, "ul") == 0 ||
	    g_strcmp0 (element_name, "ol") == 0) {
		g_string_append (helper->str, "</ul>");
		return;
	}
	g_warning ("unhandled END %s", element_name);
}

/**
 * gs_html_utils_text_cb:
 **/
static void
gs_html_utils_text_cb (GMarkupParseContext *context,
			 const gchar *text,
			 gsize text_len,
			 gpointer user_data,
			 GError **error)
{
	GsHtmlUtilsHelper *helper = (GsHtmlUtilsHelper *) user_data;
	g_autofree gchar *tmp = NULL;
	g_auto(GStrv) split = NULL;
	guint i;
	gchar *strip;

	if (helper->action == GS_HTML_UTILS_ACTION_IGNORE)
		return;

	/* only add valid lines */
	tmp = g_markup_escape_text (text, text_len);
	split = g_strsplit (tmp, "\n", -1);
	for (i = 0; split[i] != NULL; i++) {
		strip = g_strstrip (split[i]);
		if (strip[0] == '\0')
			continue;
		if (strlen (strip) < 15)
			continue;
		switch (helper->action) {
		case GS_HTML_UTILS_ACTION_PARA:
			g_string_append_printf (helper->str, "<p>%s</p>", strip);
			break;
		case GS_HTML_UTILS_ACTION_LI:
			g_string_append_printf (helper->str, "<li>%s</li>", strip);
			break;
		default:
			break;
		}
	}
}

/**
 * gs_html_utils_strreplace:
 **/
static void
gs_html_utils_strreplace (GString *str, const gchar *search, const gchar *replace)
{
	g_auto(GStrv) split = NULL;
	g_autofree gchar *new = NULL;

	/* optimise */
	if (g_strstr_len (str->str, -1, search) == NULL)
		return;
	split = g_strsplit (str->str, search, -1);
	new = g_strjoinv (replace, split);
	g_string_assign (str, new);
}

/**
 * gs_html_utils_erase:
 *
 * Replaces any tag with whitespace.
 **/
static void
gs_html_utils_erase (GString *str, const gchar *start, const gchar *end)
{
	guint i, j;
	guint start_len = strlen (start);
	guint end_len = strlen (end);
	for (i = 0; str->str[i] != '\0'; i++) {
		if (memcmp (&str->str[i], start, start_len) != 0)
			continue;
		for (j = i; i < str->len; j++) {
			if (memcmp (&str->str[j], end, end_len) != 0)
				continue;
			/* delete this section and restart the search */
			g_string_erase (str, i, (j - i) + end_len);
			i = -1;
			break;
		}
	}
}

/**
 * gs_html_utils_parse_description:
 **/
gchar *
gs_html_utils_parse_description (const gchar *html, GError **error)
{
	GMarkupParseContext *ctx;
	GsHtmlUtilsHelper helper;
	GMarkupParser parser = {
		gs_html_utils_start_cb,
		gs_html_utils_end_cb,
		gs_html_utils_text_cb,
		NULL,
		NULL };
	g_autofree gchar *tmp = NULL;
	g_autoptr(GString) str = NULL;

	/* set up XML parser */
	helper.action = GS_HTML_UTILS_ACTION_PARA;
	helper.str = g_string_new ("");
	ctx = g_markup_parse_context_new (&parser, G_MARKUP_TREAT_CDATA_AS_TEXT, &helper, NULL);

	/* ensure this has at least one se of quotes */
	str = g_string_new ("");
	g_string_append_printf (str, "<book>%s</book>", html);

	/* convert win32 line endings */
	g_strdelimit (str->str, "\r", '\n');

	/* tidy up non-compliant HTML5 */
	gs_html_utils_erase (str, "<img", ">");
	gs_html_utils_erase (str, "<br", ">");

	/* kill anything that's not wanted */
	gs_html_utils_erase (str, "<h1", "</h1>");
	gs_html_utils_erase (str, "<h2", "</h2>");
	gs_html_utils_erase (str, "<span", "</span>");
	gs_html_utils_erase (str, "<a", ">");
	gs_html_utils_erase (str, "</a", ">");

	/* use UTF-8 */
	gs_html_utils_strreplace (str, "<i>", "");
	gs_html_utils_strreplace (str, "</i>", "");
	gs_html_utils_strreplace (str, "<u>", "");
	gs_html_utils_strreplace (str, "</u>", "");
	gs_html_utils_strreplace (str, "<b>", "");
	gs_html_utils_strreplace (str, "</b>", "");
	gs_html_utils_strreplace (str, "<blockquote>", "");
	gs_html_utils_strreplace (str, "</blockquote>", "");
	gs_html_utils_strreplace (str, "<strong>", "");
	gs_html_utils_strreplace (str, "</strong>", "");
	gs_html_utils_strreplace (str, "&trade;", "™");
	gs_html_utils_strreplace (str, "&reg;", "®");

//g_print ("%s\n", str->str);

	/* parse */
	if (!g_markup_parse_context_parse (ctx, str->str, -1, error))
		return NULL;

	/* return only valid AppStream markup */
	return as_markup_convert_full (helper.str->str,
				       AS_MARKUP_CONVERT_FORMAT_APPSTREAM,
				       AS_MARKUP_CONVERT_FLAG_IGNORE_ERRORS,
				       error);
}
