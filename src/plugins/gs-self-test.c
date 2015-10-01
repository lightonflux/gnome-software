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

#include "config.h"

#include <glib.h>
#include <string.h>
#include <glib-object.h>
#include <gtk/gtk.h>

#include "gs-moduleset.h"
#include "gs-html-utils.h"

static void
html_utils_func (void)
{
	const gchar *input;
	g_autofree gchar *out_complex = NULL;
	g_autofree gchar *out_list = NULL;
	g_autofree gchar *out_simple = NULL;
	g_autoptr(GError) error = NULL;

	/* simple, from meta */
	input = "This game is simply awesome&trade; in every way!";
	out_simple = gs_html_utils_parse_description (input, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (out_simple, ==, "<p>This game is simply awesome™ in every way!</p>");

	/* complex non-compliant HTML, from div */
	input = "  <h1>header</h1>"
		"  <p>First line of the <i>description</i> is okay...</p>"
		"  <img src=\"moo.png\">"
		"  <img src=\"png\">"
		"  <p>Second <strong>line</strong> is <a href=\"#moo\">even</a> better!</p>";
	out_complex = gs_html_utils_parse_description (input, &error);
	g_print ("\n\n%s\n\n", out_complex);
	g_assert_no_error (error);
	g_assert_cmpstr (out_complex, ==, "<p>First line of the description is okay...</p>"
					  "<p>Second line is even better!</p>");

	/* complex list */
	input = "  <ul>"
		"  <li>First line of the list</li>"
		"  <li>Second line of the list</li>"
		"  </ul>";
	out_list = gs_html_utils_parse_description (input, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (out_list, ==, "<ul><li>First line of the list</li><li>Second line of the list</li></ul>");
}

static void
moduleset_func (void)
{
	gboolean ret;
	gchar **data;
	GError *error = NULL;
	g_autoptr(GsModuleset) ms = NULL;

	/* not avaiable in make distcheck */
	if (!g_file_test ("./moduleset-test.xml", G_FILE_TEST_EXISTS))
		return;

	ms = gs_moduleset_new ();
	ret = gs_moduleset_parse_filename (ms, "./moduleset-test.xml", &error);
	g_assert_no_error (error);
	g_assert (ret);

	data = gs_moduleset_get_modules (ms,
					 GS_MODULESET_MODULE_KIND_PACKAGE,
					 "gnome3",
					 NULL);
	g_assert (data != NULL);
	g_assert_cmpint (g_strv_length (data), ==, 1);
	g_assert_cmpstr (data[0], ==, "kernel");
	g_assert_cmpstr (data[1], ==, NULL);

	data = gs_moduleset_get_modules (ms,
					 GS_MODULESET_MODULE_KIND_APPLICATION,
					 "gnome3",
					 NULL);
	g_assert (data != NULL);
	g_assert_cmpint (g_strv_length (data), ==, 1);
	g_assert_cmpstr (data[0], ==, "gnome-shell.desktop");
	g_assert_cmpstr (data[1], ==, NULL);
}

int
main (int argc, char **argv)
{
	gtk_init (&argc, &argv);
	g_test_init (&argc, &argv, NULL);
	g_setenv ("G_MESSAGES_DEBUG", "all", TRUE);

	/* only critical and error are fatal */
	g_log_set_fatal_mask (NULL, G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);

	/* tests go here */
	g_test_add_func ("/moduleset", moduleset_func);
	g_test_add_func ("/html-utils", html_utils_func);

	return g_test_run ();
}

/* vim: set noexpandtab: */
