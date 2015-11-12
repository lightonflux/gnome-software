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

#define I_KNOW_THE_PACKAGEKIT_GLIB2_API_IS_SUBJECT_TO_CHANGE
#include <packagekit-glib2/packagekit.h>

#include <gs-plugin.h>

#include "packagekit-common.h"

/**
 * packagekit_status_enum_to_plugin_status:
 **/
GsPluginStatus
packagekit_status_enum_to_plugin_status (PkStatusEnum status)
{
	GsPluginStatus plugin_status = GS_PLUGIN_STATUS_UNKNOWN;

	switch (status) {
	case PK_STATUS_ENUM_SETUP:
	case PK_STATUS_ENUM_CANCEL:
	case PK_STATUS_ENUM_FINISHED:
	case PK_STATUS_ENUM_UNKNOWN:
		break;
	case PK_STATUS_ENUM_WAIT:
	case PK_STATUS_ENUM_WAITING_FOR_LOCK:
	case PK_STATUS_ENUM_WAITING_FOR_AUTH:
		plugin_status = GS_PLUGIN_STATUS_WAITING;
		break;
	case PK_STATUS_ENUM_LOADING_CACHE:
	case PK_STATUS_ENUM_TEST_COMMIT:
	case PK_STATUS_ENUM_RUNNING:
	case PK_STATUS_ENUM_SIG_CHECK:
		plugin_status = GS_PLUGIN_STATUS_SETUP;
		break;
	case PK_STATUS_ENUM_DOWNLOAD:
	case PK_STATUS_ENUM_DOWNLOAD_REPOSITORY:
	case PK_STATUS_ENUM_DOWNLOAD_PACKAGELIST:
	case PK_STATUS_ENUM_DOWNLOAD_FILELIST:
	case PK_STATUS_ENUM_DOWNLOAD_CHANGELOG:
	case PK_STATUS_ENUM_DOWNLOAD_GROUP:
	case PK_STATUS_ENUM_DOWNLOAD_UPDATEINFO:
		plugin_status = GS_PLUGIN_STATUS_DOWNLOADING;
		break;
	case PK_STATUS_ENUM_INSTALL:
		plugin_status = GS_PLUGIN_STATUS_INSTALLING;
		break;
	case PK_STATUS_ENUM_CLEANUP:
	case PK_STATUS_ENUM_REMOVE:
		plugin_status = GS_PLUGIN_STATUS_REMOVING;
		break;
	case PK_STATUS_ENUM_REQUEST:
	case PK_STATUS_ENUM_QUERY:
	case PK_STATUS_ENUM_INFO:
	case PK_STATUS_ENUM_DEP_RESOLVE:
		plugin_status = GS_PLUGIN_STATUS_QUERYING;
		break;
	default:
		g_warning ("no mapping for %s",
			   pk_status_enum_to_string (status));
		break;
	}
	return plugin_status;
}

/**
 * gs_plugin_packagekit_add_results:
 */
gboolean
gs_plugin_packagekit_add_results (GsPlugin *plugin,
				  GList **list,
				  PkResults *results,
				  GError **error)
{
	const gchar *package_id;
	guint i;
	PkPackage *package;
	g_autoptr(GHashTable) installed = NULL;
	g_autoptr(PkError) error_code = NULL;
	g_autoptr(GPtrArray) array_filtered = NULL;
	g_autoptr(GPtrArray) array = NULL;

	/* check error code */
	error_code = pk_results_get_error_code (results);
	if (error_code != NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "failed to get-packages: %s, %s",
			     pk_error_enum_to_string (pk_error_get_code (error_code)),
			     pk_error_get_details (error_code));
		return FALSE;
	}

	/* add all installed packages to a hash */
	installed = g_hash_table_new (g_str_hash, g_str_equal);
	array = pk_results_get_package_array (results);
	for (i = 0; i < array->len; i++) {
		package = g_ptr_array_index (array, i);
		if (pk_package_get_info (package) != PK_INFO_ENUM_INSTALLED)
			continue;
		g_hash_table_insert (installed,
				     (const gpointer) pk_package_get_name (package),
				     (const gpointer) pk_package_get_id (package));
	}

	/* if the search returns more than one package with the same name,
	 * ignore everything with that name except the installed package */
	array_filtered = g_ptr_array_new ();
	for (i = 0; i < array->len; i++) {
		package = g_ptr_array_index (array, i);
		package_id = g_hash_table_lookup (installed, pk_package_get_name (package));
		if (pk_package_get_info (package) == PK_INFO_ENUM_INSTALLED || package_id == NULL) {
			g_ptr_array_add (array_filtered, package);
		} else {
			g_debug ("ignoring available %s as installed %s also reported",
				 pk_package_get_id (package), package_id);
		}
	}

	/* process packages */
	for (i = 0; i < array_filtered->len; i++) {
		g_autoptr(GsApp) app = NULL;
		package = g_ptr_array_index (array_filtered, i);

		app = gs_app_new (NULL);
		gs_app_add_source (app, pk_package_get_name (package));
		gs_app_add_source_id (app, pk_package_get_id (package));
		gs_app_set_name (app,
				 GS_APP_QUALITY_LOWEST,
				 pk_package_get_name (package));
		gs_app_set_summary (app,
				    GS_APP_QUALITY_LOWEST,
				    pk_package_get_summary (package));
		gs_app_set_management_plugin (app, "PackageKit");
		gs_app_set_version (app, pk_package_get_version (package));
		switch (pk_package_get_info (package)) {
		case PK_INFO_ENUM_INSTALLED:
			gs_app_set_state (app, AS_APP_STATE_INSTALLED);
			break;
		case PK_INFO_ENUM_AVAILABLE:
		case PK_INFO_ENUM_REMOVING:
			gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
			break;
		case PK_INFO_ENUM_INSTALLING:
		case PK_INFO_ENUM_UPDATING:
			break;
		default:
			gs_app_set_state (app, AS_APP_STATE_UNKNOWN);
			g_warning ("unknown info state of %s",
				   pk_info_enum_to_string (pk_package_get_info (package)));
		}
		gs_app_set_kind (app, GS_APP_KIND_PACKAGE);
		gs_plugin_add_app (list, app);
	}
	return TRUE;
}
