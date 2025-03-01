/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/* fm-directory-view.c
 *
 * Copyright (C) 1999, 2000  Free Software Foundation
 * Copyright (C) 2000, 2001  Eazel, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Authors: Ettore Perazzoli,
 *          John Sullivan <sullivan@eazel.com>,
 *          Darin Adler <darin@bentspoon.com>,
 *          Pavel Cisler <pavel@eazel.com>,
 *          David Emory Watson <dwatson@cs.ucr.edu>
 */

#include <config.h>
#include <math.h>

#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#define MATE_DESKTOP_USE_UNSTABLE_API
#include <libmate-desktop/mate-desktop-utils.h>

#include <eel/eel-background.h>
#include <eel/eel-glib-extensions.h>
#include <eel/eel-mate-extensions.h>
#include <eel/eel-gtk-extensions.h>
#include <eel/eel-gtk-macros.h>
#include <eel/eel-stock-dialogs.h>
#include <eel/eel-string.h>
#include <eel/eel-vfs-extensions.h>

#include <libcaja-private/caja-recent.h>
#include <libcaja-extension/caja-menu-provider.h>
#include <libcaja-private/caja-clipboard.h>
#include <libcaja-private/caja-clipboard-monitor.h>
#include <libcaja-private/caja-debug-log.h>
#include <libcaja-private/caja-desktop-icon-file.h>
#include <libcaja-private/caja-desktop-directory.h>
#include <libcaja-private/caja-extensions.h>
#include <libcaja-private/caja-search-directory.h>
#include <libcaja-private/caja-directory-background.h>
#include <libcaja-private/caja-directory.h>
#include <libcaja-private/caja-dnd.h>
#include <libcaja-private/caja-file-attributes.h>
#include <libcaja-private/caja-file-changes-queue.h>
#include <libcaja-private/caja-file-dnd.h>
#include <libcaja-private/caja-file-operations.h>
#include <libcaja-private/caja-file-utilities.h>
#include <libcaja-private/caja-file-private.h> /* for caja_file_get_existing_by_uri */
#include <libcaja-private/caja-global-preferences.h>
#include <libcaja-private/caja-link.h>
#include <libcaja-private/caja-metadata.h>
#include <libcaja-private/caja-mime-actions.h>
#include <libcaja-private/caja-module.h>
#include <libcaja-private/caja-program-choosing.h>
#include <libcaja-private/caja-trash-monitor.h>
#include <libcaja-private/caja-ui-utilities.h>
#include <libcaja-private/caja-signaller.h>
#include <libcaja-private/caja-autorun.h>
#include <libcaja-private/caja-icon-names.h>
#include <libcaja-private/caja-undostack-manager.h>

#include "fm-directory-view.h"
#include "fm-list-view.h"
#include "fm-desktop-icon-view.h"
#include "fm-actions.h"
#include "fm-error-reporting.h"
#include "fm-marshal.h"
#include "fm-properties-window.h"
#include "libcaja-private/caja-open-with-dialog.h"

/* Minimum starting update inverval */
#define UPDATE_INTERVAL_MIN 100
/* Maximum update interval */
#define UPDATE_INTERVAL_MAX 2000
/* Amount of miliseconds the update interval is increased */
#define UPDATE_INTERVAL_INC 250
/* Interval at which the update interval is increased */
#define UPDATE_INTERVAL_TIMEOUT_INTERVAL 250
/* Milliseconds that have to pass without a change to reset the update interval */
#define UPDATE_INTERVAL_RESET 1000

#define SILENT_WINDOW_OPEN_LIMIT 5

#define DUPLICATE_HORIZONTAL_ICON_OFFSET 70
#define DUPLICATE_VERTICAL_ICON_OFFSET   30

#define MAX_QUEUED_UPDATES 500

#define FM_DIRECTORY_VIEW_MENU_PATH_APPLICATIONS_SUBMENU_PLACEHOLDER   	"/MenuBar/File/Open Placeholder/Open With/Applications Placeholder"
#define FM_DIRECTORY_VIEW_MENU_PATH_APPLICATIONS_PLACEHOLDER    	"/MenuBar/File/Open Placeholder/Applications Placeholder"
#define FM_DIRECTORY_VIEW_MENU_PATH_SCRIPTS_PLACEHOLDER    		"/MenuBar/File/Open Placeholder/Scripts/Scripts Placeholder"
#define FM_DIRECTORY_VIEW_MENU_PATH_EXTENSION_ACTIONS_PLACEHOLDER       "/MenuBar/Edit/Extension Actions"
#define FM_DIRECTORY_VIEW_MENU_PATH_NEW_DOCUMENTS_PLACEHOLDER  		"/MenuBar/File/New Items Placeholder/New Documents/New Documents Placeholder"
#define FM_DIRECTORY_VIEW_MENU_PATH_OPEN				"/MenuBar/File/Open Placeholder/Open"

#define FM_DIRECTORY_VIEW_POPUP_PATH_SELECTION				"/selection"
#define FM_DIRECTORY_VIEW_POPUP_PATH_APPLICATIONS_SUBMENU_PLACEHOLDER  	"/selection/Open Placeholder/Open With/Applications Placeholder"
#define FM_DIRECTORY_VIEW_POPUP_PATH_APPLICATIONS_PLACEHOLDER    	"/selection/Open Placeholder/Applications Placeholder"
#define FM_DIRECTORY_VIEW_POPUP_PATH_SCRIPTS_PLACEHOLDER    		"/selection/Open Placeholder/Scripts/Scripts Placeholder"
#define FM_DIRECTORY_VIEW_POPUP_PATH_EXTENSION_ACTIONS			"/selection/Extension Actions"
#define FM_DIRECTORY_VIEW_POPUP_PATH_OPEN				"/selection/Open Placeholder/Open"

#define FM_DIRECTORY_VIEW_POPUP_PATH_BACKGROUND				"/background"
#define FM_DIRECTORY_VIEW_POPUP_PATH_BACKGROUND_SCRIPTS_PLACEHOLDER	"/background/Before Zoom Items/New Object Items/Scripts/Scripts Placeholder"
#define FM_DIRECTORY_VIEW_POPUP_PATH_BACKGROUND_NEW_DOCUMENTS_PLACEHOLDER "/background/Before Zoom Items/New Object Items/New Documents/New Documents Placeholder"

#define FM_DIRECTORY_VIEW_POPUP_PATH_LOCATION				"/location"

#define MAX_MENU_LEVELS 5
#define TEMPLATE_LIMIT 30

enum {
	ADD_FILE,
	BEGIN_FILE_CHANGES,
	BEGIN_LOADING,
	CLEAR,
	END_FILE_CHANGES,
	FLUSH_ADDED_FILES,
	END_LOADING,
	FILE_CHANGED,
	LOAD_ERROR,
	MOVE_COPY_ITEMS,
	REMOVE_FILE,
	TRASH,
	DELETE,
	LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_WINDOW_SLOT
};

static guint signals[LAST_SIGNAL] = { 0 };

static GdkAtom copied_files_atom;

static char *scripts_directory_uri;
static int scripts_directory_uri_length;

struct FMDirectoryViewDetails
{
	CajaWindowInfo *window;
	CajaWindowSlotInfo *slot;
	CajaDirectory *model;
	CajaFile *directory_as_file;
	CajaFile *location_popup_directory_as_file;
	GdkEventButton *location_popup_event;
	GtkActionGroup *dir_action_group;
	guint dir_merge_id;

	GList *scripts_directory_list;
	GtkActionGroup *scripts_action_group;
	guint scripts_merge_id;

	GList *templates_directory_list;
	GtkActionGroup *templates_action_group;
	guint templates_merge_id;

	GtkActionGroup *extensions_menu_action_group;
	guint extensions_menu_merge_id;

	guint display_selection_idle_id;
	guint update_menus_timeout_id;
	guint update_status_idle_id;
	guint reveal_selection_idle_id;

	guint display_pending_source_id;
	guint changes_timeout_id;

	guint update_interval;
 	guint64 last_queued;

	guint files_added_handler_id;
	guint files_changed_handler_id;
	guint load_error_handler_id;
	guint done_loading_handler_id;
	guint file_changed_handler_id;

	guint delayed_rename_file_id;

	GList *new_added_files;
	GList *new_changed_files;

	GHashTable *non_ready_files;

	GList *old_added_files;
	GList *old_changed_files;

	GList *pending_locations_selected;

	/* whether we are in the active slot */
	gboolean active;

	/* loading indicates whether this view has begun loading a directory.
	 * This flag should need not be set inside subclasses. FMDirectoryView automatically
	 * sets 'loading' to TRUE before it begins loading a directory's contents and to FALSE
	 * after it finishes loading the directory and its view.
	 */
	gboolean loading;
	gboolean menu_states_untrustworthy;
	gboolean scripts_invalid;
	gboolean templates_invalid;
	gboolean reported_load_error;

	/* flag to indicate that no file updates should be dispatched to subclasses.
	 * This is a workaround for bug #87701 that prevents the list view from
	 * losing focus when the underlying GtkTreeView is updated.
	 */
	gboolean updates_frozen;
	guint	 updates_queued;
	gboolean needs_reload;

	gboolean sort_directories_first;

	gboolean show_foreign_files;
	gboolean show_hidden_files;
	gboolean ignore_hidden_file_preferences;

	gboolean show_backup_files;

	gboolean batching_selection_level;
	gboolean selection_changed_while_batched;

	gboolean selection_was_removed;

	gboolean metadata_for_directory_as_file_pending;
	gboolean metadata_for_files_in_directory_pending;

	gboolean selection_change_is_due_to_shell;
	gboolean send_selection_change_to_shell;

	GtkActionGroup *open_with_action_group;
	guint open_with_merge_id;

	GList *subdirectory_list;

	gboolean allow_moves;

	GdkPoint context_menu_position;

	gboolean undo_active;
	gboolean redo_active;
	gchar* undo_action_description;
	gchar* undo_action_label;
	gchar* redo_action_description;
	gchar* redo_action_label;
};

typedef struct {
	CajaFile *file;
	CajaDirectory *directory;
} FileAndDirectory;

/* forward declarations */

static gboolean display_selection_info_idle_callback           (gpointer              data);
static void     fm_directory_view_class_init                   (FMDirectoryViewClass *klass);
static void     fm_directory_view_init                         (FMDirectoryView      *view);
static void     fm_directory_view_duplicate_selection          (FMDirectoryView      *view,
								GList                *files,
								GArray               *item_locations);
static void     fm_directory_view_create_links_for_files       (FMDirectoryView      *view,
								GList                *files,
								GArray               *item_locations);
static void     trash_or_delete_files                          (GtkWindow            *parent_window,
								const GList          *files,
								gboolean              delete_if_all_already_in_trash,
								FMDirectoryView      *view);
static void     load_directory                                 (FMDirectoryView      *view,
								CajaDirectory    *directory);
static void     fm_directory_view_merge_menus                  (FMDirectoryView      *view);
static void     fm_directory_view_unmerge_menus                (FMDirectoryView      *view);
static void     fm_directory_view_init_show_hidden_files       (FMDirectoryView      *view);
static void     fm_directory_view_init_show_backup_files       (FMDirectoryView      *view);
static void     fm_directory_view_load_location                (CajaView         *caja_view,
								const char           *location);
static void     fm_directory_view_stop_loading                 (CajaView         *caja_view);
static void     fm_directory_view_drop_proxy_received_uris     (FMDirectoryView *view,
								const GList *source_uri_list,
								const char *target_uri,
								GdkDragAction action);
static void     fm_directory_view_drop_proxy_received_netscape_url (FMDirectoryView *view,
								    const char *netscape_url,
								    const char *target_uri,
								    GdkDragAction action);
static void     clipboard_changed_callback                     (CajaClipboardMonitor *monitor,
								FMDirectoryView      *view);
static void     open_one_in_new_window                         (gpointer              data,
								gpointer              callback_data);
static void     open_one_in_folder_window                      (gpointer              data,
								gpointer              callback_data);
static void     schedule_update_menus                          (FMDirectoryView      *view);
static void     schedule_update_menus_callback                 (gpointer              callback_data);
static void     remove_update_menus_timeout_callback           (FMDirectoryView      *view);
static void     schedule_update_status                          (FMDirectoryView      *view);
static void     remove_update_status_idle_callback             (FMDirectoryView *view);
static void     reset_update_interval                          (FMDirectoryView      *view);
static void     schedule_idle_display_of_pending_files         (FMDirectoryView      *view);
static void     unschedule_display_of_pending_files            (FMDirectoryView      *view);
static void     disconnect_model_handlers                      (FMDirectoryView      *view);
static void     metadata_for_directory_as_file_ready_callback  (CajaFile         *file,
								gpointer              callback_data);
static void     metadata_for_files_in_directory_ready_callback (CajaDirectory    *directory,
								GList                *files,
								gpointer              callback_data);
static void     fm_directory_view_trash_state_changed_callback (CajaTrashMonitor *trash,
								gboolean              state,
								gpointer              callback_data);
static void     fm_directory_view_select_file                  (FMDirectoryView      *view,
								CajaFile         *file);

static GdkDragAction ask_link_action                           (FMDirectoryView      *view);
static void     update_templates_directory                     (FMDirectoryView *view);
static void     user_dirs_changed                              (FMDirectoryView *view);
static void     fm_directory_view_set_is_active                (FMDirectoryView *view,
								gboolean         is_active);

static gboolean file_list_all_are_folders                      (GList *file_list);

static void action_open_scripts_folder_callback    (GtkAction *action,
						    gpointer   callback_data);
static void action_cut_files_callback              (GtkAction *action,
						    gpointer   callback_data);
static void action_copy_files_callback             (GtkAction *action,
						    gpointer   callback_data);
static void action_paste_files_callback            (GtkAction *action,
						    gpointer   callback_data);
static void action_copy_to_next_pane_callback      (GtkAction *action,
						    gpointer   callback_data);
static void action_move_to_next_pane_callback      (GtkAction *action,
						    gpointer   callback_data);
static void action_rename_callback                 (GtkAction *action,
						    gpointer   callback_data);
static void action_rename_select_all_callback      (GtkAction *action,
						    gpointer   callback_data);
static void action_paste_files_into_callback       (GtkAction *action,
						    gpointer   callback_data);
static void action_connect_to_server_link_callback (GtkAction *action,
						    gpointer   data);
static void action_mount_volume_callback           (GtkAction *action,
						    gpointer   data);
static void action_unmount_volume_callback         (GtkAction *action,
						    gpointer   data);
static void action_format_volume_callback          (GtkAction *action,
						    gpointer   data);
static void action_start_volume_callback           (GtkAction *action,
						    gpointer   data);
static void action_stop_volume_callback            (GtkAction *action,
						    gpointer   data);
static void action_detect_media_callback           (GtkAction *action,
						    gpointer   data);

/* location popup-related actions */

static void action_location_open_alternate_callback (GtkAction *action,
						     gpointer   callback_data);
static void action_location_open_folder_window_callback (GtkAction *action,
							 gpointer   callback_data);

static void action_location_cut_callback            (GtkAction *action,
						     gpointer   callback_data);
static void action_location_copy_callback           (GtkAction *action,
						     gpointer   callback_data);
static void action_location_trash_callback          (GtkAction *action,
						     gpointer   callback_data);
static void action_location_delete_callback         (GtkAction *action,
						     gpointer   callback_data);
static void action_location_properties_callback     (GtkAction *action,
						     gpointer   callback_data);

static void unschedule_pop_up_location_context_menu (FMDirectoryView *view);

static inline void fm_directory_view_widget_to_file_operation_position (FMDirectoryView *view,
									GdkPoint *position);
static void        fm_directory_view_widget_to_file_operation_position_xy (FMDirectoryView *view,
									   int *x, int *y);

/* undo-related actions */

static void undo_redo_menu_update_callback (CajaUndoStackManager* manager, gpointer arg1, gpointer data);

static void undo_update_menu (FMDirectoryView *view);

static void finish_undoredo_callback (gpointer data);

static void real_action_undo (FMDirectoryView *view);

static void real_action_redo (FMDirectoryView *view);

static void action_undo_callback (GtkAction *action, gpointer callback_data);

static void action_redo_callback (GtkAction *action, gpointer callback_data);

EEL_CLASS_BOILERPLATE (FMDirectoryView, fm_directory_view, GTK_TYPE_SCROLLED_WINDOW)

EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, add_file)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, bump_zoom_level)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, can_zoom_in)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, can_zoom_out)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, clear)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, file_changed)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, get_background_widget)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, get_selection)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, get_selection_for_file_transfer)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, get_item_count)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, is_empty)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, reset_to_defaults)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, restore_default_zoom_level)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, select_all)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, set_selection)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, zoom_to_level)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, get_zoom_level)
EEL_IMPLEMENT_MUST_OVERRIDE_SIGNAL (fm_directory_view, invert_selection)

typedef struct {
	GAppInfo *application;
	GList *files;
	FMDirectoryView *directory_view;
} ApplicationLaunchParameters;

typedef struct {
	CajaFile *file;
	FMDirectoryView *directory_view;
} ScriptLaunchParameters;

typedef struct {
	CajaFile *file;
	FMDirectoryView *directory_view;
} CreateTemplateParameters;

static ApplicationLaunchParameters *
application_launch_parameters_new (GAppInfo *application,
			      	   GList *files,
			           FMDirectoryView *directory_view)
{
	ApplicationLaunchParameters *result;

	result = g_new0 (ApplicationLaunchParameters, 1);
	result->application = g_object_ref (application);
	result->files = caja_file_list_copy (files);

	if (directory_view != NULL) {
		g_object_ref (directory_view);
		result->directory_view = directory_view;
	}

	return result;
}

static void
application_launch_parameters_free (ApplicationLaunchParameters *parameters)
{
	g_object_unref (parameters->application);
	caja_file_list_free (parameters->files);

	if (parameters->directory_view != NULL) {
		g_object_unref (parameters->directory_view);
	}

	g_free (parameters);
}

static GList *
file_and_directory_list_to_files (GList *fad_list)
{
	GList *res, *l;
	FileAndDirectory *fad = NULL;

	res = NULL;
	for (l = fad_list; l != NULL; l = l->next) {
		fad = l->data;
		res = g_list_prepend (res, caja_file_ref (fad->file));
	}
	return g_list_reverse (res);
}

static GList *
file_and_directory_list_from_files (CajaDirectory *directory, GList *files)
{
	GList *res, *l;
	FileAndDirectory *fad = NULL;

	res = NULL;
	for (l = files; l != NULL; l = l->next) {
		fad = g_new0 (FileAndDirectory, 1);
		fad->directory = caja_directory_ref (directory);
		fad->file = caja_file_ref (l->data);
		res = g_list_prepend (res, fad);
	}
	return g_list_reverse (res);
}

static void
file_and_directory_free (FileAndDirectory *fad)
{
	caja_directory_unref (fad->directory);
	caja_file_unref (fad->file);
	g_free (fad);
}

static void
file_and_directory_list_free (GList *list)
{
	GList *l;

	for (l = list; l != NULL; l = l->next) {
		file_and_directory_free (l->data);
	}

	g_list_free (list);
}

static gboolean
file_and_directory_equal (gconstpointer  v1,
			  gconstpointer  v2)
{
	const FileAndDirectory *fad1, *fad2;
	fad1 = v1;
	fad2 = v2;

	return (fad1->file == fad2->file &&
		fad1->directory == fad2->directory);
}

static guint
file_and_directory_hash  (gconstpointer  v)
{
	const FileAndDirectory *fad;

	fad = v;
	return GPOINTER_TO_UINT (fad->file) ^ GPOINTER_TO_UINT (fad->directory);
}

static ScriptLaunchParameters *
script_launch_parameters_new (CajaFile *file,
			      FMDirectoryView *directory_view)
{
	ScriptLaunchParameters *result;

	result = g_new0 (ScriptLaunchParameters, 1);
	g_object_ref (directory_view);
	result->directory_view = directory_view;
	caja_file_ref (file);
	result->file = file;

	return result;
}

static void
script_launch_parameters_free (ScriptLaunchParameters *parameters)
{
	g_object_unref (parameters->directory_view);
	caja_file_unref (parameters->file);
	g_free (parameters);
}

static CreateTemplateParameters *
create_template_parameters_new (CajaFile *file,
				FMDirectoryView *directory_view)
{
	CreateTemplateParameters *result;

	result = g_new0 (CreateTemplateParameters, 1);
	g_object_ref (directory_view);
	result->directory_view = directory_view;
	caja_file_ref (file);
	result->file = file;

	return result;
}

static void
create_templates_parameters_free (CreateTemplateParameters *parameters)
{
	g_object_unref (parameters->directory_view);
	caja_file_unref (parameters->file);
	g_free (parameters);
}

CajaWindowInfo *
fm_directory_view_get_caja_window (FMDirectoryView  *view)
{
	g_assert (view->details->window != NULL);

	return view->details->window;
}

CajaWindowSlotInfo *
fm_directory_view_get_caja_window_slot (FMDirectoryView  *view)
{
	g_assert (view->details->slot != NULL);

	return view->details->slot;
}

/* Returns the GtkWindow that this directory view occupies, or NULL
 * if at the moment this directory view is not in a GtkWindow or the
 * GtkWindow cannot be determined. Primarily used for parenting dialogs.
 */
GtkWindow *
fm_directory_view_get_containing_window (FMDirectoryView *view)
{
	GtkWidget *window;

	g_assert (FM_IS_DIRECTORY_VIEW (view));

	window = gtk_widget_get_ancestor (GTK_WIDGET (view), GTK_TYPE_WINDOW);
	if (window == NULL) {
		return NULL;
	}

	return GTK_WINDOW (window);
}

static gboolean
fm_directory_view_confirm_multiple (GtkWindow *parent_window,
				    int count,
				    gboolean tabs)
{
	GtkDialog *dialog;
	char *prompt;
	char *detail;
	int response;

	if (count <= SILENT_WINDOW_OPEN_LIMIT) {
		return TRUE;
	}

	prompt = _("Are you sure you want to open all files?");
	if (tabs) {
		detail = g_strdup_printf (ngettext("This will open %'d separate tab.",
						   "This will open %'d separate tabs.", count), count);
	} else {
		detail = g_strdup_printf (ngettext("This will open %'d separate window.",
						   "This will open %'d separate windows.", count), count);
	}
	dialog = eel_show_yes_no_dialog (prompt, detail,
					 "gtk-ok", "process-stop",
					 parent_window);
	g_free (detail);

	response = gtk_dialog_run (dialog);
	gtk_widget_destroy (GTK_WIDGET (dialog));

	return response == GTK_RESPONSE_YES;
}

static gboolean
selection_contains_one_item_in_menu_callback (FMDirectoryView *view, GList *selection)
{
	if (eel_g_list_exactly_one_item (selection)) {
		return TRUE;
	}

	/* If we've requested a menu update that hasn't yet occurred, then
	 * the mismatch here doesn't surprise us, and we won't complain.
	 * Otherwise, we will complain.
	 */
	if (!view->details->menu_states_untrustworthy) {
		g_warning ("Expected one selected item, found %'d. No action will be performed.",
			   g_list_length (selection));
	}

	return FALSE;
}

static gboolean
selection_not_empty_in_menu_callback (FMDirectoryView *view, GList *selection)
{
	if (selection != NULL) {
		return TRUE;
	}

	/* If we've requested a menu update that hasn't yet occurred, then
	 * the mismatch here doesn't surprise us, and we won't complain.
	 * Otherwise, we will complain.
	 */
	if (!view->details->menu_states_untrustworthy) {
		g_warning ("Empty selection found when selection was expected. No action will be performed.");
	}

	return FALSE;
}

static char *
get_view_directory (FMDirectoryView *view)
{
	char *uri, *path;
	GFile *f;

	uri = caja_directory_get_uri (view->details->model);
	if (eel_uri_is_desktop (uri)) {
		g_free (uri);
		uri = caja_get_desktop_directory_uri ();

	}
	f = g_file_new_for_uri (uri);
	path = g_file_get_path (f);
	g_object_unref (f);
	g_free (uri);

	return path;
}

void
fm_directory_view_activate_files (FMDirectoryView *view,
				  GList *files,
				  CajaWindowOpenMode mode,
				  CajaWindowOpenFlags flags,
				  gboolean confirm_multiple)
{
	char *path;

	path = get_view_directory (view);
	caja_mime_activate_files (fm_directory_view_get_containing_window (view),
				      view->details->slot,
				      files,
				      path,
				      mode,
				      flags,
				      confirm_multiple);

	g_free (path);
}

void
fm_directory_view_activate_file (FMDirectoryView *view,
				 CajaFile *file,
				 CajaWindowOpenMode mode,
				 CajaWindowOpenFlags flags)
{
	char *path;

	path = get_view_directory (view);
	caja_mime_activate_file (fm_directory_view_get_containing_window (view),
				     view->details->slot,
				     file,
				     path,
				     mode,
				     flags);

	g_free (path);
}

static void
action_open_callback (GtkAction *action,
		      gpointer callback_data)
{
	GList *selection;
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (callback_data);

	selection = fm_directory_view_get_selection (view);
	fm_directory_view_activate_files (view,
					  selection,
					  CAJA_WINDOW_OPEN_ACCORDING_TO_MODE,
					  0,
					  TRUE);
	caja_file_list_free (selection);
}

static void
action_open_close_parent_callback (GtkAction *action,
				   gpointer callback_data)
{
	GList *selection;
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (callback_data);

	selection = fm_directory_view_get_selection (view);
	fm_directory_view_activate_files (view,
					  selection,
					  CAJA_WINDOW_OPEN_ACCORDING_TO_MODE,
					  CAJA_WINDOW_OPEN_FLAG_CLOSE_BEHIND,
					  TRUE);
	caja_file_list_free (selection);
}

static void
action_open_alternate_callback (GtkAction *action,
				gpointer callback_data)
{
	FMDirectoryView *view;
	GList *selection;
	GtkWindow *window;

	view = FM_DIRECTORY_VIEW (callback_data);
	selection = fm_directory_view_get_selection (view);

	window = fm_directory_view_get_containing_window (view);

	if (fm_directory_view_confirm_multiple (window, g_list_length (selection), FALSE)) {
		g_list_foreach (selection, open_one_in_new_window, view);
	}

	caja_file_list_free (selection);
}

static void
action_open_new_tab_callback (GtkAction *action,
			      gpointer callback_data)
{
	FMDirectoryView *view;
	GList *selection;
	GtkWindow *window;

	view = FM_DIRECTORY_VIEW (callback_data);
	selection = fm_directory_view_get_selection (view);

	window = fm_directory_view_get_containing_window (view);

	if (fm_directory_view_confirm_multiple (window, g_list_length (selection), TRUE)) {
		fm_directory_view_activate_files (view,
						  selection,
						  CAJA_WINDOW_OPEN_ACCORDING_TO_MODE,
						  CAJA_WINDOW_OPEN_FLAG_NEW_TAB,
						  FALSE);
	}

	caja_file_list_free (selection);
}

static void
action_open_folder_window_callback (GtkAction *action,
				gpointer callback_data)
{
	FMDirectoryView *view;
	GList *selection;
	GtkWindow *window;

	view = FM_DIRECTORY_VIEW (callback_data);
	selection = fm_directory_view_get_selection (view);

	window = fm_directory_view_get_containing_window (view);

	if (fm_directory_view_confirm_multiple (window, g_list_length (selection), FALSE)) {
		g_list_foreach (selection, open_one_in_folder_window, view);
	}

	caja_file_list_free (selection);
}

static void
open_location (FMDirectoryView *directory_view,
	       const char *new_uri,
	       CajaWindowOpenMode mode,
	       CajaWindowOpenFlags flags)
{
	GtkWindow *window;
	GFile *location;

	g_assert (FM_IS_DIRECTORY_VIEW (directory_view));
	g_assert (new_uri != NULL);

	window = fm_directory_view_get_containing_window (directory_view);
	caja_debug_log (FALSE, CAJA_DEBUG_LOG_DOMAIN_USER,
			    "directory view open_location window=%p: %s", window, new_uri);
	location = g_file_new_for_uri (new_uri);
	caja_window_slot_info_open_location (directory_view->details->slot,
						 location, mode, flags, NULL);
	g_object_unref (location);
}

static void
application_selected_cb (CajaOpenWithDialog *dialog,
			 GAppInfo *app,
			 gpointer user_data)
{
	GtkWindow *parent_window;
	CajaFile *file;
	GList files;

	parent_window = GTK_WINDOW (user_data);

	file = g_object_get_data (G_OBJECT (dialog), "directory-view:file");

	files.next = NULL;
	files.prev = NULL;
	files.data = file;
	caja_launch_application (app, &files, parent_window);
}

static void
choose_program (FMDirectoryView *view,
		CajaFile *file)
{
	GtkWidget *dialog;
	char *uri;
	char *mime_type;

	g_assert (FM_IS_DIRECTORY_VIEW (view));
	g_assert (CAJA_IS_FILE (file));

	caja_file_ref (file);
	uri = caja_file_get_uri (file);
	mime_type = caja_file_get_mime_type (file);

	dialog = caja_open_with_dialog_new (uri, mime_type, NULL);
	g_object_set_data_full (G_OBJECT (dialog),
				"directory-view:file",
				g_object_ref (file),
				(GDestroyNotify)g_object_unref);

	gtk_window_set_screen (GTK_WINDOW (dialog),
			       gtk_widget_get_screen (GTK_WIDGET (view)));
	gtk_widget_show (dialog);

	g_signal_connect_object (dialog,
				 "application_selected",
				 G_CALLBACK (application_selected_cb),
				 fm_directory_view_get_containing_window (view),
				 0);

 	g_free (uri);
	g_free (mime_type);
	caja_file_unref (file);
}

static void
open_with_other_program (FMDirectoryView *view)
{
        GList *selection;

	g_assert (FM_IS_DIRECTORY_VIEW (view));

       	selection = fm_directory_view_get_selection (view);

	if (selection_contains_one_item_in_menu_callback (view, selection)) {
		choose_program (view, CAJA_FILE (selection->data));
	}

	caja_file_list_free (selection);
}

static void
action_other_application_callback (GtkAction *action,
				   gpointer callback_data)
{
	g_assert (FM_IS_DIRECTORY_VIEW (callback_data));

	open_with_other_program (FM_DIRECTORY_VIEW (callback_data));
}

static void
trash_or_delete_selected_files (FMDirectoryView *view)
{
	/* This might be rapidly called multiple times for the same selection
	 * when using keybindings. So we remember if the current selection
	 * was already removed (but the view doesn't know about it yet).
	 */
	if (!view->details->selection_was_removed) {
	        GList *selection;

		selection = fm_directory_view_get_selection_for_file_transfer (view);
		trash_or_delete_files (fm_directory_view_get_containing_window (view),
				       selection, TRUE,
				       view);
		caja_file_list_free (selection);
		view->details->selection_was_removed = TRUE;
	}
}

static gboolean
real_trash (FMDirectoryView *view)
{
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	GtkAction *action;

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_TRASH);
	if (gtk_action_get_sensitive (action) &&
	    gtk_action_get_visible (action)) {
		trash_or_delete_selected_files (view);
		return TRUE;
	}
	G_GNUC_END_IGNORE_DEPRECATIONS;
	return FALSE;
}

static void
action_trash_callback (GtkAction *action,
		       gpointer callback_data)
{
        trash_or_delete_selected_files (FM_DIRECTORY_VIEW (callback_data));
}

static void
delete_selected_files (FMDirectoryView *view)
{
        GList *selection;
	GList *node;
	GList *locations;

	selection = fm_directory_view_get_selection_for_file_transfer (view);
	if (selection == NULL) {
		return;
	}

	locations = NULL;
	for (node = selection; node != NULL; node = node->next) {
		locations = g_list_prepend (locations,
					    caja_file_get_location ((CajaFile *) node->data));
	}
	locations = g_list_reverse (locations);

	caja_file_operations_delete (locations, fm_directory_view_get_containing_window (view), NULL, NULL);

	g_list_free_full (locations, g_object_unref);
        caja_file_list_free (selection);
}

static void
action_delete_callback (GtkAction *action,
			gpointer callback_data)
{
        delete_selected_files (FM_DIRECTORY_VIEW (callback_data));
}

static void
action_restore_from_trash_callback (GtkAction *action,
				    gpointer callback_data)
{
	FMDirectoryView *view;
	GList *selection;

	view = FM_DIRECTORY_VIEW (callback_data);

	selection = fm_directory_view_get_selection_for_file_transfer (view);
	caja_restore_files_from_trash (selection,
					   fm_directory_view_get_containing_window (view));

	caja_file_list_free (selection);

}

static gboolean
real_delete (FMDirectoryView *view)
{
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	GtkAction *action;

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_TRASH);
	if (gtk_action_get_sensitive (action) &&
	    gtk_action_get_visible (action)) {
		delete_selected_files (view);
		return TRUE;
	}
	G_GNUC_END_IGNORE_DEPRECATIONS;
	return FALSE;
}

static void
action_duplicate_callback (GtkAction *action,
			   gpointer callback_data)
{
        FMDirectoryView *view;
        GList *selection;

        view = FM_DIRECTORY_VIEW (callback_data);
	selection = fm_directory_view_get_selection_for_file_transfer (view);
	if (selection_not_empty_in_menu_callback (view, selection)) {
	        GArray *selected_item_locations;

		/* FIXME bugzilla.gnome.org 45061:
		 * should change things here so that we use a get_icon_locations (view, selection).
		 * Not a problem in this case but in other places the selection may change by
		 * the time we go and retrieve the icon positions, relying on the selection
		 * staying intact to ensure the right sequence and count of positions is fragile.
		 */
		selected_item_locations = fm_directory_view_get_selected_icon_locations (view);
	        fm_directory_view_duplicate_selection (view, selection, selected_item_locations);
	        g_array_free (selected_item_locations, TRUE);
	}

        caja_file_list_free (selection);
}

static void
action_create_link_callback (GtkAction *action,
			     gpointer callback_data)
{
        FMDirectoryView *view;
        GList *selection;

        g_assert (FM_IS_DIRECTORY_VIEW (callback_data));

        view = FM_DIRECTORY_VIEW (callback_data);
	selection = fm_directory_view_get_selection (view);
	if (selection_not_empty_in_menu_callback (view, selection)) {
	        GArray *selected_item_locations;

		selected_item_locations = fm_directory_view_get_selected_icon_locations (view);
	        fm_directory_view_create_links_for_files (view, selection, selected_item_locations);
	        g_array_free (selected_item_locations, TRUE);
	}

        caja_file_list_free (selection);
}

static void
action_select_all_callback (GtkAction *action,
			    gpointer callback_data)
{
	g_assert (FM_IS_DIRECTORY_VIEW (callback_data));

	fm_directory_view_select_all (callback_data);
}

static void
action_invert_selection_callback (GtkAction *action,
				  gpointer callback_data)
{
	g_assert (FM_IS_DIRECTORY_VIEW (callback_data));

	fm_directory_view_invert_selection (callback_data);
}

static void
pattern_select_response_cb (GtkWidget *dialog, int response, gpointer user_data)
{
	FMDirectoryView *view;
	CajaDirectory *directory;
	GtkWidget *entry;
	GList *selection;
	GError *error;

	view = FM_DIRECTORY_VIEW (user_data);

	switch (response) {
	case GTK_RESPONSE_OK :
		entry = g_object_get_data (G_OBJECT (dialog), "entry");
		directory = fm_directory_view_get_model (view);
		selection = caja_directory_match_pattern (directory,
					gtk_entry_get_text (GTK_ENTRY (entry)));

		if (selection) {
			fm_directory_view_set_selection (view, selection);
			caja_file_list_free (selection);

			fm_directory_view_reveal_selection(view);
		}
		/* fall through */
	case GTK_RESPONSE_NONE :
	case GTK_RESPONSE_DELETE_EVENT :
	case GTK_RESPONSE_CANCEL :
		gtk_widget_destroy (GTK_WIDGET (dialog));
		break;
	case GTK_RESPONSE_HELP :
		error = NULL;
		gtk_show_uri_on_window (GTK_WINDOW (dialog),
			                "help:mate-user-guide/caja-select-pattern",
			                gtk_get_current_event_time (), &error);
		if (error) {
			eel_show_error_dialog (_("There was an error displaying help."), error->message,
					       GTK_WINDOW (dialog));
			g_error_free (error);
		}
		break;
	default :
		g_assert_not_reached ();
	}
}

static void
select_pattern (FMDirectoryView *view)
{
	GtkWidget *dialog;
	GtkWidget *label;
	GtkWidget *example;
	GtkWidget *grid;
	GtkWidget *entry;
	char *example_pattern;

	dialog = gtk_dialog_new ();
	gtk_window_set_title (GTK_WINDOW (dialog), _("Select Items Matching"));
	gtk_window_set_transient_for (GTK_WINDOW (dialog), fm_directory_view_get_containing_window (view));
	gtk_window_set_destroy_with_parent (GTK_WINDOW (dialog), TRUE);

	eel_dialog_add_button (GTK_DIALOG (dialog),
			       _("_Help"),
			       "help-browser",
			       GTK_RESPONSE_HELP);

	eel_dialog_add_button (GTK_DIALOG (dialog),
			       _("_Cancel"),
			       "process-stop",
			       GTK_RESPONSE_CANCEL);

	eel_dialog_add_button (GTK_DIALOG (dialog),
			       _("_OK"),
			       "gtk-ok",
			       GTK_RESPONSE_OK);

	gtk_dialog_set_default_response (GTK_DIALOG (dialog),
					 GTK_RESPONSE_OK);
	gtk_container_set_border_width (GTK_CONTAINER (dialog), 5);
	gtk_box_set_spacing (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))), 2);

	label = gtk_label_new_with_mnemonic (_("_Pattern:"));

	gtk_widget_set_halign (label, GTK_ALIGN_START);

	example = gtk_label_new (NULL);
	gtk_widget_set_halign (example, GTK_ALIGN_START);
	example_pattern = g_strdup_printf ("<b>%s</b><i>%s</i>",
					   _("Examples: "),
					   "*.png, file\?\?.txt, pict*.\?\?\?");
	gtk_label_set_markup (GTK_LABEL (example), example_pattern);
	g_free (example_pattern);
	gtk_widget_set_halign (example, GTK_ALIGN_START);

	entry = gtk_entry_new ();
	gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
	gtk_widget_set_hexpand (entry, TRUE);

	grid = gtk_grid_new ();
	g_object_set (grid,
		      "orientation", GTK_ORIENTATION_VERTICAL,
		      "border-width", 6,
		      "row-spacing", 6,
		      "column-spacing", 12,
		      NULL);

	gtk_container_add (GTK_CONTAINER (grid), label);
	gtk_grid_attach_next_to (GTK_GRID (grid), entry, label,
				 GTK_POS_RIGHT, 1, 1);
	gtk_grid_attach_next_to (GTK_GRID (grid), example, entry,
				 GTK_POS_BOTTOM, 1, 1);

	gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);
	gtk_widget_show_all (grid);
	gtk_container_add (GTK_CONTAINER (gtk_dialog_get_content_area (GTK_DIALOG (dialog))), grid);

	g_object_set_data (G_OBJECT (dialog), "entry", entry);
	g_signal_connect (dialog, "response",
			  G_CALLBACK (pattern_select_response_cb),
			  view);
	gtk_widget_show_all (dialog);
}

static void
action_select_pattern_callback (GtkAction *action,
				gpointer callback_data)
{
	g_assert (FM_IS_DIRECTORY_VIEW (callback_data));

	select_pattern(callback_data);
}

static void
action_reset_to_defaults_callback (GtkAction *action,
				   gpointer callback_data)
{
	g_assert (FM_IS_DIRECTORY_VIEW (callback_data));

	fm_directory_view_reset_to_defaults (callback_data);
}

static void
hidden_files_mode_changed (CajaWindow *window,
			   gpointer callback_data)
{
	FMDirectoryView *directory_view;

	directory_view = FM_DIRECTORY_VIEW (callback_data);

	fm_directory_view_init_show_hidden_files (directory_view);
}

static void
backup_files_mode_changed (CajaWindow *window,
			   gpointer callback_data)
{
	FMDirectoryView *directory_view;

	directory_view = FM_DIRECTORY_VIEW (callback_data);

	fm_directory_view_init_show_backup_files (directory_view);
}

static void
action_save_search_callback (GtkAction *action,
			     gpointer callback_data)
{
	FMDirectoryView	*directory_view;

        directory_view = FM_DIRECTORY_VIEW (callback_data);

	if (directory_view->details->model &&
	    CAJA_IS_SEARCH_DIRECTORY (directory_view->details->model)) {
		CajaSearchDirectory *search;

		search = CAJA_SEARCH_DIRECTORY (directory_view->details->model);
		caja_search_directory_save_search (search);

		/* Save search is disabled */
		schedule_update_menus (directory_view);
	}
}

static void
query_name_entry_changed_cb  (GtkWidget *entry, GtkWidget *button)
{
	const char *text;
	gboolean sensitive;

	text = gtk_entry_get_text (GTK_ENTRY (entry));

	sensitive = (text != NULL) && (*text != 0);

	gtk_widget_set_sensitive (button, sensitive);
}

static void
action_save_search_as_callback (GtkAction *action,
				gpointer callback_data)
{
	FMDirectoryView	*directory_view;

        directory_view = FM_DIRECTORY_VIEW (callback_data);

	if (directory_view->details->model &&
	    CAJA_IS_SEARCH_DIRECTORY (directory_view->details->model)) {
		CajaSearchDirectory *search;
		GtkWidget *dialog, *grid, *label, *entry, *chooser, *save_button;

		search = CAJA_SEARCH_DIRECTORY (directory_view->details->model);

		dialog = gtk_dialog_new ();
		gtk_window_set_title (GTK_WINDOW (dialog), _("Save Search as"));
		gtk_window_set_transient_for (GTK_WINDOW (dialog), fm_directory_view_get_containing_window (directory_view));

		eel_dialog_add_button (GTK_DIALOG (dialog),
				       _("_Cancel"),
				       "process-stop",
				       GTK_RESPONSE_CANCEL);

		save_button = eel_dialog_add_button (GTK_DIALOG (dialog), _("_Save"),
						     "document-save", GTK_RESPONSE_OK);

		gtk_dialog_set_default_response (GTK_DIALOG (dialog),
						 GTK_RESPONSE_OK);
		gtk_container_set_border_width (GTK_CONTAINER (dialog), 5);
		gtk_box_set_spacing (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))), 2);
		gtk_window_set_resizable (GTK_WINDOW (dialog), FALSE);

		grid = gtk_grid_new ();
		g_object_set (grid,
			      "orientation", GTK_ORIENTATION_VERTICAL,
			      "border-width", 5,
			      "row-spacing", 6,
			      "column-spacing", 12,
			      NULL);
		gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))), grid, TRUE, TRUE, 0);
		gtk_widget_show (grid);

		label = gtk_label_new_with_mnemonic (_("Search _name:"));
		gtk_label_set_xalign (GTK_LABEL (label), 0.0);
		gtk_container_add (GTK_CONTAINER (grid), label);
		gtk_widget_show (label);
		entry = gtk_entry_new ();
		gtk_widget_set_hexpand (entry, TRUE);
		gtk_grid_attach_next_to (GTK_GRID (grid), entry, label,
					 GTK_POS_RIGHT, 1, 1);
		gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
		gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);

		gtk_widget_set_sensitive (save_button, FALSE);
		g_signal_connect (entry, "changed",
				  G_CALLBACK (query_name_entry_changed_cb), save_button);

		gtk_widget_show (entry);
		label = gtk_label_new_with_mnemonic (_("_Folder:"));
		gtk_label_set_xalign (GTK_LABEL (label), 0.0);
		gtk_container_add (GTK_CONTAINER (grid), label);
		gtk_widget_show (label);

		chooser = gtk_file_chooser_button_new (_("Select Folder to Save Search In"),
						      GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
		gtk_widget_set_hexpand (chooser, TRUE);
		gtk_grid_attach_next_to (GTK_GRID (grid), chooser, label,
					 GTK_POS_RIGHT, 1, 1);
		gtk_label_set_mnemonic_widget (GTK_LABEL (label), chooser);
		gtk_widget_show (chooser);

		gtk_file_chooser_set_local_only (GTK_FILE_CHOOSER (chooser), TRUE);

		gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (chooser),
						     g_get_home_dir ());

		if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK) {
			const char *entry_text;
			char *filename, *filename_utf8, *dirname, *path, *uri;
			GFile *location;

			entry_text = gtk_entry_get_text (GTK_ENTRY (entry));
			if (g_str_has_suffix (entry_text, CAJA_SAVED_SEARCH_EXTENSION)) {
				filename_utf8 = g_strdup (entry_text);
			} else {
				filename_utf8 = g_strconcat (entry_text, CAJA_SAVED_SEARCH_EXTENSION, NULL);
			}

			filename = g_filename_from_utf8 (filename_utf8, -1, NULL, NULL, NULL);
			g_free (filename_utf8);

			dirname = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (chooser));

			path = g_build_filename (dirname, filename, NULL);
			g_free (filename);
			g_free (dirname);

			uri = g_filename_to_uri (path, NULL, NULL);
			g_free (path);

			caja_search_directory_save_to_file (search, uri);
			location = g_file_new_for_uri (uri);
			caja_file_changes_queue_file_added (location);
			g_object_unref (location);
			caja_file_changes_consume_changes (TRUE);
			g_free (uri);
		}

		gtk_widget_destroy (dialog);
	}
}

static void
action_empty_trash_callback (GtkAction *action,
			     gpointer callback_data)
{
        g_assert (FM_IS_DIRECTORY_VIEW (callback_data));

	caja_file_operations_empty_trash (GTK_WIDGET (callback_data));
}

static void
action_new_folder_callback (GtkAction *action,
			    gpointer callback_data)
{
        g_assert (FM_IS_DIRECTORY_VIEW (callback_data));

	fm_directory_view_new_folder (FM_DIRECTORY_VIEW (callback_data));
}

static void
action_new_empty_file_callback (GtkAction *action,
				gpointer callback_data)
{
        g_assert (FM_IS_DIRECTORY_VIEW (callback_data));

	fm_directory_view_new_file (FM_DIRECTORY_VIEW (callback_data), NULL, NULL);
}

static void
action_new_launcher_callback (GtkAction *action,
			      gpointer callback_data)
{
	char *parent_uri;
	FMDirectoryView *view;
	GtkWindow *window;

	g_assert (FM_IS_DIRECTORY_VIEW (callback_data));

	view = FM_DIRECTORY_VIEW (callback_data);

	parent_uri = fm_directory_view_get_backing_uri (view);

	window = fm_directory_view_get_containing_window (view);
	caja_debug_log (FALSE, CAJA_DEBUG_LOG_DOMAIN_USER,
			    "directory view create new launcher in window=%p: %s", window, parent_uri);
	caja_launch_application_from_command (gtk_widget_get_screen (GTK_WIDGET (view)),
						  "mate-desktop-item-edit",
						  "mate-desktop-item-edit",
						  FALSE,
						  "--create-new", parent_uri, NULL);

	g_free (parent_uri);
}

static void
action_properties_callback (GtkAction *action,
			    gpointer callback_data)
{
        FMDirectoryView *view;
        GList *selection;

        g_assert (FM_IS_DIRECTORY_VIEW (callback_data));

        view = FM_DIRECTORY_VIEW (callback_data);
	selection = fm_directory_view_get_selection (view);
	if (g_list_length (selection) == 0) {
		if (view->details->directory_as_file != NULL) {
			GList *files;

			files = g_list_append (NULL, caja_file_ref (view->details->directory_as_file));

			fm_properties_window_present (files, GTK_WIDGET (view));

			caja_file_list_free (files);
		}
	} else {
		fm_properties_window_present (selection, GTK_WIDGET (view));
	}
        caja_file_list_free (selection);
}

static void
action_location_properties_callback (GtkAction *action,
				     gpointer   callback_data)
{
	FMDirectoryView *view;
	GList           *files;

	g_assert (FM_IS_DIRECTORY_VIEW (callback_data));

	view = FM_DIRECTORY_VIEW (callback_data);
	g_assert (CAJA_IS_FILE (view->details->location_popup_directory_as_file));

	files = g_list_append (NULL, caja_file_ref (view->details->location_popup_directory_as_file));

	fm_properties_window_present (files, GTK_WIDGET (view));

	caja_file_list_free (files);
}

static gboolean
all_files_in_trash (GList *files)
{
	GList *node;

	/* Result is ambiguous if called on NULL, so disallow. */
	g_return_val_if_fail (files != NULL, FALSE);

	for (node = files; node != NULL; node = node->next) {
		if (!caja_file_is_in_trash (CAJA_FILE (node->data))) {
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
all_selected_items_in_trash (FMDirectoryView *view)
{
	GList *selection;
	gboolean result;

	/* If the contents share a parent directory, we need only
	 * check that parent directory. Otherwise we have to inspect
	 * each selected item.
	 */
	selection = fm_directory_view_get_selection (view);
	result = (selection == NULL) ? FALSE : all_files_in_trash (selection);
	caja_file_list_free (selection);

	return result;
}

static gboolean
we_are_in_vfolder_desktop_dir (FMDirectoryView *view)
{
	CajaFile *file;
	char *mime_type;

	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	if (view->details->model == NULL) {
		return FALSE;
	}

	file = caja_directory_get_corresponding_file (view->details->model);
	mime_type = caja_file_get_mime_type (file);
	caja_file_unref (file);

	if (mime_type != NULL
	    && strcmp (mime_type, "x-directory/vfolder-desktop") == 0) {
		g_free (mime_type);
		return TRUE;
	} else {
		g_free (mime_type);
		return FALSE;
	}
}

/* Preferences changed callbacks */
static void
text_attribute_names_changed_callback (gpointer callback_data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (callback_data);

	EEL_CALL_METHOD
		(FM_DIRECTORY_VIEW_CLASS, view,
		 text_attribute_names_changed, (view));
}

static void
image_display_policy_changed_callback (gpointer callback_data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (callback_data);

	EEL_CALL_METHOD
		(FM_DIRECTORY_VIEW_CLASS, view,
		 image_display_policy_changed, (view));
}

static void
click_policy_changed_callback (gpointer callback_data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (callback_data);

	EEL_CALL_METHOD
		(FM_DIRECTORY_VIEW_CLASS, view,
		 click_policy_changed, (view));
}

gboolean
fm_directory_view_should_sort_directories_first (FMDirectoryView *view)
{
	return view->details->sort_directories_first;
}

static void
sort_directories_first_changed_callback (gpointer callback_data)
{
	FMDirectoryView *view;
	gboolean preference_value;

	view = FM_DIRECTORY_VIEW (callback_data);

	preference_value =
		g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_SORT_DIRECTORIES_FIRST);

	if (preference_value != view->details->sort_directories_first) {
		view->details->sort_directories_first = preference_value;
		EEL_CALL_METHOD
			(FM_DIRECTORY_VIEW_CLASS, view,
			 sort_directories_first_changed, (view));
	}
}

static void set_up_scripts_directory_global(void)
{
	if (scripts_directory_uri != NULL)
	{
		return;
	}

	char* scripts_directory_path;
	scripts_directory_path = g_build_filename (g_get_user_config_dir (), "caja", "scripts", NULL);

	if (g_mkdir_with_parents(scripts_directory_path, 0755) == 0)
	{
		scripts_directory_uri = g_filename_to_uri(scripts_directory_path, NULL, NULL);
		scripts_directory_uri_length = strlen(scripts_directory_uri);

		/* Support for GNOME Nautilus scripts
		 */
		char* nautilus_scripts_path = g_build_filename(g_get_home_dir(), ".gnome2", "nautilus-scripts", NULL);

		if (g_file_test(nautilus_scripts_path, G_FILE_TEST_IS_DIR) == TRUE)
		{
			char* nautilus_syslink = g_build_filename(g_get_user_config_dir(), "caja", "scripts", "nautilus", NULL);
			/* If link already exists, or also any other kind of file/dir with same name, ignore it */
			if (g_file_test(nautilus_syslink, G_FILE_TEST_IS_SYMLINK) == FALSE &&
				g_file_test(nautilus_syslink, G_FILE_TEST_EXISTS) == FALSE &&
				g_file_test(nautilus_syslink, G_FILE_TEST_IS_DIR) == FALSE)
			{
				/* Check if we need to create a link */
				GDir* dir = g_dir_open(nautilus_scripts_path, 0, NULL);

				if (dir)
				{
					/* If directory contains files, we can create the link */
					int count = 0;

					while (g_dir_read_name(dir) != NULL)
					{
						count++;
					}

					if (count > 0)
					{
						/* Create link to nautilus folder */
						int res = symlink (nautilus_scripts_path, nautilus_syslink);
						if (res != 0)
							g_warning ("Can't create symlink to nautilus scripts folder");
					}

					g_dir_close(dir);
				}
			}

			g_free(nautilus_syslink);
		}

		g_free(nautilus_scripts_path);
	}

	g_free(scripts_directory_path);
}

static void
scripts_added_or_changed_callback (CajaDirectory *directory,
				   GList *files,
				   gpointer callback_data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (callback_data);

	view->details->scripts_invalid = TRUE;
	if (view->details->active) {
		schedule_update_menus (view);
	}
}

static void
templates_added_or_changed_callback (CajaDirectory *directory,
				     GList *files,
				     gpointer callback_data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (callback_data);

	view->details->templates_invalid = TRUE;
	if (view->details->active) {
		schedule_update_menus (view);
	}
}

static void
add_directory_to_directory_list (FMDirectoryView *view,
				 CajaDirectory *directory,
				 GList **directory_list,
				 GCallback changed_callback)
{
	CajaFileAttributes attributes;

	if (g_list_find (*directory_list, directory) == NULL) {
		caja_directory_ref (directory);

		attributes =
			CAJA_FILE_ATTRIBUTES_FOR_ICON |
			CAJA_FILE_ATTRIBUTE_INFO |
			CAJA_FILE_ATTRIBUTE_DIRECTORY_ITEM_COUNT;

		caja_directory_file_monitor_add (directory, directory_list,
						     FALSE, attributes,
						     (CajaDirectoryCallback)changed_callback, view);

		g_signal_connect_object (directory, "files_added",
					 G_CALLBACK (changed_callback), view, 0);
		g_signal_connect_object (directory, "files_changed",
					 G_CALLBACK (changed_callback), view, 0);

		*directory_list = g_list_append	(*directory_list, directory);
	}
}

static void
remove_directory_from_directory_list (FMDirectoryView *view,
				      CajaDirectory *directory,
				      GList **directory_list,
				      GCallback changed_callback)
{
	*directory_list = g_list_remove	(*directory_list, directory);

	g_signal_handlers_disconnect_by_func (directory,
					      G_CALLBACK (changed_callback),
					      view);

	caja_directory_file_monitor_remove (directory, directory_list);

	caja_directory_unref (directory);
}

static void
add_directory_to_scripts_directory_list (FMDirectoryView *view,
					 CajaDirectory *directory)
{
	add_directory_to_directory_list (view, directory,
					 &view->details->scripts_directory_list,
					 G_CALLBACK (scripts_added_or_changed_callback));
}

static void
remove_directory_from_scripts_directory_list (FMDirectoryView *view,
					      CajaDirectory *directory)
{
	remove_directory_from_directory_list (view, directory,
					      &view->details->scripts_directory_list,
					      G_CALLBACK (scripts_added_or_changed_callback));
}

static void
add_directory_to_templates_directory_list (FMDirectoryView *view,
					   CajaDirectory *directory)
{
	add_directory_to_directory_list (view, directory,
					 &view->details->templates_directory_list,
					 G_CALLBACK (templates_added_or_changed_callback));
}

static void
remove_directory_from_templates_directory_list (FMDirectoryView *view,
						CajaDirectory *directory)
{
	remove_directory_from_directory_list (view, directory,
					      &view->details->templates_directory_list,
					      G_CALLBACK (templates_added_or_changed_callback));
}

static void
slot_active (CajaWindowSlot *slot,
	     FMDirectoryView *view)
{
	g_assert (!view->details->active);
	view->details->active = TRUE;

	fm_directory_view_merge_menus (view);
	schedule_update_menus (view);
}

static void
slot_inactive (CajaWindowSlot *slot,
	       FMDirectoryView *view)
{
	g_assert (view->details->active ||
		  gtk_widget_get_parent (GTK_WIDGET (view)) == NULL);
	view->details->active = FALSE;

	fm_directory_view_unmerge_menus (view);
	remove_update_menus_timeout_callback (view);
}

static void
fm_directory_view_grab_focus (CajaView *view)
{
	/* focus the child of the scrolled window if it exists */
	GtkWidget *child;
	child = gtk_bin_get_child (GTK_BIN (view));
	if (child) {
		gtk_widget_grab_focus (GTK_WIDGET (child));
	}
}

static void
view_iface_update_menus (CajaView *view)
{
	fm_directory_view_update_menus (FM_DIRECTORY_VIEW (view));
}

static GtkWidget *
fm_directory_view_get_widget (CajaView *view)
{
	return GTK_WIDGET (view);
}

static int
fm_directory_view_get_selection_count (CajaView *view)
{
	/* FIXME: This could be faster if we special cased it in subclasses */
	GList *files;
	int len;

	files = fm_directory_view_get_selection (FM_DIRECTORY_VIEW (view));
	len = g_list_length (files);
	caja_file_list_free (files);

	return len;
}

static GList *
fm_directory_view_get_selection_locations (CajaView *view)
{
	GList *files;
	GList *locations;
	GList *l;
	GFile *location = NULL;

	files = fm_directory_view_get_selection (FM_DIRECTORY_VIEW (view));
	locations = NULL;
	for (l = files; l != NULL; l = l->next) {
		location = caja_file_get_location (CAJA_FILE (l->data));
		locations = g_list_prepend (locations, location);
	}
	caja_file_list_free (files);

	return g_list_reverse (locations);
}

static GList *
file_list_from_location_list (const GList *uri_list)
{
	GList *file_list;
	const GList *node;

	file_list = NULL;
	for (node = uri_list; node != NULL; node = node->next) {
		file_list = g_list_prepend
			(file_list,
			 caja_file_get (node->data));
	}
	return g_list_reverse (file_list);
}

static void
fm_directory_view_set_selection_locations (CajaView *caja_view,
					   GList *selection_locations)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (caja_view);

	if (!view->details->loading) {
		GList *selection;

		/* If we aren't still loading, set the selection right now,
		 * and reveal the new selection.
		 */
		selection = file_list_from_location_list (selection_locations);
		view->details->selection_change_is_due_to_shell = TRUE;
		fm_directory_view_set_selection (view, selection);
		view->details->selection_change_is_due_to_shell = FALSE;
		fm_directory_view_reveal_selection (view);
		caja_file_list_free (selection);
	} else {
		/* If we are still loading, set the list of pending URIs instead.
		 * done_loading() will eventually select the pending URIs and reveal them.
		 */
		g_list_free_full (view->details->pending_locations_selected, g_object_unref);
		view->details->pending_locations_selected =
			g_list_copy_deep (selection_locations, (GCopyFunc) g_object_ref, NULL);
	}
}

void
fm_directory_view_init_view_iface (CajaViewIface *iface)
{
	iface->grab_focus = fm_directory_view_grab_focus;
	iface->update_menus = view_iface_update_menus;

	iface->get_widget = fm_directory_view_get_widget;
  	iface->load_location = fm_directory_view_load_location;
	iface->stop_loading = fm_directory_view_stop_loading;

	iface->get_selection_count = fm_directory_view_get_selection_count;
	iface->get_selection = fm_directory_view_get_selection_locations;
	iface->set_selection = fm_directory_view_set_selection_locations;
	iface->set_is_active = (gpointer)fm_directory_view_set_is_active;

	iface->supports_zooming = (gpointer)fm_directory_view_supports_zooming;
	iface->bump_zoom_level = (gpointer)fm_directory_view_bump_zoom_level;
        iface->zoom_to_level = (gpointer)fm_directory_view_zoom_to_level;
        iface->restore_default_zoom_level = (gpointer)fm_directory_view_restore_default_zoom_level;
        iface->can_zoom_in = (gpointer)fm_directory_view_can_zoom_in;
        iface->can_zoom_out = (gpointer)fm_directory_view_can_zoom_out;
	iface->get_zoom_level = (gpointer)fm_directory_view_get_zoom_level;

	iface->pop_up_location_context_menu = (gpointer)fm_directory_view_pop_up_location_context_menu;
	iface->drop_proxy_received_uris = (gpointer)fm_directory_view_drop_proxy_received_uris;
	iface->drop_proxy_received_netscape_url = (gpointer)fm_directory_view_drop_proxy_received_netscape_url;
}

static void
fm_directory_view_init (FMDirectoryView *view)
{
	CajaDirectory *scripts_directory;

	view->details = g_new0 (FMDirectoryViewDetails, 1);

	/* Default to true; desktop-icon-view sets to false */
	view->details->show_foreign_files = TRUE;

	view->details->non_ready_files =
		g_hash_table_new_full (file_and_directory_hash,
				       file_and_directory_equal,
				       (GDestroyNotify)file_and_directory_free,
				       NULL);

	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (view),
					GTK_POLICY_AUTOMATIC,
					GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_hadjustment (GTK_SCROLLED_WINDOW (view), NULL);
	gtk_scrolled_window_set_vadjustment (GTK_SCROLLED_WINDOW (view), NULL);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (view), GTK_SHADOW_ETCHED_IN);
	gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (view), FALSE);

	set_up_scripts_directory_global ();
	scripts_directory = caja_directory_get_by_uri (scripts_directory_uri);
	add_directory_to_scripts_directory_list (view, scripts_directory);
	caja_directory_unref (scripts_directory);

	if (caja_should_use_templates_directory ()) {
		CajaDirectory *templates_directory;
		char *templates_uri;

		templates_uri = caja_get_templates_directory_uri ();
		templates_directory = caja_directory_get_by_uri (templates_uri);
		g_free (templates_uri);
		add_directory_to_templates_directory_list (view, templates_directory);
		caja_directory_unref (templates_directory);
	}
	update_templates_directory (view);
	g_signal_connect_object (caja_signaller_get_current (),
				 "user_dirs_changed",
				 G_CALLBACK (user_dirs_changed),
				 view, G_CONNECT_SWAPPED);

	view->details->sort_directories_first =
		g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_SORT_DIRECTORIES_FIRST);

	g_signal_connect_object (caja_trash_monitor_get (), "trash_state_changed",
				 G_CALLBACK (fm_directory_view_trash_state_changed_callback), view, 0);

	/* React to clipboard changes */
	g_signal_connect_object (caja_clipboard_monitor_get (), "clipboard_changed",
				 G_CALLBACK (clipboard_changed_callback), view, 0);

        /* Register to menu provider extension signal managing menu updates */
        g_signal_connect_object (caja_signaller_get_current (), "popup_menu_changed",
                         G_CALLBACK (fm_directory_view_update_menus), view, G_CONNECT_SWAPPED);

	gtk_widget_show (GTK_WIDGET (view));

	g_signal_connect_swapped (caja_preferences,
							  "changed::" CAJA_PREFERENCES_ENABLE_DELETE,
							  G_CALLBACK (schedule_update_menus_callback), view);
	g_signal_connect_swapped (caja_icon_view_preferences,
							  "changed::" CAJA_PREFERENCES_ICON_VIEW_CAPTIONS,
							  G_CALLBACK(text_attribute_names_changed_callback),
							  view);
	g_signal_connect_swapped (caja_preferences,
							  "changed::" CAJA_PREFERENCES_SHOW_IMAGE_FILE_THUMBNAILS,
							  G_CALLBACK (image_display_policy_changed_callback),
							  view);
	g_signal_connect_swapped (caja_preferences,
							  "changed::" CAJA_PREFERENCES_CLICK_POLICY,
							  G_CALLBACK(click_policy_changed_callback),
							  view);
	g_signal_connect_swapped (caja_preferences,
							  "changed::" CAJA_PREFERENCES_SORT_DIRECTORIES_FIRST,
							  G_CALLBACK(sort_directories_first_changed_callback),
							  view);
	g_signal_connect_swapped (mate_lockdown_preferences,
							  "changed::" CAJA_PREFERENCES_LOCKDOWN_COMMAND_LINE,
							  G_CALLBACK (schedule_update_menus), view);

	/* Update undo actions stuff and connect signals from the undostack manager */
	view->details->undo_active = FALSE;
	view->details->redo_active = FALSE;
	view->details->undo_action_description = NULL;
	view->details->undo_action_label = NULL;
	view->details->redo_action_description = NULL;
	view->details->redo_action_label = NULL;

	CajaUndoStackManager* manager = caja_undostack_manager_instance ();

	g_signal_connect_object (G_OBJECT(manager), "request-menu-update",
		   G_CALLBACK(undo_redo_menu_update_callback), view, 0);

	caja_undostack_manager_request_menu_update (caja_undostack_manager_instance());
}

static void
real_unmerge_menus (FMDirectoryView *view)
{
	GtkUIManager *ui_manager;

	if (view->details->window == NULL) {
		return;
	}

	ui_manager = caja_window_info_get_ui_manager (view->details->window);

	caja_ui_unmerge_ui (ui_manager,
				&view->details->dir_merge_id,
				&view->details->dir_action_group);
	caja_ui_unmerge_ui (ui_manager,
				&view->details->extensions_menu_merge_id,
				&view->details->extensions_menu_action_group);
	caja_ui_unmerge_ui (ui_manager,
				&view->details->open_with_merge_id,
				&view->details->open_with_action_group);
	caja_ui_unmerge_ui (ui_manager,
				&view->details->scripts_merge_id,
				&view->details->scripts_action_group);
	caja_ui_unmerge_ui (ui_manager,
				&view->details->templates_merge_id,
				&view->details->templates_action_group);
}

static void
fm_directory_view_destroy (GtkWidget *object)
{
	FMDirectoryView *view;
	GList *node, *next;

	view = FM_DIRECTORY_VIEW (object);

	disconnect_model_handlers (view);

	fm_directory_view_unmerge_menus (view);

	/* We don't own the window, so no unref */
	view->details->slot = NULL;
	view->details->window = NULL;

	fm_directory_view_stop (view);
	fm_directory_view_clear (view);

	for (node = view->details->scripts_directory_list; node != NULL; node = next) {
		next = node->next;
		remove_directory_from_scripts_directory_list (view, node->data);
	}

	for (node = view->details->templates_directory_list; node != NULL; node = next) {
		next = node->next;
		remove_directory_from_templates_directory_list (view, node->data);
	}

	while (view->details->subdirectory_list != NULL) {
		fm_directory_view_remove_subdirectory (view,
				view->details->subdirectory_list->data);
	}

	remove_update_menus_timeout_callback (view);
	remove_update_status_idle_callback (view);

	if (view->details->display_selection_idle_id != 0) {
		g_source_remove (view->details->display_selection_idle_id);
		view->details->display_selection_idle_id = 0;
	}

	if (view->details->reveal_selection_idle_id != 0) {
		g_source_remove (view->details->reveal_selection_idle_id);
		view->details->reveal_selection_idle_id = 0;
	}

	if (view->details->delayed_rename_file_id != 0) {
		g_source_remove (view->details->delayed_rename_file_id);
		view->details->delayed_rename_file_id = 0;
	}

	if (view->details->model) {
		caja_directory_unref (view->details->model);
		view->details->model = NULL;
	}

	if (view->details->directory_as_file) {
		caja_file_unref (view->details->directory_as_file);
		view->details->directory_as_file = NULL;
	}

	EEL_CALL_PARENT (GTK_WIDGET_CLASS, destroy, (object));
}

static void
fm_directory_view_finalize (GObject *object)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (object);

	g_signal_handlers_disconnect_by_func (caja_preferences,
        				      schedule_update_menus_callback, view);
	g_signal_handlers_disconnect_by_func (caja_icon_view_preferences,
        				      text_attribute_names_changed_callback, view);
	g_signal_handlers_disconnect_by_func (caja_preferences,
        				      image_display_policy_changed_callback, view);
	g_signal_handlers_disconnect_by_func (caja_preferences,
        				      click_policy_changed_callback, view);
	g_signal_handlers_disconnect_by_func (caja_preferences,
        				      sort_directories_first_changed_callback, view);
	g_signal_handlers_disconnect_by_func (mate_lockdown_preferences,
        				      schedule_update_menus, view);

	unschedule_pop_up_location_context_menu (view);
	if (view->details->location_popup_event != NULL) {
		gdk_event_free ((GdkEvent *) view->details->location_popup_event);
	}

	g_hash_table_destroy (view->details->non_ready_files);

	g_free (view->details);

	EEL_CALL_PARENT (G_OBJECT_CLASS, finalize, (object));
}

/**
 * fm_directory_view_display_selection_info:
 *
 * Display information about the current selection, and notify the view frame of the changed selection.
 * @view: FMDirectoryView for which to display selection info.
 *
 **/
void
fm_directory_view_display_selection_info (FMDirectoryView *view)
{
	GList *selection;
	goffset non_folder_size;
	gboolean non_folder_size_known;
	guint non_folder_count, folder_count, folder_item_count;
	gboolean folder_item_count_known;
	guint file_item_count;
	GList *p;
	char *first_item_name;
	char *non_folder_str;
	char *folder_count_str;
	char *folder_item_count_str;
	char *status_string;
	char *free_space_str;
	char *obj_selected_free_space_str;
	CajaFile *file = NULL;

	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	selection = fm_directory_view_get_selection (view);

	folder_item_count_known = TRUE;
	folder_count = 0;
	folder_item_count = 0;
	non_folder_count = 0;
	non_folder_size_known = FALSE;
	non_folder_size = 0;
	first_item_name = NULL;
	folder_count_str = NULL;
	non_folder_str = NULL;
	folder_item_count_str = NULL;
	free_space_str = NULL;
	obj_selected_free_space_str = NULL;

	for (p = selection; p != NULL; p = p->next) {
		file = p->data;
		if (caja_file_is_directory (file)) {
			folder_count++;
			if (caja_file_get_directory_item_count (file, &file_item_count, NULL)) {
				folder_item_count += file_item_count;
			} else {
				folder_item_count_known = FALSE;
			}
		} else {
			non_folder_count++;
			if (!caja_file_can_get_size (file)) {
				non_folder_size_known = TRUE;
				non_folder_size += caja_file_get_size (file);
			}
		}

		if (first_item_name == NULL) {
			first_item_name = caja_file_get_display_name (file);
		}
	}

	caja_file_list_free (selection);

	/* Break out cases for localization's sake. But note that there are still pieces
	 * being assembled in a particular order, which may be a problem for some localizers.
	 */

	if (folder_count != 0) {
		if (folder_count == 1 && non_folder_count == 0) {
			folder_count_str = g_strdup_printf (_("\"%s\" selected"), first_item_name);
		} else {
			folder_count_str = g_strdup_printf (ngettext("%'d folder selected",
								     "%'d folders selected",
								     folder_count),
							    folder_count);
		}

		if (folder_count == 1) {
			if (!folder_item_count_known) {
				folder_item_count_str = g_strdup ("");
			} else {
				folder_item_count_str = g_strdup_printf (ngettext(" (containing %'d item)",
										  " (containing %'d items)",
										  folder_item_count),
									 folder_item_count);
			}
		}
		else {
			if (!folder_item_count_known) {
				folder_item_count_str = g_strdup ("");
			} else {
				/* Translators: this is preceded with a string of form 'N folders' (N more than 1) */
				folder_item_count_str = g_strdup_printf (ngettext(" (containing a total of %'d item)",
										  " (containing a total of %'d items)",
										  folder_item_count),
									 folder_item_count);
			}

		}
	}

	if (non_folder_count != 0) {
		char *items_string;

		if (folder_count == 0) {
			if (non_folder_count == 1) {
				items_string = g_strdup_printf (_("\"%s\" selected"),
								  first_item_name);
			} else {
				items_string = g_strdup_printf (ngettext("%'d item selected",
									   "%'d items selected",
									   non_folder_count),
								  non_folder_count);
			}
		} else {
			/* Folders selected also, use "other" terminology */
			items_string = g_strdup_printf (ngettext("%'d other item selected",
								   "%'d other items selected",
								   non_folder_count),
							  non_folder_count);
		}

		if (non_folder_size_known) {
			char *size_string;

			if (g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_USE_IEC_UNITS))
				size_string = g_format_size_full (non_folder_size, G_FORMAT_SIZE_IEC_UNITS);
			else
				size_string = g_format_size(non_folder_size);

			/* Translators: This is marked for translation in case a localiser
			 * needs to use something other than parentheses. The
			 * first message gives the number of items selected;
			 * the message in parentheses the size of those items.
			 */
			non_folder_str = g_strdup_printf (_("%s (%s)"),
							  items_string,
							  size_string);

			g_free (size_string);
			g_free (items_string);
		} else {
			non_folder_str = items_string;
		}
	}

	free_space_str = caja_file_get_volume_free_space (view->details->directory_as_file);
	if (free_space_str != NULL) {
		obj_selected_free_space_str = g_strdup_printf (_("Free space: %s"), free_space_str);
	}
	if (folder_count == 0 && non_folder_count == 0)	{
		char *item_count_str;
		guint item_count;

		item_count = fm_directory_view_get_item_count (view);

		item_count_str = g_strdup_printf (ngettext ("%'u item", "%'u items", item_count), item_count);

		if (free_space_str != NULL) {
			status_string = g_strdup_printf (_("%s, Free space: %s"), item_count_str, free_space_str);
			g_free (item_count_str);
		} else {
			status_string = item_count_str;
		}

	} else if (folder_count == 0) {
		if (free_space_str == NULL) {
			status_string = g_strdup (non_folder_str);
		} else {
			/* Marking this for translation, since you
			 * might want to change "," to something else.
			 * After the comma the amount of free space will
			 * be shown.
			 */
			status_string = g_strdup_printf (_("%s, %s"),
							 non_folder_str,
							 obj_selected_free_space_str);
		}
	} else if (non_folder_count == 0) {
		if (free_space_str == NULL) {
		        /* No use marking this for translation, since you
		         * can't reorder the strings, which is the main thing
		         * you'd want to do.
		         */
			status_string = g_strdup_printf ("%s%s",
							 folder_count_str,
							 folder_item_count_str);
		} else {
			/* Marking this for translation, since you
			 * might want to change "," to something else.
			 * After the comma the amount of free space will
			 * be shown.
			 */
			status_string = g_strdup_printf (_("%s%s, %s"),
							 folder_count_str,
							 folder_item_count_str,
							 obj_selected_free_space_str);
		}
	} else {
		if (obj_selected_free_space_str == NULL) {
			/* This is marked for translation in case a localizer
			 * needs to change ", " to something else. The comma
			 * is between the message about the number of folders
			 * and the number of items in those folders and the
			 * message about the number of other items and the
			 * total size of those items.
			 */
			status_string = g_strdup_printf (_("%s%s, %s"),
							 folder_count_str,
							 folder_item_count_str,
							 non_folder_str);
		} else {
			/* This is marked for translation in case a localizer
			 * needs to change ", " to something else. The first comma
			 * is between the message about the number of folders
			 * and the number of items in those folders and the
			 * message about the number of other items and the
			 * total size of those items. After the second comma
			 * the free space is written.
			 */
			status_string = g_strdup_printf (_("%s%s, %s, %s"),
							 folder_count_str,
							 folder_item_count_str,
							 non_folder_str,
							 obj_selected_free_space_str);
		}
	}

	g_free (free_space_str);
	g_free (obj_selected_free_space_str);
	g_free (first_item_name);
	g_free (folder_count_str);
	g_free (folder_item_count_str);
	g_free (non_folder_str);

	caja_window_slot_info_set_status (view->details->slot,
					      status_string);
	g_free (status_string);
}

void
fm_directory_view_send_selection_change (FMDirectoryView *view)
{
	caja_window_info_report_selection_changed (view->details->window);

	view->details->send_selection_change_to_shell = FALSE;
}

gboolean
fm_directory_view_get_allow_moves (FMDirectoryView *view)
{
	return view->details->allow_moves;
}

static void
fm_directory_view_load_location (CajaView *caja_view,
				 const char *location)
{
	CajaDirectory *directory;
	FMDirectoryView *directory_view;

	directory_view = FM_DIRECTORY_VIEW (caja_view);

	if (eel_uri_is_search (location)) {
		directory_view->details->allow_moves = FALSE;
	} else {
		directory_view->details->allow_moves = TRUE;
	}

	directory = caja_directory_get_by_uri (location);
	load_directory (directory_view, directory);
	caja_directory_unref (directory);
}

static void
fm_directory_view_stop_loading (CajaView *caja_view)
{
	fm_directory_view_stop (FM_DIRECTORY_VIEW (caja_view));
}

static gboolean
reveal_selection_idle_callback (gpointer data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (data);

	view->details->reveal_selection_idle_id = 0;
	fm_directory_view_reveal_selection (view);

	return FALSE;
}

static void
done_loading (FMDirectoryView *view,
	      gboolean all_files_seen)
{
	if (!view->details->loading) {
		return;
	}

	/* This can be called during destruction, in which case there
	 * is no CajaWindowInfo any more.
	 */
	if (view->details->window != NULL) {
		GList *locations_selected;

		if (all_files_seen) {
			caja_window_info_report_load_complete (view->details->window, CAJA_VIEW (view));
		}

		schedule_update_menus (view);
		schedule_update_status (view);
		reset_update_interval (view);

		locations_selected = view->details->pending_locations_selected;

		if (locations_selected != NULL && all_files_seen) {
			GList *selection;

			view->details->pending_locations_selected = NULL;

			selection = file_list_from_location_list (locations_selected);

			view->details->selection_change_is_due_to_shell = TRUE;
			fm_directory_view_set_selection (view, selection);
			view->details->selection_change_is_due_to_shell = FALSE;
			caja_file_list_free (selection);

			if (FM_IS_LIST_VIEW (view)) {
				/* HACK: We should be able to directly call reveal_selection here,
				 * but at this point the GtkTreeView hasn't allocated the new nodes
				 * yet, and it has a bug in the scroll calculation dealing with this
				 * special case. It would always make the selection the top row, even
				 * if no scrolling would be neccessary to reveal it. So we let it
				 * allocate before revealing.
				 */
				if (view->details->reveal_selection_idle_id != 0) {
					g_source_remove (view->details->reveal_selection_idle_id);
				}
				view->details->reveal_selection_idle_id =
					g_idle_add (reveal_selection_idle_callback, view);
			} else {
				fm_directory_view_reveal_selection (view);
			}
		}
		g_list_free_full (locations_selected, g_object_unref);
		fm_directory_view_display_selection_info (view);
	}

	fm_directory_view_end_loading (view, all_files_seen);

	view->details->loading = FALSE;
}

typedef struct {
	GHashTable *debuting_files;
	GList	   *added_files;
} DebutingFilesData;

static void
debuting_files_data_free (DebutingFilesData *data)
{
	g_hash_table_unref (data->debuting_files);
	caja_file_list_free (data->added_files);
	g_free (data);
}

/* This signal handler watch for the arrival of the icons created
 * as the result of a file operation. Once the last one is detected
 * it selects and reveals them all.
 */
static void
debuting_files_add_file_callback (FMDirectoryView *view,
				  CajaFile *new_file,
				  CajaDirectory *directory,
				  DebutingFilesData *data)
{
	GFile *location;

	location = caja_file_get_location (new_file);

	if (g_hash_table_remove (data->debuting_files, location)) {
		caja_file_ref (new_file);
		data->added_files = g_list_prepend (data->added_files, new_file);

		if (g_hash_table_size (data->debuting_files) == 0) {
			fm_directory_view_set_selection (view, data->added_files);
			fm_directory_view_reveal_selection (view);
			g_signal_handlers_disconnect_by_func (view,
							      G_CALLBACK (debuting_files_add_file_callback),
							      data);
		}
	}

	g_object_unref (location);
}

typedef struct {
	GList		*added_files;
	FMDirectoryView *directory_view;
} CopyMoveDoneData;

static void
copy_move_done_data_free (CopyMoveDoneData *data)
{
	g_assert (data != NULL);

	eel_remove_weak_pointer (&data->directory_view);
	caja_file_list_free (data->added_files);
	g_free (data);
}

static void
pre_copy_move_add_file_callback (FMDirectoryView *view,
				 CajaFile *new_file,
				 CajaDirectory *directory,
				 CopyMoveDoneData *data)
{
	caja_file_ref (new_file);
	data->added_files = g_list_prepend (data->added_files, new_file);
}

/* This needs to be called prior to caja_file_operations_copy_move.
 * It hooks up a signal handler to catch any icons that get added before
 * the copy_done_callback is invoked. The return value should  be passed
 * as the data for uri_copy_move_done_callback.
 */
static CopyMoveDoneData *
pre_copy_move (FMDirectoryView *directory_view)
{
	CopyMoveDoneData *copy_move_done_data;

	copy_move_done_data = g_new0 (CopyMoveDoneData, 1);
	copy_move_done_data->directory_view = directory_view;

	eel_add_weak_pointer (&copy_move_done_data->directory_view);

	/* We need to run after the default handler adds the folder we want to
	 * operate on. The ADD_FILE signal is registered as G_SIGNAL_RUN_LAST, so we
	 * must use connect_after.
	 */
	g_signal_connect (directory_view, "add_file",
			  G_CALLBACK (pre_copy_move_add_file_callback), copy_move_done_data);

	return copy_move_done_data;
}

/* This function is used to pull out any debuting uris that were added
 * and (as a side effect) remove them from the debuting uri hash table.
 */
static gboolean
copy_move_done_partition_func (gpointer data, gpointer callback_data)
{
 	GFile *location;
 	gboolean result;

	location = caja_file_get_location (CAJA_FILE (data));
	result = g_hash_table_remove ((GHashTable *) callback_data, location);
	g_object_unref (location);

	return result;
}

static gboolean
remove_not_really_moved_files (gpointer key,
			       gpointer value,
			       gpointer callback_data)
{
	GList **added_files;
	GFile *loc;

	loc = key;

	if (GPOINTER_TO_INT (value)) {
		return FALSE;
	}

	added_files = callback_data;
	*added_files = g_list_prepend (*added_files,
				       caja_file_get (loc));
	return TRUE;
}

/* When this function is invoked, the file operation is over, but all
 * the icons may not have been added to the directory view yet, so
 * we can't select them yet.
 *
 * We're passed a hash table of the uri's to look out for, we hook
 * up a signal handler to await their arrival.
 */
static void
copy_move_done_callback (GHashTable *debuting_files, gpointer data)
{
	FMDirectoryView  *directory_view;
	CopyMoveDoneData *copy_move_done_data;
	DebutingFilesData  *debuting_files_data;

	copy_move_done_data = (CopyMoveDoneData *) data;
	directory_view = copy_move_done_data->directory_view;

	if (directory_view != NULL) {
		g_assert (FM_IS_DIRECTORY_VIEW (directory_view));

		debuting_files_data = g_new (DebutingFilesData, 1);
		debuting_files_data->debuting_files = g_hash_table_ref (debuting_files);
		debuting_files_data->added_files = eel_g_list_partition
			(copy_move_done_data->added_files,
			 copy_move_done_partition_func,
			 debuting_files,
			 &copy_move_done_data->added_files);

		/* We're passed the same data used by pre_copy_move_add_file_callback, so disconnecting
		 * it will free data. We've already siphoned off the added_files we need, and stashed the
		 * directory_view pointer.
		 */
		g_signal_handlers_disconnect_by_func (directory_view,
						      G_CALLBACK (pre_copy_move_add_file_callback),
						      data);

		/* Any items in the debuting_files hash table that have
		 * "FALSE" as their value aren't really being copied
		 * or moved, so we can't wait for an add_file signal
		 * to come in for those.
		 */
		g_hash_table_foreach_remove (debuting_files,
					     remove_not_really_moved_files,
					     &debuting_files_data->added_files);

		if (g_hash_table_size (debuting_files) == 0) {
			/* on the off-chance that all the icons have already been added */
			if (debuting_files_data->added_files != NULL) {
				fm_directory_view_set_selection (directory_view,
								 debuting_files_data->added_files);
				fm_directory_view_reveal_selection (directory_view);
			}
			debuting_files_data_free (debuting_files_data);
		} else {
			/* We need to run after the default handler adds the folder we want to
			 * operate on. The ADD_FILE signal is registered as G_SIGNAL_RUN_LAST, so we
			 * must use connect_after.
			 */
			g_signal_connect_data (directory_view,
					       "add_file",
					       G_CALLBACK (debuting_files_add_file_callback),
					       debuting_files_data,
					       (GClosureNotify) debuting_files_data_free,
					       G_CONNECT_AFTER);
		}

		/* Schedule menu update for undo items */
		schedule_update_menus (directory_view);

	}

	copy_move_done_data_free (copy_move_done_data);
}

static gboolean
real_file_still_belongs (FMDirectoryView *view, CajaFile *file, CajaDirectory *directory)
{
	if (view->details->model != directory &&
	    g_list_find (view->details->subdirectory_list, directory) == NULL) {
		return FALSE;
	}

	return caja_directory_contains_file (directory, file);
}

static gboolean
still_should_show_file (FMDirectoryView *view, CajaFile *file, CajaDirectory *directory)
{
	return fm_directory_view_should_show_file (view, file)
		&& EEL_INVOKE_METHOD (FM_DIRECTORY_VIEW_CLASS, view, file_still_belongs, (view, file, directory));
}

static gboolean
ready_to_load (CajaFile *file)
{
	return caja_file_check_if_ready (file,
					     CAJA_FILE_ATTRIBUTES_FOR_ICON);
}

static int
compare_files_cover (gconstpointer a, gconstpointer b, gpointer callback_data)
{
	const FileAndDirectory *fad1, *fad2;
	FMDirectoryView *view;

	view = callback_data;
	fad1 = a; fad2 = b;

	if (fad1->directory < fad2->directory) {
		return -1;
	} else if (fad1->directory > fad2->directory) {
		return 1;
	} else {
		return EEL_INVOKE_METHOD (FM_DIRECTORY_VIEW_CLASS, view, compare_files,
					  (view, fad1->file, fad2->file));
	}
}
static void
sort_files (FMDirectoryView *view, GList **list)
{
	*list = g_list_sort_with_data (*list, compare_files_cover, view);

}

/* Go through all the new added and changed files.
 * Put any that are not ready to load in the non_ready_files hash table.
 * Add all the rest to the old_added_files and old_changed_files lists.
 * Sort the old_*_files lists if anything was added to them.
 */
static void
process_new_files (FMDirectoryView *view)
{
	GList *new_added_files, *new_changed_files, *old_added_files, *old_changed_files;
	GHashTable *non_ready_files;
	GList *node, *next;
	FileAndDirectory *pending;
	gboolean in_non_ready;

	new_added_files = view->details->new_added_files;
	view->details->new_added_files = NULL;
	new_changed_files = view->details->new_changed_files;
	view->details->new_changed_files = NULL;

	non_ready_files = view->details->non_ready_files;

	old_added_files = view->details->old_added_files;
	old_changed_files = view->details->old_changed_files;

	/* Newly added files go into the old_added_files list if they're
	 * ready, and into the hash table if they're not.
	 */
	for (node = new_added_files; node != NULL; node = next) {
		next = node->next;
		pending = (FileAndDirectory *)node->data;
		in_non_ready = g_hash_table_lookup (non_ready_files, pending) != NULL;
		if (fm_directory_view_should_show_file (view, pending->file)) {
			if (ready_to_load (pending->file)) {
				if (in_non_ready) {
					g_hash_table_remove (non_ready_files, pending);
				}
				new_added_files = g_list_delete_link (new_added_files, node);
				old_added_files = g_list_prepend (old_added_files, pending);
			} else {
				if (!in_non_ready) {
					new_added_files = g_list_delete_link (new_added_files, node);
					g_hash_table_insert (non_ready_files, pending, pending);
				}
			}
		}
	}
	file_and_directory_list_free (new_added_files);

	/* Newly changed files go into the old_added_files list if they're ready
	 * and were seen non-ready in the past, into the old_changed_files list
	 * if they are read and were not seen non-ready in the past, and into
	 * the hash table if they're not ready.
	 */
	for (node = new_changed_files; node != NULL; node = next) {
		next = node->next;
		pending = (FileAndDirectory *)node->data;
		if (!still_should_show_file (view, pending->file, pending->directory) || ready_to_load (pending->file)) {
			if (g_hash_table_lookup (non_ready_files, pending) != NULL) {
				g_hash_table_remove (non_ready_files, pending);
				if (still_should_show_file (view, pending->file, pending->directory)) {
					new_changed_files = g_list_delete_link (new_changed_files, node);
					old_added_files = g_list_prepend (old_added_files, pending);
				}
			} else if (fm_directory_view_should_show_file (view, pending->file)) {
				new_changed_files = g_list_delete_link (new_changed_files, node);
				old_changed_files = g_list_prepend (old_changed_files, pending);
			}
		}
	}
	file_and_directory_list_free (new_changed_files);

	/* If any files were added to old_added_files, then resort it. */
	if (old_added_files != view->details->old_added_files) {
		view->details->old_added_files = old_added_files;
		sort_files (view, &view->details->old_added_files);
	}

	/* Resort old_changed_files too, since file attributes
	 * relevant to sorting could have changed.
	 */
	if (old_changed_files != view->details->old_changed_files) {
		view->details->old_changed_files = old_changed_files;
		sort_files (view, &view->details->old_changed_files);
	}

}

static void
process_old_files (FMDirectoryView *view)
{
	GList *files_added, *files_changed, *node;
	GList *selection, *files;
	gboolean send_selection_change;

	files_added = view->details->old_added_files;
	files_changed = view->details->old_changed_files;

	send_selection_change = FALSE;

	if (files_added != NULL || files_changed != NULL) {
		FileAndDirectory *pending = NULL;

		g_signal_emit (view, signals[BEGIN_FILE_CHANGES], 0);

		for (node = files_added; node != NULL; node = node->next) {
			pending = node->data;
			g_signal_emit (view,
				       signals[ADD_FILE], 0, pending->file, pending->directory);
		}

		for (node = files_changed; node != NULL; node = node->next) {
			pending = node->data;
			g_signal_emit (view,
				       signals[still_should_show_file (view, pending->file, pending->directory)
					       ? FILE_CHANGED : REMOVE_FILE], 0,
				       pending->file, pending->directory);
		}

		g_signal_emit (view, signals[END_FILE_CHANGES], 0);

		if (files_changed != NULL) {
			selection = fm_directory_view_get_selection (view);
			files = file_and_directory_list_to_files (files_changed);
			send_selection_change = eel_g_lists_sort_and_check_for_intersection
				(&files, &selection);
			caja_file_list_free (files);
			caja_file_list_free (selection);
		}

		file_and_directory_list_free (view->details->old_added_files);
		view->details->old_added_files = NULL;

		file_and_directory_list_free (view->details->old_changed_files);
		view->details->old_changed_files = NULL;
	}

	if (send_selection_change) {
		/* Send a selection change since some file names could
		 * have changed.
		 */
		fm_directory_view_send_selection_change (view);
	}
}

static void
display_pending_files (FMDirectoryView *view)
{

	/* Don't dispatch any updates while the view is frozen. */
	if (view->details->updates_frozen) {
		return;
	}

	process_new_files (view);
	process_old_files (view);

	if (view->details->model != NULL
	    && caja_directory_are_all_files_seen (view->details->model)
	    && g_hash_table_size (view->details->non_ready_files) == 0) {
		done_loading (view, TRUE);
	}
}

void
fm_directory_view_freeze_updates (FMDirectoryView *view)
{
	view->details->updates_frozen = TRUE;
	view->details->updates_queued = 0;
	view->details->needs_reload = FALSE;
}

void
fm_directory_view_unfreeze_updates (FMDirectoryView *view)
{
	view->details->updates_frozen = FALSE;

	if (view->details->needs_reload) {
		view->details->needs_reload = FALSE;
		if (view->details->model != NULL) {
			load_directory (view, view->details->model);
		}
	} else {
		schedule_idle_display_of_pending_files (view);
	}
}

static gboolean
display_selection_info_idle_callback (gpointer data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (data);

	g_object_ref (G_OBJECT (view));

	view->details->display_selection_idle_id = 0;
	fm_directory_view_display_selection_info (view);
	if (view->details->send_selection_change_to_shell) {
		fm_directory_view_send_selection_change (view);
	}

	g_object_unref (G_OBJECT (view));

	return FALSE;
}

static void
remove_update_menus_timeout_callback (FMDirectoryView *view)
{
	if (view->details->update_menus_timeout_id != 0) {
		g_source_remove (view->details->update_menus_timeout_id);
		view->details->update_menus_timeout_id = 0;
	}
}

static void
update_menus_if_pending (FMDirectoryView *view)
{
	if (!view->details->menu_states_untrustworthy) {
		return;
	}

	remove_update_menus_timeout_callback (view);
	fm_directory_view_update_menus (view);
}

static gboolean
update_menus_timeout_callback (gpointer data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (data);

	g_object_ref (G_OBJECT (view));

	view->details->update_menus_timeout_id = 0;
	fm_directory_view_update_menus (view);

	g_object_unref (G_OBJECT (view));

	return FALSE;
}

static gboolean
display_pending_callback (gpointer data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (data);

	g_object_ref (G_OBJECT (view));

	view->details->display_pending_source_id = 0;

	display_pending_files (view);

	g_object_unref (G_OBJECT (view));

	return FALSE;
}

static void
schedule_idle_display_of_pending_files (FMDirectoryView *view)
{
	/* Get rid of a pending source as it might be a timeout */
	unschedule_display_of_pending_files (view);

	/* We want higher priority than the idle that handles the relayout
	   to avoid a resort on each add. But we still want to allow repaints
	   and other hight prio events while we have pending files to show. */
	view->details->display_pending_source_id =
		g_idle_add_full (G_PRIORITY_DEFAULT_IDLE - 20,
				 display_pending_callback, view, NULL);
}

static void
schedule_timeout_display_of_pending_files (FMDirectoryView *view, guint interval)
{
 	/* No need to schedule an update if there's already one pending. */
	if (view->details->display_pending_source_id != 0) {
 		return;
	}

	view->details->display_pending_source_id =
		g_timeout_add (interval, display_pending_callback, view);
}

static void
unschedule_display_of_pending_files (FMDirectoryView *view)
{
	/* Get rid of source if it's active. */
	if (view->details->display_pending_source_id != 0) {
		g_source_remove (view->details->display_pending_source_id);
		view->details->display_pending_source_id = 0;
	}
}

static void
queue_pending_files (FMDirectoryView *view,
		     CajaDirectory *directory,
		     GList *files,
		     GList **pending_list)
{
	if (files == NULL) {
		return;
	}

	/* Don't queue any more updates if we need to reload anyway */
	if (view->details->needs_reload) {
		return;
	}

	if (view->details->updates_frozen) {
		view->details->updates_queued += g_list_length (files);
		/* Mark the directory for reload when there are too much queued
		 * changes to prevent the pending list from growing infinitely.
		 */
		if (view->details->updates_queued > MAX_QUEUED_UPDATES) {
			view->details->needs_reload = TRUE;
			return;
		}
	}

	*pending_list = g_list_concat (file_and_directory_list_from_files (directory, files),
				       *pending_list);

	if (! view->details->loading || caja_directory_are_all_files_seen (directory)) {
		schedule_timeout_display_of_pending_files (view, view->details->update_interval);
	}
}

static void
remove_changes_timeout_callback (FMDirectoryView *view)
{
	if (view->details->changes_timeout_id != 0) {
		g_source_remove (view->details->changes_timeout_id);
		view->details->changes_timeout_id = 0;
	}
}

static void
reset_update_interval (FMDirectoryView *view)
{
	view->details->update_interval = UPDATE_INTERVAL_MIN;
	remove_changes_timeout_callback (view);
	/* Reschedule a pending timeout to idle */
	if (view->details->display_pending_source_id != 0) {
		schedule_idle_display_of_pending_files (view);
	}
}

static gboolean
changes_timeout_callback (gpointer data)
{
	gint64 now;
	gint64 time_delta;
	gboolean ret;
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (data);

	g_object_ref (G_OBJECT (view));

	now = g_get_monotonic_time();
	time_delta = now - view->details->last_queued;

	if (time_delta < UPDATE_INTERVAL_RESET*1000) {
		if (view->details->update_interval < UPDATE_INTERVAL_MAX &&
			view->details->loading) {
			/* Increase */
			view->details->update_interval += UPDATE_INTERVAL_INC;
		}
		ret = TRUE;
	} else {
		/* Reset */
		reset_update_interval (view);
		ret = FALSE;
	}

	g_object_unref (G_OBJECT (view));

	return ret;
}

static void
schedule_changes (FMDirectoryView *view)
{
	/* Remember when the change was queued */
	view->details->last_queued = g_get_monotonic_time();

	/* No need to schedule if there are already changes pending or during loading */
	if (view->details->changes_timeout_id != 0 ||
		view->details->loading) {
		return;
	}

	view->details->changes_timeout_id =
		g_timeout_add (UPDATE_INTERVAL_TIMEOUT_INTERVAL, changes_timeout_callback, view);
}

static void
action_undo_callback (GtkAction *action,
			gpointer callback_data)
{
	real_action_undo (FM_DIRECTORY_VIEW (callback_data));
}

static void
action_redo_callback (GtkAction *action,
			gpointer callback_data)
{
	real_action_redo (FM_DIRECTORY_VIEW (callback_data));
}

static void
files_added_callback (CajaDirectory *directory,
		      GList *files,
		      gpointer callback_data)
{
	FMDirectoryView *view;
	GtkWindow *window;
	char *uri;

	view = FM_DIRECTORY_VIEW (callback_data);

	window = fm_directory_view_get_containing_window (view);
	uri = fm_directory_view_get_uri (view);
	caja_debug_log_with_file_list (FALSE, CAJA_DEBUG_LOG_DOMAIN_ASYNC, files,
					   "files added in window %p: %s",
					   window,
					   uri ? uri : "(no directory)");
	g_free (uri);

	schedule_changes (view);

	queue_pending_files (view, directory, files, &view->details->new_added_files);

	/* The number of items could have changed */
	schedule_update_status (view);
}

static void
files_changed_callback (CajaDirectory *directory,
			GList *files,
			gpointer callback_data)
{
	FMDirectoryView *view;
	GtkWindow *window;
	char *uri;

	view = FM_DIRECTORY_VIEW (callback_data);

	window = fm_directory_view_get_containing_window (view);
	uri = fm_directory_view_get_uri (view);
	caja_debug_log_with_file_list (FALSE, CAJA_DEBUG_LOG_DOMAIN_ASYNC, files,
					   "files changed in window %p: %s",
					   window,
					   uri ? uri : "(no directory)");
	g_free (uri);

	schedule_changes (view);

	queue_pending_files (view, directory, files, &view->details->new_changed_files);

	/* The free space or the number of items could have changed */
	schedule_update_status (view);

	/* A change in MIME type could affect the Open with menu, for
	 * one thing, so we need to update menus when files change.
	 */
	schedule_update_menus (view);
}

static void
done_loading_callback (CajaDirectory *directory,
		       gpointer callback_data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (callback_data);

	process_new_files (view);
	if (g_hash_table_size (view->details->non_ready_files) == 0) {
		/* Unschedule a pending update and schedule a new one with the minimal
		 * update interval. This gives the view a short chance at gathering the
		 * (cached) deep counts.
		 */
		unschedule_display_of_pending_files (view);
		schedule_timeout_display_of_pending_files (view, UPDATE_INTERVAL_MIN);
	}
}

static void
load_error_callback (CajaDirectory *directory,
		     GError *error,
		     gpointer callback_data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (callback_data);

	/* FIXME: By doing a stop, we discard some pending files. Is
	 * that OK?
	 */
	fm_directory_view_stop (view);

	/* Emit a signal to tell subclasses that a load error has
	 * occurred, so they can handle it in the UI.
	 */
	g_signal_emit (view,
		       signals[LOAD_ERROR], 0, error);
}

static void
real_load_error (FMDirectoryView *view, GError *error)
{
	/* Report only one error per failed directory load (from the UI
	 * point of view, not from the CajaDirectory point of view).
	 * Otherwise you can get multiple identical errors caused by
	 * unrelated code that just happens to try to iterate this
	 * directory.
	 */
	if (!view->details->reported_load_error) {
		fm_report_error_loading_directory
			(fm_directory_view_get_directory_as_file (view),
			 error,
			 fm_directory_view_get_containing_window (view));
	}
	view->details->reported_load_error = TRUE;
}

void
fm_directory_view_add_subdirectory (FMDirectoryView  *view,
				    CajaDirectory*directory)
{
	CajaFileAttributes attributes;

	g_assert (!g_list_find (view->details->subdirectory_list, directory));

	caja_directory_ref (directory);

	attributes =
		CAJA_FILE_ATTRIBUTES_FOR_ICON |
		CAJA_FILE_ATTRIBUTE_DIRECTORY_ITEM_COUNT |
		CAJA_FILE_ATTRIBUTE_INFO |
		CAJA_FILE_ATTRIBUTE_LINK_INFO |
		CAJA_FILE_ATTRIBUTE_MOUNT |
		CAJA_FILE_ATTRIBUTE_EXTENSION_INFO;

	caja_directory_file_monitor_add (directory,
					     &view->details->model,
					     view->details->show_hidden_files,
					     attributes,
					     files_added_callback, view);

	g_signal_connect
		(directory, "files_added",
		 G_CALLBACK (files_added_callback), view);
	g_signal_connect
		(directory, "files_changed",
		 G_CALLBACK (files_changed_callback), view);

	view->details->subdirectory_list = g_list_prepend (
			   view->details->subdirectory_list, directory);
}

void
fm_directory_view_remove_subdirectory (FMDirectoryView  *view,
				       CajaDirectory*directory)
{
	g_assert (g_list_find (view->details->subdirectory_list, directory));

	view->details->subdirectory_list = g_list_remove (
				view->details->subdirectory_list, directory);

	g_signal_handlers_disconnect_by_func (directory,
					      G_CALLBACK (files_added_callback),
					      view);
	g_signal_handlers_disconnect_by_func (directory,
					      G_CALLBACK (files_changed_callback),
					      view);

	caja_directory_file_monitor_remove (directory, &view->details->model);

	caja_directory_unref (directory);
}

/**
 * fm_directory_view_clear:
 *
 * Emit the signal to clear the contents of the view. Subclasses must
 * override the signal handler for this signal. This is normally called
 * only by FMDirectoryView.
 * @view: FMDirectoryView to empty.
 *
 **/
void
fm_directory_view_clear (FMDirectoryView *view)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	g_signal_emit (view, signals[CLEAR], 0);
}

/**
 * fm_directory_view_begin_loading:
 *
 * Emit the signal to prepare for loading the contents of a new location.
 * Subclasses might want to override the signal handler for this signal.
 * This is normally called only by FMDirectoryView.
 * @view: FMDirectoryView that is switching to view a new location.
 *
 **/
void
fm_directory_view_begin_loading (FMDirectoryView *view)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	g_signal_emit (view, signals[BEGIN_LOADING], 0);
}

/**
 * fm_directory_view_end_loading:
 *
 * Emit the signal after loading the contents of a new location.
 * Subclasses might want to override the signal handler for this signal.
 * This is normally called only by FMDirectoryView.
 * @view: FMDirectoryView that is switching to view a new location.
 *
 **/
void
fm_directory_view_end_loading (FMDirectoryView *view,
			       gboolean all_files_seen)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	g_signal_emit (view, signals[END_LOADING], 0, all_files_seen);
}

/**
 * fm_directory_view_get_loading:
 * @view: an #FMDirectoryView.
 *
 * Return value: #gboolean inicating whether @view is currently loaded.
 *
 **/
gboolean
fm_directory_view_get_loading (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	return view->details->loading;
}

/**
 * fm_directory_view_bump_zoom_level:
 *
 * bump the current zoom level by invoking the relevant subclass through the slot
 *
 **/
void
fm_directory_view_bump_zoom_level (FMDirectoryView *view, int zoom_increment)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	if (!fm_directory_view_supports_zooming (view)) {
		return;
	}

	EEL_CALL_METHOD
		(FM_DIRECTORY_VIEW_CLASS, view,
		 bump_zoom_level, (view, zoom_increment));
}

/**
 * fm_directory_view_zoom_to_level:
 *
 * Set the current zoom level by invoking the relevant subclass through the slot
 *
 **/
void
fm_directory_view_zoom_to_level (FMDirectoryView *view,
				 CajaZoomLevel zoom_level)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	if (!fm_directory_view_supports_zooming (view)) {
		return;
	}

	EEL_CALL_METHOD
		(FM_DIRECTORY_VIEW_CLASS, view,
		 zoom_to_level, (view, zoom_level));
}

CajaZoomLevel
fm_directory_view_get_zoom_level (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), CAJA_ZOOM_LEVEL_STANDARD);

	if (!fm_directory_view_supports_zooming (view)) {
		return CAJA_ZOOM_LEVEL_STANDARD;
	}

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 get_zoom_level, (view));
}

/**
 * fm_directory_view_restore_default_zoom_level:
 *
 * restore to the default zoom level by invoking the relevant subclass through the slot
 *
 **/
void
fm_directory_view_restore_default_zoom_level (FMDirectoryView *view)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	if (!fm_directory_view_supports_zooming (view)) {
		return;
	}

	EEL_CALL_METHOD
		(FM_DIRECTORY_VIEW_CLASS, view,
		 restore_default_zoom_level, (view));
}

/**
 * fm_directory_view_can_zoom_in:
 *
 * Determine whether the view can be zoomed any closer.
 * @view: The zoomable FMDirectoryView.
 *
 * Return value: TRUE if @view can be zoomed any closer, FALSE otherwise.
 *
 **/
gboolean
fm_directory_view_can_zoom_in (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	if (!fm_directory_view_supports_zooming (view)) {
		return FALSE;
	}

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 can_zoom_in, (view));
}

/**
 * fm_directory_view_can_rename_file
 *
 * Determine whether a file can be renamed.
 * @file: A CajaFile
 *
 * Return value: TRUE if @file can be renamed, FALSE otherwise.
 *
 **/
static gboolean
fm_directory_view_can_rename_file (FMDirectoryView *view, CajaFile *file)
{
	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 can_rename_file, (view, file));
}

/**
 * fm_directory_view_can_zoom_out:
 *
 * Determine whether the view can be zoomed any further away.
 * @view: The zoomable FMDirectoryView.
 *
 * Return value: TRUE if @view can be zoomed any further away, FALSE otherwise.
 *
 **/
gboolean
fm_directory_view_can_zoom_out (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	if (!fm_directory_view_supports_zooming (view)) {
		return FALSE;
	}

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 can_zoom_out, (view));
}

GtkWidget *
fm_directory_view_get_background_widget (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), NULL);

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 get_background_widget, (view));
}

EelBackground *
fm_directory_view_get_background (FMDirectoryView *view)
{
	return eel_get_widget_background (fm_directory_view_get_background_widget (view));
}

static void
real_set_is_active (FMDirectoryView *view,
		    gboolean is_active)
{
	EelBackground *bg;

	bg = fm_directory_view_get_background (view);
	eel_background_set_active (bg, is_active);
}

static void
fm_directory_view_set_is_active (FMDirectoryView *view,
				 gboolean is_active)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	EEL_CALL_METHOD (FM_DIRECTORY_VIEW_CLASS, view,
			 set_is_active, (view, is_active));
}

/**
 * fm_directory_view_get_selection:
 *
 * Get a list of CajaFile pointers that represents the
 * currently-selected items in this view. Subclasses must override
 * the signal handler for the 'get_selection' signal. Callers are
 * responsible for g_free-ing the list (but not its data).
 * @view: FMDirectoryView whose selected items are of interest.
 *
 * Return value: GList of CajaFile pointers representing the selection.
 *
 **/
GList *
fm_directory_view_get_selection (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), NULL);

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 get_selection, (view));
}

void
fm_directory_view_invert_selection (FMDirectoryView *view)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	EEL_CALL_METHOD
		(FM_DIRECTORY_VIEW_CLASS, view,
		 invert_selection, (view));
}

GList *
fm_directory_view_get_selection_for_file_transfer (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), NULL);

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 get_selection_for_file_transfer, (view));
}

guint
fm_directory_view_get_item_count (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), 0);

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 get_item_count, (view));
}

GtkUIManager *
fm_directory_view_get_ui_manager (FMDirectoryView  *view)
{
	if (view->details->window == NULL) {
		return NULL;
	}
	return caja_window_info_get_ui_manager (view->details->window);
}

/**
 * fm_directory_view_get_model:
 *
 * Get the model for this FMDirectoryView.
 * @view: FMDirectoryView of interest.
 *
 * Return value: CajaDirectory for this view.
 *
 **/
CajaDirectory *
fm_directory_view_get_model (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), NULL);

	return view->details->model;
}

GdkAtom
fm_directory_view_get_copied_files_atom (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), GDK_NONE);

	return copied_files_atom;
}

static void
prepend_uri_one (gpointer data, gpointer callback_data)
{
	CajaFile *file;
	GList **result;

	g_assert (CAJA_IS_FILE (data));
	g_assert (callback_data != NULL);

	result = (GList **) callback_data;
	file = (CajaFile *) data;
	*result = g_list_prepend (*result, caja_file_get_uri (file));
}

static void
offset_drop_points (GArray *relative_item_points,
		    int x_offset, int y_offset)
{
	guint index;

	if (relative_item_points == NULL) {
		return;
	}

	for (index = 0; index < relative_item_points->len; index++) {
		g_array_index (relative_item_points, GdkPoint, index).x += x_offset;
		g_array_index (relative_item_points, GdkPoint, index).y += y_offset;
	}
}

static void
fm_directory_view_create_links_for_files (FMDirectoryView *view, GList *files,
					  GArray *relative_item_points)
{
	GList *uris;
	char *dir_uri;
	CopyMoveDoneData *copy_move_done_data;
	g_assert (relative_item_points->len == 0
		  || g_list_length (files) == relative_item_points->len);

        g_assert (FM_IS_DIRECTORY_VIEW (view));
        g_assert (files != NULL);

	/* create a list of URIs */
	uris = NULL;
	g_list_foreach (files, prepend_uri_one, &uris);
	uris = g_list_reverse (uris);

        g_assert (g_list_length (uris) == g_list_length (files));

	/* offset the drop locations a bit so that we don't pile
	 * up the icons on top of each other
	 */
	offset_drop_points (relative_item_points,
			    DUPLICATE_HORIZONTAL_ICON_OFFSET,
			    DUPLICATE_VERTICAL_ICON_OFFSET);

        copy_move_done_data = pre_copy_move (view);
	dir_uri = fm_directory_view_get_backing_uri (view);
	caja_file_operations_copy_move (uris, relative_item_points, dir_uri, GDK_ACTION_LINK,
					    GTK_WIDGET (view), copy_move_done_callback, copy_move_done_data);
	g_free (dir_uri);
	g_list_free_full (uris, g_free);
}

static void
fm_directory_view_duplicate_selection (FMDirectoryView *view, GList *files,
				       GArray *relative_item_points)
{
	GList *uris;
	CopyMoveDoneData *copy_move_done_data;

        g_assert (FM_IS_DIRECTORY_VIEW (view));
        g_assert (files != NULL);
	g_assert (g_list_length (files) == relative_item_points->len
		|| relative_item_points->len == 0);

	/* create a list of URIs */
	uris = NULL;
	g_list_foreach (files, prepend_uri_one, &uris);
	uris = g_list_reverse (uris);

        g_assert (g_list_length (uris) == g_list_length (files));

	/* offset the drop locations a bit so that we don't pile
	 * up the icons on top of each other
	 */
	offset_drop_points (relative_item_points,
			    DUPLICATE_HORIZONTAL_ICON_OFFSET,
			    DUPLICATE_VERTICAL_ICON_OFFSET);

        copy_move_done_data = pre_copy_move (view);
	caja_file_operations_copy_move (uris, relative_item_points, NULL, GDK_ACTION_COPY,
		GTK_WIDGET (view), copy_move_done_callback, copy_move_done_data);
	g_list_free_full (uris, g_free);
}

/* special_link_in_selection
 *
 * Return TRUE if one of our special links is in the selection.
 * Special links include the following:
 *	 CAJA_DESKTOP_LINK_TRASH, CAJA_DESKTOP_LINK_HOME, CAJA_DESKTOP_LINK_MOUNT
 */

static gboolean
special_link_in_selection (FMDirectoryView *view)
{
	gboolean saw_link;
	GList *selection, *node;
	CajaFile *file = NULL;

	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	saw_link = FALSE;

	selection = fm_directory_view_get_selection (FM_DIRECTORY_VIEW (view));

	for (node = selection; node != NULL; node = node->next) {
		file = CAJA_FILE (node->data);

		saw_link = CAJA_IS_DESKTOP_ICON_FILE (file);

		if (saw_link) {
			break;
		}
	}

	caja_file_list_free (selection);

	return saw_link;
}

/* desktop_or_home_dir_in_selection
 *
 * Return TRUE if either the desktop or the home directory is in the selection.
 */

static gboolean
desktop_or_home_dir_in_selection (FMDirectoryView *view)
{
	gboolean saw_desktop_or_home_dir;
	GList *selection, *node;
	CajaFile *file = NULL;

	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	saw_desktop_or_home_dir = FALSE;

	selection = fm_directory_view_get_selection (FM_DIRECTORY_VIEW (view));

	for (node = selection; node != NULL; node = node->next) {
		file = CAJA_FILE (node->data);

		saw_desktop_or_home_dir =
			caja_file_is_home (file)
			|| caja_file_is_desktop_directory (file);

		if (saw_desktop_or_home_dir) {
			break;
		}
	}

	caja_file_list_free (selection);

	return saw_desktop_or_home_dir;
}

static void
trash_or_delete_done_cb (GHashTable *debuting_uris,
			 gboolean user_cancel,
			 FMDirectoryView *view)
{
	if (user_cancel) {
		view->details->selection_was_removed = FALSE;
	}
}

static void
trash_or_delete_files (GtkWindow *parent_window,
		       const GList *files,
		       gboolean delete_if_all_already_in_trash,
		       FMDirectoryView *view)
{
	GList *locations;
	const GList *node;

	locations = NULL;
	for (node = files; node != NULL; node = node->next) {
		locations = g_list_prepend (locations,
					    caja_file_get_location ((CajaFile *) node->data));
	}

	locations = g_list_reverse (locations);

	caja_file_operations_trash_or_delete (locations,
						  parent_window,
						  (CajaDeleteCallback) trash_or_delete_done_cb,
						  view);
	g_list_free_full (locations, g_object_unref);
}

static gboolean
can_rename_file (FMDirectoryView *view, CajaFile *file)
{
	return caja_file_can_rename (file);
}

static void
start_renaming_file (FMDirectoryView *view,
		     CajaFile *file,
		     gboolean select_all)
{
	if (file !=  NULL) {
		fm_directory_view_select_file (view, file);
	}
}

typedef struct {
	FMDirectoryView *view;
	CajaFile *new_file;
} RenameData;

static gboolean
delayed_rename_file_hack_callback (RenameData *data)
{
	FMDirectoryView *view;
	CajaFile *new_file;

	view = data->view;
	new_file = data->new_file;

	if (view->details->window != NULL &&
	    view->details->active) {
		EEL_CALL_METHOD (FM_DIRECTORY_VIEW_CLASS, view, start_renaming_file, (view, new_file, FALSE));
		fm_directory_view_reveal_selection (view);
	}

	return FALSE;
}

static void
delayed_rename_file_hack_removed (RenameData *data)
{
	g_object_unref (data->view);
	caja_file_unref (data->new_file);
	g_free (data);
}

static void
rename_file (FMDirectoryView *view, CajaFile *new_file)
{
	/* HACK!!!!
	   This is a work around bug in listview. After the rename is
	   enabled we will get file changes due to info about the new
	   file being read, which will cause the model to change. When
	   the model changes GtkTreeView clears the editing. This hack just
	   delays editing for some time to try to avoid this problem.
	   A major problem is that the selection of the row causes us
	   to load the slow mimetype for the file, which leads to a
	   file_changed. So, before we delay we select the row.
	*/
	if (FM_IS_LIST_VIEW (view)) {
		RenameData *data;

		fm_directory_view_select_file (view, new_file);

		data = g_new (RenameData, 1);
		data->view = g_object_ref (view);
		data->new_file = caja_file_ref (new_file);
		if (view->details->delayed_rename_file_id != 0) {
			g_source_remove (view->details->delayed_rename_file_id);
		}
		view->details->delayed_rename_file_id =
			g_timeout_add_full (G_PRIORITY_DEFAULT,
					    100, (GSourceFunc)delayed_rename_file_hack_callback,
					    data, (GDestroyNotify) delayed_rename_file_hack_removed);

		return;
	}

	/* no need to select because start_renaming_file selects
	 * fm_directory_view_select_file (view, new_file);
	 */
	EEL_CALL_METHOD (FM_DIRECTORY_VIEW_CLASS, view, start_renaming_file, (view, new_file, FALSE));
	fm_directory_view_reveal_selection (view);
}

static void
reveal_newly_added_folder (FMDirectoryView *view, CajaFile *new_file,
			   CajaDirectory *directory, GFile *target_location)
{
	GFile *location;

	location = caja_file_get_location (new_file);
	if (g_file_equal (location, target_location)) {
		g_signal_handlers_disconnect_by_func (view,
						      G_CALLBACK (reveal_newly_added_folder),
						      (void *) target_location);
		rename_file (view, new_file);
	}
	g_object_unref (location);
}

typedef struct {
	FMDirectoryView *directory_view;
	GHashTable *added_locations;
} NewFolderData;

static void
track_newly_added_locations (FMDirectoryView *view, CajaFile *new_file,
			     CajaDirectory *directory, gpointer user_data)
{
	NewFolderData *data;

	data = user_data;

	g_hash_table_insert (data->added_locations, caja_file_get_location (new_file), NULL);
}

static void
new_folder_done (GFile *new_folder, gpointer user_data)
{
	FMDirectoryView *directory_view;
	CajaFile *file;
	char screen_string[32];
	GdkScreen *screen;
	NewFolderData *data;

	data = (NewFolderData *)user_data;

	directory_view = data->directory_view;

	if (directory_view == NULL) {
		goto fail;
	}

	g_signal_handlers_disconnect_by_func (directory_view,
					      G_CALLBACK (track_newly_added_locations),
					      (void *) data);

	if (new_folder == NULL) {
		goto fail;
	}

	screen = gtk_widget_get_screen (GTK_WIDGET (directory_view));
	g_snprintf (screen_string, sizeof (screen_string), "%d", gdk_x11_screen_get_screen_number (screen));

	file = caja_file_get (new_folder);
	caja_file_set_metadata
		(file, CAJA_METADATA_KEY_SCREEN,
		 NULL,
		 screen_string);

	if (g_hash_table_lookup_extended (data->added_locations, new_folder, NULL, NULL)) {
		/* The file was already added */
		rename_file (directory_view, file);
	} else {
		/* We need to run after the default handler adds the folder we want to
		 * operate on. The ADD_FILE signal is registered as G_SIGNAL_RUN_LAST, so we
		 * must use connect_after.
		 */
		g_signal_connect_data (directory_view,
				       "add_file",
				       G_CALLBACK (reveal_newly_added_folder),
				       g_object_ref (new_folder),
				       (GClosureNotify)g_object_unref,
				       G_CONNECT_AFTER);
	}
	caja_file_unref (file);

 fail:
	g_hash_table_destroy (data->added_locations);
	eel_remove_weak_pointer (&data->directory_view);
	g_free (data);
}

static NewFolderData *
new_folder_data_new (FMDirectoryView *directory_view)
{
	NewFolderData *data;

	data = g_new (NewFolderData, 1);
	data->directory_view = directory_view;
	data->added_locations = g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal,
						       g_object_unref, NULL);
	eel_add_weak_pointer (&data->directory_view);

	return data;
}

static GdkPoint *
context_menu_to_file_operation_position (FMDirectoryView *directory_view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (directory_view), NULL);

	if (fm_directory_view_using_manual_layout (directory_view)
	    && directory_view->details->context_menu_position.x >= 0
	    && directory_view->details->context_menu_position.y >= 0) {
		EEL_CALL_METHOD (FM_DIRECTORY_VIEW_CLASS, directory_view,
				 widget_to_file_operation_position,
				 (directory_view, &directory_view->details->context_menu_position));
		return &directory_view->details->context_menu_position;
	} else {
		return NULL;
	}
}

static void
update_context_menu_position_from_event (FMDirectoryView *view,
					 GdkEventButton  *event)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	if (event != NULL) {
		view->details->context_menu_position.x = event->x;
		view->details->context_menu_position.y = event->y;
	} else {
		view->details->context_menu_position.x = -1;
		view->details->context_menu_position.y = -1;
	}
}

void
fm_directory_view_new_folder (FMDirectoryView *directory_view)
{
	char *parent_uri;
	NewFolderData *data;
	GdkPoint *pos;

	data = new_folder_data_new (directory_view);

	g_signal_connect_data (directory_view,
			       "add_file",
			       G_CALLBACK (track_newly_added_locations),
			       data,
			       (GClosureNotify)NULL,
			       G_CONNECT_AFTER);

	pos = context_menu_to_file_operation_position (directory_view);

	parent_uri = fm_directory_view_get_backing_uri (directory_view);
	caja_file_operations_new_folder (GTK_WIDGET (directory_view),
					     pos, parent_uri,
					     new_folder_done, data);

	g_free (parent_uri);
}

static NewFolderData *
setup_new_folder_data (FMDirectoryView *directory_view)
{
	NewFolderData *data;

	data = new_folder_data_new (directory_view);

	g_signal_connect_data (directory_view,
			       "add_file",
			       G_CALLBACK (track_newly_added_locations),
			       data,
			       (GClosureNotify)NULL,
			       G_CONNECT_AFTER);

	return data;
}

static void
fm_directory_view_new_file_with_initial_contents (FMDirectoryView *directory_view,
						  const char *parent_uri,
						  const char *filename,
						  const char *initial_contents,
						  int length,
						  GdkPoint *pos)
{
	NewFolderData *data;

	g_assert (parent_uri != NULL);

	data = setup_new_folder_data (directory_view);

	if (pos == NULL) {
		pos = context_menu_to_file_operation_position (directory_view);
	}

	caja_file_operations_new_file (GTK_WIDGET (directory_view),
					   pos, parent_uri, filename,
					   initial_contents, length,
					   new_folder_done, data);
}

void
fm_directory_view_new_file (FMDirectoryView *directory_view,
			    const char *parent_uri,
			    CajaFile *source)
{
	GdkPoint *pos;
	NewFolderData *data;
	char *source_uri;
	char *container_uri;

	container_uri = NULL;
	if (parent_uri == NULL) {
		container_uri = fm_directory_view_get_backing_uri (directory_view);
		g_assert (container_uri != NULL);
	}

	if (source == NULL) {
		fm_directory_view_new_file_with_initial_contents (directory_view,
								  parent_uri != NULL ? parent_uri : container_uri,
								  NULL,
								  NULL,
								  0,
								  NULL);
		g_free (container_uri);
		return;
	}

	g_return_if_fail (caja_file_is_local (source));

	pos = context_menu_to_file_operation_position (directory_view);

	data = setup_new_folder_data (directory_view);

	source_uri = caja_file_get_uri (source);

	caja_file_operations_new_file_from_template (GTK_WIDGET (directory_view),
							 pos,
							 parent_uri != NULL ? parent_uri : container_uri,
							 NULL,
							 source_uri,
							 new_folder_done, data);

	g_free (source_uri);
	g_free (container_uri);
}

/* handle the open command */

static void
open_one_in_new_window (gpointer data, gpointer callback_data)
{
	g_assert (CAJA_IS_FILE (data));
	g_assert (FM_IS_DIRECTORY_VIEW (callback_data));

	fm_directory_view_activate_file (FM_DIRECTORY_VIEW (callback_data),
					 CAJA_FILE (data),
					 CAJA_WINDOW_OPEN_IN_NAVIGATION,
					 0);
}

static void
open_one_in_folder_window (gpointer data, gpointer callback_data)
{
	g_assert (CAJA_IS_FILE (data));
	g_assert (FM_IS_DIRECTORY_VIEW (callback_data));

	fm_directory_view_activate_file (FM_DIRECTORY_VIEW (callback_data),
					 CAJA_FILE (data),
					 CAJA_WINDOW_OPEN_IN_SPATIAL,
					 0);
}

CajaFile *
fm_directory_view_get_directory_as_file (FMDirectoryView *view)
{
	g_assert (FM_IS_DIRECTORY_VIEW (view));

	return view->details->directory_as_file;
}

static void
open_with_launch_application_callback (GtkAction *action,
				       gpointer callback_data)
{
	ApplicationLaunchParameters *launch_parameters;

	launch_parameters = (ApplicationLaunchParameters *) callback_data;
	caja_launch_application
		(launch_parameters->application,
		 launch_parameters->files,
		 fm_directory_view_get_containing_window (launch_parameters->directory_view));
}

static void
open_parent_folder_callback (GtkAction *action,
			     gpointer callback_data)
{
	gchar *uri;

	uri = (gchar *) callback_data;
	g_app_info_launch_default_for_uri (uri, NULL, NULL);
}

static char *
escape_action_name (const char *action_name,
		    const char *prefix)
{
	GString *s;

	if (action_name == NULL) {
		return NULL;
	}

	s = g_string_new (prefix);

	while (*action_name != 0) {
		switch (*action_name) {
		case '\\':
			g_string_append (s, "\\\\");
			break;
		case '/':
			g_string_append (s, "\\s");
			break;
		case '&':
			g_string_append (s, "\\a");
			break;
		case '"':
			g_string_append (s, "\\q");
			break;
		default:
			g_string_append_c (s, *action_name);
		}

		action_name ++;
	}
	return g_string_free (s, FALSE);
}

static char *
escape_action_path (const char *action_path)
{
	GString *s;

	if (action_path == NULL) {
		return NULL;
	}

	s = g_string_sized_new (strlen (action_path) + 2);

	while (*action_path != 0) {
		switch (*action_path) {
		case '\\':
			g_string_append (s, "\\\\");
			break;
		case '&':
			g_string_append (s, "\\a");
			break;
		case '"':
			g_string_append (s, "\\q");
			break;
		default:
			g_string_append_c (s, *action_path);
		}

		action_path ++;
	}
	return g_string_free (s, FALSE);
}

static void
add_submenu (GtkUIManager *ui_manager,
	     GtkActionGroup *action_group,
	     guint merge_id,
	     const char *parent_path,
	     const char *uri,
	     const char *label,
	     cairo_surface_t *surface,
	     gboolean add_action)
{
	if (parent_path != NULL) {
		char *escaped_label;
		char *action_name;
		char *submenu_name;
		char *escaped_submenu_name;

		action_name = escape_action_name (uri, "submenu_");
		submenu_name = g_path_get_basename (uri);
		escaped_submenu_name = escape_action_path (submenu_name);
		escaped_label = eel_str_double_underscores (label);

		if (add_action) {
			GtkAction *action;

			G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
			action = gtk_action_new (action_name,
						 escaped_label,
						 NULL,
						 NULL);
			G_GNUC_END_IGNORE_DEPRECATIONS;
			if (surface != NULL) {
				g_object_set_data_full (G_OBJECT (action), "menu-icon",
							cairo_surface_reference (surface),
							(GDestroyNotify)cairo_surface_destroy);
			}

			g_object_set (action, "hide-if-empty", FALSE, NULL);

			G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
			gtk_action_group_add_action (action_group,
						     action);
			G_GNUC_END_IGNORE_DEPRECATIONS;
			g_object_unref (action);
		}

		gtk_ui_manager_add_ui (ui_manager,
				       merge_id,
				       parent_path,
				       escaped_submenu_name,
				       action_name,
				       GTK_UI_MANAGER_MENU,
				       FALSE);
		g_free (action_name);
		g_free (escaped_label);
		g_free (submenu_name);
		g_free (escaped_submenu_name);
	}
}

static void
add_application_to_open_with_menu (FMDirectoryView *view,
				   GAppInfo *application,
				   GList *files,
				   int index,
				   const char *menu_placeholder,
				   const char *popup_placeholder,
				   const gboolean submenu)
{
	ApplicationLaunchParameters *launch_parameters;
	char *tip;
	char *label;
	char *action_name;
	char *escaped_app;
	char *path;
	GtkAction *action;
	GIcon *app_icon;
	GtkWidget *menuitem;

	launch_parameters = application_launch_parameters_new
		(application, files, view);
	escaped_app = eel_str_double_underscores (g_app_info_get_display_name (application));
	if (submenu)
		label = g_strdup_printf ("%s", escaped_app);
	else
		label = g_strdup_printf (_("Open With %s"), escaped_app);

	tip = g_strdup_printf (ngettext ("Use \"%s\" to open the selected item",
					 "Use \"%s\" to open the selected items",
					 g_list_length (files)),
			       escaped_app);
	g_free (escaped_app);

	action_name = g_strdup_printf ("open_with_%d", index);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_new (action_name,
				 label,
				 tip,
				 NULL);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	app_icon = g_app_info_get_icon (application);
	if (app_icon != NULL) {
		g_object_ref (app_icon);
	} else {
		app_icon = g_themed_icon_new ("application-x-executable");
	}

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	gtk_action_set_gicon (action, app_icon);
	G_GNUC_END_IGNORE_DEPRECATIONS;
	g_object_unref (app_icon);

	g_signal_connect_data (action, "activate",
			       G_CALLBACK (open_with_launch_application_callback),
			       launch_parameters,
			       (GClosureNotify)application_launch_parameters_free, 0);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	gtk_action_group_add_action (view->details->open_with_action_group,
				     action);
	G_GNUC_END_IGNORE_DEPRECATIONS;
	g_object_unref (action);

	gtk_ui_manager_add_ui (caja_window_info_get_ui_manager (view->details->window),
			       view->details->open_with_merge_id,
			       menu_placeholder,
			       action_name,
			       action_name,
			       GTK_UI_MANAGER_MENUITEM,
			       FALSE);

	path = g_strdup_printf ("%s/%s", menu_placeholder, action_name);
	menuitem = gtk_ui_manager_get_widget (
			caja_window_info_get_ui_manager (view->details->window),
			path);
	gtk_image_menu_item_set_always_show_image (GTK_IMAGE_MENU_ITEM (menuitem), TRUE);
	g_free (path);

	gtk_ui_manager_add_ui (caja_window_info_get_ui_manager (view->details->window),
			       view->details->open_with_merge_id,
			       popup_placeholder,
			       action_name,
			       action_name,
			       GTK_UI_MANAGER_MENUITEM,
			       FALSE);

	path = g_strdup_printf ("%s/%s", popup_placeholder, action_name);
	menuitem = gtk_ui_manager_get_widget (
			caja_window_info_get_ui_manager (view->details->window),
			path);
	gtk_image_menu_item_set_always_show_image (GTK_IMAGE_MENU_ITEM (menuitem), TRUE);

	g_free (path);
	g_free (action_name);
	g_free (label);
	g_free (tip);
}

static void
add_parent_folder_to_open_menu (FMDirectoryView *view,
				GList *files,
				const char *menu_placeholder,
				const char *popup_placeholder)
{
	CajaFile *file;
	gchar *uri;
	char *tip;
	char *label;
	char *action_name;
	char *path;
	GtkAction *action;
	GtkWidget *menuitem;

	file = g_list_first(files)->data;

	if (caja_file_is_directory (file))
		return;

	uri = caja_file_get_parent_uri (file);

	label = g_strdup (_("Open parent location"));
	tip = g_strdup (_("Open parent location for the selected item"));
	action_name = g_strdup ("open_location");

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_new (action_name,
				 label,
				 tip,
				 NULL);

	gtk_action_set_icon_name (action, "folder");
	G_GNUC_END_IGNORE_DEPRECATIONS;

	g_signal_connect_data (action, "activate",
			       G_CALLBACK (open_parent_folder_callback),
			       uri, (GClosureNotify)g_free, 0);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	gtk_action_group_add_action (view->details->open_with_action_group,
				     action);
	G_GNUC_END_IGNORE_DEPRECATIONS;
	g_object_unref (action);

	gtk_ui_manager_add_ui (caja_window_info_get_ui_manager (view->details->window),
			       view->details->open_with_merge_id,
			       menu_placeholder,
			       action_name,
			       action_name,
			       GTK_UI_MANAGER_MENUITEM,
			       FALSE);

	path = g_strdup_printf ("%s/%s", menu_placeholder, action_name);
	menuitem = gtk_ui_manager_get_widget (
			caja_window_info_get_ui_manager (view->details->window),
			path);
	gtk_image_menu_item_set_always_show_image (GTK_IMAGE_MENU_ITEM (menuitem), TRUE);
	g_free (path);

	gtk_ui_manager_add_ui (caja_window_info_get_ui_manager (view->details->window),
			       view->details->open_with_merge_id,
			       popup_placeholder,
			       action_name,
			       action_name,
			       GTK_UI_MANAGER_MENUITEM,
			       FALSE);

	path = g_strdup_printf ("%s/%s", popup_placeholder, action_name);
	menuitem = gtk_ui_manager_get_widget (
			caja_window_info_get_ui_manager (view->details->window),
			path);
	gtk_image_menu_item_set_always_show_image (GTK_IMAGE_MENU_ITEM (menuitem), TRUE);

	g_free (path);
	g_free (action_name);
	g_free (label);
	g_free (tip);
}

static void
get_x_content_async_callback (char **content,
			      gpointer user_data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (user_data);

	if (view->details->window != NULL) {
		schedule_update_menus (view);
	}
	g_object_unref (view);
}

static void
add_x_content_apps (FMDirectoryView *view, CajaFile *file, GList **applications)
{
	GMount *mount;
	char **x_content_types;

	g_return_if_fail (applications != NULL);

	mount = caja_file_get_mount (file);

	if (mount == NULL) {
		return;
	}

	x_content_types = caja_autorun_get_cached_x_content_types_for_mount (mount);
	if (x_content_types != NULL) {
		unsigned int n;

		for (n = 0; x_content_types[n] != NULL; n++) {
			char *x_content_type = x_content_types[n];
			GList *app_info_for_x_content_type;

			app_info_for_x_content_type = g_app_info_get_all_for_type (x_content_type);
			*applications = g_list_concat (*applications, app_info_for_x_content_type);
		}
		g_strfreev (x_content_types);
	} else {
		caja_autorun_get_x_content_types_for_mount_async (mount,
								      get_x_content_async_callback,
								      NULL,
								      g_object_ref (view));

	}

	g_object_unref (mount);
}

static void
reset_open_with_menu (FMDirectoryView *view, GList *selection)
{
	GList *applications, *node;
	gboolean submenu_visible, filter_default;
	int num_applications;
	int index;
	gboolean other_applications_visible;
	gboolean open_with_chooser_visible;
	GtkUIManager *ui_manager;
	GtkAction *action;
	GAppInfo *default_app;
	char *uri;

	/* Clear any previous inserted items in the applications and viewers placeholders */

	ui_manager = caja_window_info_get_ui_manager (view->details->window);
	caja_ui_unmerge_ui (ui_manager,
				&view->details->open_with_merge_id,
				&view->details->open_with_action_group);

	caja_ui_prepare_merge_ui (ui_manager,
				      "OpenWithGroup",
				      &view->details->open_with_merge_id,
				      &view->details->open_with_action_group);

	num_applications = 0;

	other_applications_visible = (selection != NULL);
	filter_default = (selection != NULL);

	default_app = NULL;
	if (filter_default) {
		default_app = caja_mime_get_default_application_for_files (selection);
	}

	applications = NULL;
	if (other_applications_visible) {
		applications = caja_mime_get_applications_for_files (selection);
	}

	if (g_list_length (selection) == 1) {
		add_x_content_apps (view, CAJA_FILE (selection->data), &applications);
	}

	num_applications = g_list_length (applications);

	if (file_list_all_are_folders (selection)) {
		submenu_visible = (num_applications > 2);
	} else {
		submenu_visible = (num_applications > 3);
	}

	for (node = applications, index = 0; node != NULL; node = node->next, index++) {
		GAppInfo *application;
		char *menu_path;
		char *popup_path;

		application = node->data;

		if (default_app != NULL && g_app_info_equal (default_app, application)) {
			continue;
		}

		if (submenu_visible) {
			menu_path = FM_DIRECTORY_VIEW_MENU_PATH_APPLICATIONS_SUBMENU_PLACEHOLDER;
			popup_path = FM_DIRECTORY_VIEW_POPUP_PATH_APPLICATIONS_SUBMENU_PLACEHOLDER;
		} else {
			menu_path = FM_DIRECTORY_VIEW_MENU_PATH_APPLICATIONS_PLACEHOLDER;
			popup_path = FM_DIRECTORY_VIEW_POPUP_PATH_APPLICATIONS_PLACEHOLDER;
		}

		gtk_ui_manager_add_ui (caja_window_info_get_ui_manager (view->details->window),
				       view->details->open_with_merge_id,
				       menu_path,
				       "separator",
				       NULL,
				       GTK_UI_MANAGER_SEPARATOR,
				       FALSE);

		add_application_to_open_with_menu (view,
						   node->data,
						   selection,
						   index,
						   menu_path, popup_path, submenu_visible);

	}
	g_list_free_full (applications, g_object_unref);
	if (default_app != NULL) {
		g_object_unref (default_app);
	}

	/* Show open parent folder action if we are in search mode */
	uri = fm_directory_view_get_uri (view);
	if (eel_uri_is_search (uri) && g_list_length (selection) == 1)
		add_parent_folder_to_open_menu (view,
					       selection,
					       FM_DIRECTORY_VIEW_MENU_PATH_OPEN,
					       FM_DIRECTORY_VIEW_POPUP_PATH_OPEN);

	g_free (uri);

	open_with_chooser_visible = other_applications_visible &&
				    g_list_length (selection) == 1;

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	if (submenu_visible) {
		action = gtk_action_group_get_action (view->details->dir_action_group,
						      FM_ACTION_OTHER_APPLICATION1);
		gtk_action_set_visible (action, open_with_chooser_visible);
		action = gtk_action_group_get_action (view->details->dir_action_group,
						      FM_ACTION_OTHER_APPLICATION2);
		gtk_action_set_visible (action, FALSE);
	} else {
		action = gtk_action_group_get_action (view->details->dir_action_group,
						      FM_ACTION_OTHER_APPLICATION1);
		gtk_action_set_visible (action, FALSE);
		action = gtk_action_group_get_action (view->details->dir_action_group,
						      FM_ACTION_OTHER_APPLICATION2);
		gtk_action_set_visible (action, open_with_chooser_visible);
	}
	G_GNUC_END_IGNORE_DEPRECATIONS;
}

static GList *
get_all_extension_menu_items (GtkWidget *window,
			      GList *selection)
{
	GList *items;
	GList *providers;
	GList *l;

	providers = caja_extensions_get_for_type (CAJA_TYPE_MENU_PROVIDER);
	items = NULL;

	for (l = providers; l != NULL; l = l->next) {
		CajaMenuProvider *provider;
		GList *file_items;

		provider = CAJA_MENU_PROVIDER (l->data);
		file_items = caja_menu_provider_get_file_items (provider,
								    window,
								    selection);
		items = g_list_concat (items, file_items);
	}

	caja_module_extension_list_free (providers);

	return items;
}

typedef struct
{
	CajaMenuItem *item;
	FMDirectoryView *view;
	GList *selection;
	GtkAction *action;
} ExtensionActionCallbackData;

static void
extension_action_callback_data_free (ExtensionActionCallbackData *data)
{
	g_object_unref (data->item);
	caja_file_list_free (data->selection);

	g_free (data);
}

static gboolean
search_in_menu_items (GList* items, const char *item_name)
{
	GList* list;

	for (list = items; list != NULL; list = list->next) {
		CajaMenu* menu;
		char *name;

		g_object_get (list->data, "name", &name, NULL);
		if (strcmp (name, item_name) == 0) {
			g_free (name);
			return TRUE;
		}
		g_free (name);

		menu = NULL;
		g_object_get (list->data, "menu", &menu, NULL);
		if (menu != NULL) {
			gboolean ret;
			GList* submenus;

			submenus = caja_menu_get_items (menu);
			ret = search_in_menu_items (submenus, item_name);
			caja_menu_item_list_free (submenus);
			g_object_unref (menu);
			if (ret) {
			    return TRUE;
			}
		}
	}
	return FALSE;
}

static void
extension_action_callback (GtkAction *action,
			   gpointer callback_data)
{
	ExtensionActionCallbackData *data;
	char *item_name;
	gboolean is_valid;
	GList *l;
	GList *items;

	data = callback_data;

	/* Make sure the selected menu item is valid for the final sniffed
	 * mime type */
	g_object_get (data->item, "name", &item_name, NULL);
	items = get_all_extension_menu_items (gtk_widget_get_toplevel (GTK_WIDGET (data->view)),
					      data->selection);

	is_valid = search_in_menu_items (items, item_name);

	for (l = items; l != NULL; l = l->next) {
		g_object_unref (l->data);
	}
	g_list_free (items);

	g_free (item_name);

	if (is_valid) {
		caja_menu_item_activate (data->item);
	}
}

static cairo_surface_t *
get_menu_icon (const char *icon_name,
	       GtkWidget  *widget)
{
	CajaIconInfo *info;
	cairo_surface_t *surface;
	int size, scale;

	size = caja_get_icon_size_for_stock_size (GTK_ICON_SIZE_MENU);
	scale = gtk_widget_get_scale_factor (widget);

	if (g_path_is_absolute (icon_name)) {
		info = caja_icon_info_lookup_from_path (icon_name, size, scale);
	} else {
		info = caja_icon_info_lookup_from_name (icon_name, size, scale);
	}
	surface = caja_icon_info_get_surface_nodefault_at_size (info, size);
	g_object_unref (info);

	return surface;
}

static cairo_surface_t *
get_menu_icon_for_file (CajaFile  *file,
			GtkWidget *widget)
{
	CajaIconInfo *info;
	cairo_surface_t *surface;
	int size, scale;

	size = caja_get_icon_size_for_stock_size (GTK_ICON_SIZE_MENU);
	scale = gtk_widget_get_scale_factor (widget);

	info = caja_file_get_icon (file, size, scale, 0);
	surface = caja_icon_info_get_surface_nodefault_at_size (info, size);
	g_object_unref (info);

	return surface;
}

static GtkAction *
add_extension_action_for_files (FMDirectoryView *view,
				CajaMenuItem *item,
				GList *files)
{
	char *name, *label, *tip, *icon;
	gboolean sensitive, priority;
	GtkAction *action;
	ExtensionActionCallbackData *data;

	g_object_get (G_OBJECT (item),
		      "name", &name, "label", &label,
		      "tip", &tip, "icon", &icon,
		      "sensitive", &sensitive,
		      "priority", &priority,
		      NULL);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_new (name,
				 label,
				 tip,
				 icon);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	if (icon != NULL) {
		cairo_surface_t *surface;

		surface = get_menu_icon (icon, GTK_WIDGET (view));

		if (surface != NULL) {
			g_object_set_data_full (G_OBJECT (action), "menu-icon",
						surface,
						(GDestroyNotify)cairo_surface_destroy);
		}
	}

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	gtk_action_set_sensitive (action, sensitive);
	G_GNUC_END_IGNORE_DEPRECATIONS;
	g_object_set (action, "is-important", priority, NULL);

	data = g_new0 (ExtensionActionCallbackData, 1);
	data->item = g_object_ref (item);
	data->view = view;
	data->selection = caja_file_list_copy (files);
	data->action = action;

	g_signal_connect_data (action, "activate",
			       G_CALLBACK (extension_action_callback),
			       data,
			       (GClosureNotify)extension_action_callback_data_free, 0);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	gtk_action_group_add_action (view->details->extensions_menu_action_group,
				     GTK_ACTION (action));
	G_GNUC_END_IGNORE_DEPRECATIONS;
	g_object_unref (action);

	g_free (name);
	g_free (label);
	g_free (tip);
	g_free (icon);

	return action;
}

static void
add_extension_menu_items (FMDirectoryView *view,
			  GList *files,
			  GList *menu_items,
			  const char *subdirectory)
{
	GtkUIManager *ui_manager;
	GList *l;

	ui_manager = caja_window_info_get_ui_manager (view->details->window);

	for (l = menu_items; l; l = l->next) {
		CajaMenuItem *item;
		CajaMenu *menu;
		GtkAction *action;
		const gchar *action_name;
		char *path;

		item = CAJA_MENU_ITEM (l->data);

		g_object_get (item, "menu", &menu, NULL);

		action = add_extension_action_for_files (view, item, files);

		path = g_build_path ("/", FM_DIRECTORY_VIEW_POPUP_PATH_EXTENSION_ACTIONS, subdirectory, NULL);
		G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
		action_name = gtk_action_get_name (action);
		G_GNUC_END_IGNORE_DEPRECATIONS;

		gtk_ui_manager_add_ui (ui_manager,
				       view->details->extensions_menu_merge_id,
				       path,
				       action_name,
				       action_name,
				       (menu != NULL) ? GTK_UI_MANAGER_MENU : GTK_UI_MANAGER_MENUITEM,
				       FALSE);
		g_free (path);

		path = g_build_path ("/", FM_DIRECTORY_VIEW_MENU_PATH_EXTENSION_ACTIONS_PLACEHOLDER, subdirectory, NULL);
		gtk_ui_manager_add_ui (ui_manager,
				       view->details->extensions_menu_merge_id,
				       path,
				       action_name,
				       action_name,
				       (menu != NULL) ? GTK_UI_MANAGER_MENU : GTK_UI_MANAGER_MENUITEM,
				       FALSE);
		g_free (path);

		/* recursively fill the menu */
		if (menu != NULL) {
			char *subdir;
			GList *children;

			children = caja_menu_get_items (menu);

			G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
			subdir = g_build_path ("/", subdirectory, gtk_action_get_name (action), NULL);
			G_GNUC_END_IGNORE_DEPRECATIONS;
			add_extension_menu_items (view,
						  files,
						  children,
						  subdir);

			caja_menu_item_list_free (children);
			g_free (subdir);
		}
	}
}

static void
reset_extension_actions_menu (FMDirectoryView *view, GList *selection)
{
	GList *items;
	GtkUIManager *ui_manager;

	/* Clear any previous inserted items in the extension actions placeholder */
	ui_manager = caja_window_info_get_ui_manager (view->details->window);

	caja_ui_unmerge_ui (ui_manager,
				&view->details->extensions_menu_merge_id,
				&view->details->extensions_menu_action_group);

	caja_ui_prepare_merge_ui (ui_manager,
				      "DirExtensionsMenuGroup",
				      &view->details->extensions_menu_merge_id,
				      &view->details->extensions_menu_action_group);

	items = get_all_extension_menu_items (gtk_widget_get_toplevel (GTK_WIDGET (view)),
					      selection);
	if (items != NULL) {
		add_extension_menu_items (view, selection, items, "");

		g_list_free_full (items, g_object_unref);
	}
}

static char *
change_to_view_directory (FMDirectoryView *view)
{
	char *path;
	char *old_path;

	old_path = g_get_current_dir ();

	path = get_view_directory (view);

	/* FIXME: What to do about non-local directories? */
	if (path != NULL) {
		g_chdir (path);
	}

	g_free (path);

	return old_path;
}

static char **
get_file_names_as_parameter_array (GList *selection,
				   CajaDirectory *model)
{
	char **parameters;
	GList *node;
	GFile *model_location;
	int i;
	CajaFile *file = NULL;
	GFile *file_location = NULL;

	if (model == NULL) {
		return NULL;
	}

	parameters = g_new (char *, g_list_length (selection) + 1);

	model_location = caja_directory_get_location (model);

	for (node = selection, i = 0; node != NULL; node = node->next, i++) {
		file = CAJA_FILE (node->data);

		if (!caja_file_is_local (file)) {
			parameters[i] = NULL;
			g_strfreev (parameters);
			return NULL;
		}

		file_location = caja_file_get_location (CAJA_FILE (node->data));
		parameters[i] = g_file_get_relative_path (model_location, file_location);
		if (parameters[i] == NULL) {
			parameters[i] = g_file_get_path (file_location);
		}
		g_object_unref (file_location);
	}

	g_object_unref (model_location);

	parameters[i] = NULL;
	return parameters;
}

static char *
get_file_paths_or_uris_as_newline_delimited_string (GList *selection, gboolean get_paths)
{
	char *path;
	char *result;
	CajaDesktopLink *link;
	GString *expanding_string;
	GList *node;
	GFile *location;

	expanding_string = g_string_new ("");
	for (node = selection; node != NULL; node = node->next) {
		char *uri;

		uri = NULL;

		if (CAJA_IS_DESKTOP_ICON_FILE (node->data)) {
			link = caja_desktop_icon_file_get_link (CAJA_DESKTOP_ICON_FILE (node->data));
			if (link != NULL) {
				location = caja_desktop_link_get_activation_location (link);
				uri = g_file_get_uri (location);
				g_object_unref (location);
				g_object_unref (G_OBJECT (link));
			}
		} else {
			uri = caja_file_get_uri (CAJA_FILE (node->data));
		}
		if (uri == NULL) {
			continue;
		}

		if (get_paths) {
			path = g_filename_from_uri (uri, NULL, NULL);
			if (path != NULL) {
				g_string_append (expanding_string, path);
				g_free (path);
				g_string_append (expanding_string, "\n");
			}
		} else {
			g_string_append (expanding_string, uri);
			g_string_append (expanding_string, "\n");
		}
		g_free (uri);
	}

	result = expanding_string->str;
	g_string_free (expanding_string, FALSE);

	return result;
}

static char *
get_file_paths_as_newline_delimited_string (GList *selection)
{
	return get_file_paths_or_uris_as_newline_delimited_string (selection, TRUE);
}

static char *
get_file_uris_as_newline_delimited_string (GList *selection)
{
	return get_file_paths_or_uris_as_newline_delimited_string (selection, FALSE);
}

/* returns newly allocated strings for setting the environment variables */
static void
get_strings_for_environment_variables (FMDirectoryView *view, GList *selected_files,
				       char **file_paths, char **uris, char **uri)
{
	char *directory_uri;

	/* We need to check that the directory uri starts with "file:" since
	 * caja_directory_is_local returns FALSE for nfs.
	 */
	directory_uri = caja_directory_get_uri (view->details->model);
	if (eel_str_has_prefix (directory_uri, "file:") ||
	    eel_uri_is_desktop (directory_uri) ||
	    eel_uri_is_trash (directory_uri)) {
		*file_paths = get_file_paths_as_newline_delimited_string (selected_files);
	} else {
		*file_paths = g_strdup ("");
	}
	g_free (directory_uri);

	*uris = get_file_uris_as_newline_delimited_string (selected_files);

	*uri = caja_directory_get_uri (view->details->model);
	if (eel_uri_is_desktop (*uri)) {
		g_free (*uri);
		*uri = caja_get_desktop_directory_uri ();
	}
}

static FMDirectoryView *
get_directory_view_of_extra_pane (FMDirectoryView *view)
{
	CajaWindowSlotInfo *slot;

	slot = caja_window_info_get_extra_slot (fm_directory_view_get_caja_window (view));
	if (slot != NULL) {
		CajaView *next_view;

		next_view = caja_window_slot_info_get_current_view (slot);

		if (FM_IS_DIRECTORY_VIEW (next_view)) {
			return FM_DIRECTORY_VIEW (next_view);
		}
	}
	return NULL;
}

/*
 * Set up some environment variables that scripts can use
 * to take advantage of the current Caja state.
 */
static void set_script_environment_variables(FMDirectoryView* view, GList* selected_files)
{
	char* file_paths;
	char* uris;
	char* uri;
	char* geometry_string;
	FMDirectoryView* next_view;

	get_strings_for_environment_variables(view, selected_files, &file_paths, &uris, &uri);

	g_setenv("CAJA_SCRIPT_SELECTED_FILE_PATHS", file_paths, TRUE);
	g_setenv("NAUTILUS_SCRIPT_SELECTED_FILE_PATHS", file_paths, TRUE); // compatibilidad GNOME

	g_free(file_paths);

	g_setenv("CAJA_SCRIPT_SELECTED_URIS", uris, TRUE);
	g_setenv("NAUTILUS_SCRIPT_SELECTED_URIS", uris, TRUE); // compatibilidad GNOME

	g_free(uris);

	g_setenv("CAJA_SCRIPT_CURRENT_URI", uri, TRUE);
	g_setenv("NAUTILUS_SCRIPT_CURRENT_URI", uri, TRUE); // compatibilidad GNOME

	g_free(uri);

	geometry_string = eel_gtk_window_get_geometry_string(GTK_WINDOW (fm_directory_view_get_containing_window (view)));

	g_setenv("CAJA_SCRIPT_WINDOW_GEOMETRY", geometry_string, TRUE);
	g_setenv("NAUTILUS_SCRIPT_WINDOW_GEOMETRY", geometry_string, TRUE); // compatibilidad GNOME

	g_free(geometry_string);

	/* next pane */
	next_view = get_directory_view_of_extra_pane(view);

	if (next_view)
	{
		GList* next_pane_selected_files = fm_directory_view_get_selection (next_view);

		get_strings_for_environment_variables(next_view, next_pane_selected_files, &file_paths, &uris, &uri);

		caja_file_list_free(next_pane_selected_files);
	}
	else
	{
		file_paths = g_strdup("");
		uris = g_strdup("");
		uri = g_strdup("");
	}

	g_setenv("CAJA_SCRIPT_NEXT_PANE_SELECTED_FILE_PATHS", file_paths, TRUE);
	g_setenv("NAUTILUS_SCRIPT_NEXT_PANE_SELECTED_FILE_PATHS", file_paths, TRUE); // compatibilidad GNOME
	g_free(file_paths);

	g_setenv("CAJA_SCRIPT_NEXT_PANE_SELECTED_URIS", uris, TRUE);
	g_setenv("NAUTILUS_SCRIPT_NEXT_PANE_SELECTED_URIS", uris, TRUE); // compatibilidad GNOME
	g_free(uris);

	g_setenv("CAJA_SCRIPT_NEXT_PANE_CURRENT_URI", uri, TRUE);
	g_setenv("NAUTILUS_SCRIPT_NEXT_PANE_CURRENT_URI", uri, TRUE); // compatibilidad GNOME
	g_free(uri);
}

/* Unset all the special script environment variables. */
static void unset_script_environment_variables(void)
{
	g_unsetenv("CAJA_SCRIPT_SELECTED_FILE_PATHS");
	g_unsetenv("NAUTILUS_SCRIPT_SELECTED_FILE_PATHS");

	g_unsetenv("CAJA_SCRIPT_SELECTED_URIS");
	g_unsetenv("NAUTILUS_SCRIPT_SELECTED_URIS");

	g_unsetenv("CAJA_SCRIPT_CURRENT_URI");
	g_unsetenv("NAUTILUS_SCRIPT_CURRENT_URI");

	g_unsetenv("CAJA_SCRIPT_WINDOW_GEOMETRY");
	g_unsetenv("NAUTILUS_SCRIPT_WINDOW_GEOMETRY");

	g_unsetenv("CAJA_SCRIPT_NEXT_PANE_SELECTED_FILE_PATHS");
	g_unsetenv("NAUTILUS_SCRIPT_NEXT_PANE_SELECTED_FILE_PATHS");

	g_unsetenv("CAJA_SCRIPT_NEXT_PANE_SELECTED_URIS");
	g_unsetenv("NAUTILUS_SCRIPT_NEXT_PANE_SELECTED_URIS");

	g_unsetenv("CAJA_SCRIPT_NEXT_PANE_CURRENT_URI");
	g_unsetenv("NAUTILUS_SCRIPT_NEXT_PANE_CURRENT_URI");
}

static void
run_script_callback (GtkAction *action, gpointer callback_data)
{
	ScriptLaunchParameters *launch_parameters;
	GdkScreen *screen;
	GList *selected_files;
	char *file_uri;
	char *local_file_path;
	char *quoted_path;
	char *old_working_dir;
	char **parameters, *name;
	GtkWindow *window;

	launch_parameters = (ScriptLaunchParameters *) callback_data;

	file_uri = caja_file_get_uri (launch_parameters->file);
	local_file_path = g_filename_from_uri (file_uri, NULL, NULL);
	g_assert (local_file_path != NULL);
	g_free (file_uri);

	quoted_path = g_shell_quote (local_file_path);

	old_working_dir = change_to_view_directory (launch_parameters->directory_view);

	selected_files = fm_directory_view_get_selection (launch_parameters->directory_view);
	set_script_environment_variables (launch_parameters->directory_view, selected_files);

	parameters = get_file_names_as_parameter_array (selected_files,
						        launch_parameters->directory_view->details->model);

	screen = gtk_widget_get_screen (GTK_WIDGET (launch_parameters->directory_view));

	name = caja_file_get_name (launch_parameters->file);
	/* FIXME: handle errors with dialog? Or leave up to each script? */
	window = fm_directory_view_get_containing_window (launch_parameters->directory_view);
	caja_debug_log (FALSE, CAJA_DEBUG_LOG_DOMAIN_USER,
			    "directory view run_script_callback, window=%p, name=\"%s\", script_path=\"%s\" (omitting script parameters)",
			    window, name, local_file_path);
	caja_launch_application_from_command_array (screen, name, quoted_path, FALSE,
							(const char * const *) parameters);
	g_free (local_file_path);
	g_free (name);
	g_strfreev (parameters);

	caja_file_list_free (selected_files);
	unset_script_environment_variables ();
	g_chdir (old_working_dir);
	g_free (old_working_dir);
	g_free (quoted_path);
}

static void
add_script_to_scripts_menus (FMDirectoryView *directory_view,
			     CajaFile *file,
			     const char *menu_path,
			     const char *popup_path,
			     const char *popup_bg_path)
{
	ScriptLaunchParameters *launch_parameters;
	char *tip;
	char *name;
	char *uri;
	char *action_name;
	char *escaped_label;
	cairo_surface_t *surface;
	GtkUIManager *ui_manager;
	GtkAction *action;

	name = caja_file_get_display_name (file);
	uri = caja_file_get_uri (file);
	tip = g_strdup_printf (_("Run \"%s\" on any selected items"), name);

	launch_parameters = script_launch_parameters_new (file, directory_view);

	action_name = escape_action_name (uri, "script_");
	escaped_label = eel_str_double_underscores (name);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_new (action_name,
				 escaped_label,
				 tip,
				 NULL);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	surface = get_menu_icon_for_file (file, GTK_WIDGET (directory_view));
	if (surface != NULL) {
		g_object_set_data_full (G_OBJECT (action), "menu-icon",
					surface,
					(GDestroyNotify)cairo_surface_destroy);
	}

	g_signal_connect_data (action, "activate",
			       G_CALLBACK (run_script_callback),
			       launch_parameters,
			       (GClosureNotify)script_launch_parameters_free, 0);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	gtk_action_group_add_action_with_accel (directory_view->details->scripts_action_group,
						action, NULL);
	G_GNUC_END_IGNORE_DEPRECATIONS;
	g_object_unref (action);

	ui_manager = caja_window_info_get_ui_manager (directory_view->details->window);

	gtk_ui_manager_add_ui (ui_manager,
			       directory_view->details->scripts_merge_id,
			       menu_path,
			       action_name,
			       action_name,
			       GTK_UI_MANAGER_MENUITEM,
			       FALSE);
	gtk_ui_manager_add_ui (ui_manager,
			       directory_view->details->scripts_merge_id,
			       popup_path,
			       action_name,
			       action_name,
			       GTK_UI_MANAGER_MENUITEM,
			       FALSE);
	gtk_ui_manager_add_ui (ui_manager,
			       directory_view->details->scripts_merge_id,
			       popup_bg_path,
			       action_name,
			       action_name,
			       GTK_UI_MANAGER_MENUITEM,
			       FALSE);

	g_free (name);
	g_free (uri);
	g_free (tip);
	g_free (action_name);
	g_free (escaped_label);
}

static void
add_submenu_to_directory_menus (FMDirectoryView *directory_view,
				GtkActionGroup *action_group,
				guint merge_id,
				CajaFile *file,
				const char *menu_path,
				const char *popup_path,
				const char *popup_bg_path)
{
	char *name;
	cairo_surface_t *surface;
	char *uri;
	GtkUIManager *ui_manager;

	ui_manager = caja_window_info_get_ui_manager (directory_view->details->window);
	uri = caja_file_get_uri (file);
	name = caja_file_get_display_name (file);
	surface = get_menu_icon_for_file (file, GTK_WIDGET (directory_view));
	add_submenu (ui_manager, action_group, merge_id, menu_path, uri, name, surface, TRUE);
	add_submenu (ui_manager, action_group, merge_id, popup_path, uri, name, surface, FALSE);
	add_submenu (ui_manager, action_group, merge_id, popup_bg_path, uri, name, surface, FALSE);
	if (surface) {
		cairo_surface_destroy (surface);
	}
	g_free (name);
	g_free (uri);
}

static gboolean
directory_belongs_in_scripts_menu (const char *uri)
{
	int num_levels;
	int i;

	if (!eel_str_has_prefix (uri, scripts_directory_uri)) {
		return FALSE;
	}

	num_levels = 0;
	for (i = scripts_directory_uri_length; uri[i] != '\0'; i++) {
		if (uri[i] == '/') {
			num_levels++;
		}
	}

	if (num_levels > MAX_MENU_LEVELS) {
		return FALSE;
	}

	return TRUE;
}

static gboolean
update_directory_in_scripts_menu (FMDirectoryView *view, CajaDirectory *directory)
{
	char *menu_path, *popup_path, *popup_bg_path;
	GList *file_list, *filtered, *node;
	gboolean any_scripts;
	CajaDirectory *dir;
	char *uri;
	char *escaped_path;
	CajaFile *file = NULL;

	uri = caja_directory_get_uri (directory);
	escaped_path = escape_action_path (uri + scripts_directory_uri_length);
	g_free (uri);
	menu_path = g_strconcat (FM_DIRECTORY_VIEW_MENU_PATH_SCRIPTS_PLACEHOLDER,
				 escaped_path,
				 NULL);
	popup_path = g_strconcat (FM_DIRECTORY_VIEW_POPUP_PATH_SCRIPTS_PLACEHOLDER,
				  escaped_path,
				  NULL);
	popup_bg_path = g_strconcat (FM_DIRECTORY_VIEW_POPUP_PATH_BACKGROUND_SCRIPTS_PLACEHOLDER,
				  escaped_path,
				  NULL);
	g_free (escaped_path);

	file_list = caja_directory_get_file_list (directory);
	filtered = caja_file_list_filter_hidden (file_list, FALSE);
	caja_file_list_free (file_list);

	file_list = caja_file_list_sort_by_display_name (filtered);

	any_scripts = FALSE;
	for (node = file_list; node != NULL; node = node->next) {
		file = node->data;

		if (caja_file_is_launchable (file)) {
			add_script_to_scripts_menus (view, file, menu_path, popup_path, popup_bg_path);
			any_scripts = TRUE;
		} else if (caja_file_is_directory (file)) {
			uri = caja_file_get_uri (file);
			if (directory_belongs_in_scripts_menu (uri)) {
				dir = caja_directory_get_by_uri (uri);
				add_directory_to_scripts_directory_list (view, dir);
				caja_directory_unref (dir);

				add_submenu_to_directory_menus (view,
								view->details->scripts_action_group,
								view->details->scripts_merge_id,
								file, menu_path, popup_path, popup_bg_path);

				any_scripts = TRUE;
			}
			g_free (uri);
		}
	}

	caja_file_list_free (file_list);

	g_free (popup_path);
	g_free (popup_bg_path);
	g_free (menu_path);

	return any_scripts;
}

static void
update_scripts_menu (FMDirectoryView *view)
{
	gboolean any_scripts;
	GList *sorted_copy, *node;
	GtkUIManager *ui_manager;
	GtkAction *action;
	CajaDirectory *directory = NULL;

	/* There is a race condition here.  If we don't mark the scripts menu as
	   valid before we begin our task then we can lose script menu updates that
	   occur before we finish. */
	view->details->scripts_invalid = FALSE;

	ui_manager = caja_window_info_get_ui_manager (view->details->window);
	caja_ui_unmerge_ui (ui_manager,
				&view->details->scripts_merge_id,
				&view->details->scripts_action_group);

	caja_ui_prepare_merge_ui (ui_manager,
				      "ScriptsGroup",
				      &view->details->scripts_merge_id,
				      &view->details->scripts_action_group);

	/* As we walk through the directories, remove any that no longer belong. */
	any_scripts = FALSE;
	sorted_copy = caja_directory_list_sort_by_uri
		(caja_directory_list_copy (view->details->scripts_directory_list));
	for (node = sorted_copy; node != NULL; node = node->next) {
		char *uri;

		directory = node->data;

		uri = caja_directory_get_uri (directory);
		if (!directory_belongs_in_scripts_menu (uri)) {
			remove_directory_from_scripts_directory_list (view, directory);
		} else if (update_directory_in_scripts_menu (view, directory)) {
			any_scripts = TRUE;
		}
		g_free (uri);
	}
	caja_directory_list_free (sorted_copy);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group, FM_ACTION_SCRIPTS);
	gtk_action_set_visible (action, any_scripts);
	G_GNUC_END_IGNORE_DEPRECATIONS;
}

static void
create_template_callback (GtkAction *action, gpointer callback_data)
{
	CreateTemplateParameters *parameters;

	parameters = callback_data;

	fm_directory_view_new_file (parameters->directory_view, NULL, parameters->file);
}

static void
add_template_to_templates_menus (FMDirectoryView *directory_view,
				 CajaFile *file,
				 const char *menu_path,
				 const char *popup_bg_path)
{
	char *tmp, *tip, *uri, *name;
	char *escaped_label;
	cairo_surface_t *surface;
	char *action_name;
	CreateTemplateParameters *parameters;
	GtkUIManager *ui_manager;
	GtkAction *action;

	tmp = caja_file_get_display_name (file);
	name = eel_filename_strip_extension (tmp);
	g_free (tmp);

	uri = caja_file_get_uri (file);
	tip = g_strdup_printf (_("Create Document from template \"%s\""), name);

	action_name = escape_action_name (uri, "template_");
	escaped_label = eel_str_double_underscores (name);

	parameters = create_template_parameters_new (file, directory_view);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_new (action_name,
				 escaped_label,
				 tip,
				 NULL);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	surface = get_menu_icon_for_file (file, GTK_WIDGET (directory_view));
	if (surface != NULL) {
		g_object_set_data_full (G_OBJECT (action), "menu-icon",
					surface,
					(GDestroyNotify)cairo_surface_destroy);
	}

	g_signal_connect_data (action, "activate",
			       G_CALLBACK (create_template_callback),
			       parameters,
			       (GClosureNotify)create_templates_parameters_free, 0);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	gtk_action_group_add_action (directory_view->details->templates_action_group,
				     action);
	G_GNUC_END_IGNORE_DEPRECATIONS;
	g_object_unref (action);

	ui_manager = caja_window_info_get_ui_manager (directory_view->details->window);

	gtk_ui_manager_add_ui (ui_manager,
			       directory_view->details->templates_merge_id,
			       menu_path,
			       action_name,
			       action_name,
			       GTK_UI_MANAGER_MENUITEM,
			       FALSE);

	gtk_ui_manager_add_ui (ui_manager,
			       directory_view->details->templates_merge_id,
			       popup_bg_path,
			       action_name,
			       action_name,
			       GTK_UI_MANAGER_MENUITEM,
			       FALSE);

	g_free (escaped_label);
	g_free (name);
	g_free (tip);
	g_free (uri);
	g_free (action_name);
}

static void
update_templates_directory (FMDirectoryView *view)
{
	GList *node, *next;

	for (node = view->details->templates_directory_list; node != NULL; node = next) {
		next = node->next;
		remove_directory_from_templates_directory_list (view, node->data);
	}

	if (caja_should_use_templates_directory ()) {
		CajaDirectory *templates_directory;
		char *templates_uri;

		templates_uri = caja_get_templates_directory_uri ();
		templates_directory = caja_directory_get_by_uri (templates_uri);
		g_free (templates_uri);
		add_directory_to_templates_directory_list (view, templates_directory);
		caja_directory_unref (templates_directory);
	}
}

static void
user_dirs_changed (FMDirectoryView *view)
{
	update_templates_directory (view);
	view->details->templates_invalid = TRUE;
	schedule_update_menus (view);
}

static gboolean
directory_belongs_in_templates_menu (const char *templates_directory_uri,
				     const char *uri)
{
	int num_levels;
	int i;

	if (templates_directory_uri == NULL) {
		return FALSE;
	}

	if (!g_str_has_prefix (uri, templates_directory_uri)) {
		return FALSE;
	}

	num_levels = 0;
	for (i = strlen (templates_directory_uri); uri[i] != '\0'; i++) {
		if (uri[i] == '/') {
			num_levels++;
		}
	}

	if (num_levels > MAX_MENU_LEVELS) {
		return FALSE;
	}

	return TRUE;
}

static gboolean
update_directory_in_templates_menu (FMDirectoryView *view,
				    const char *templates_directory_uri,
				    CajaDirectory *directory)
{
	char *menu_path, *popup_bg_path;
	GList *file_list, *filtered, *node;
	gboolean any_templates;
	CajaDirectory *dir;
	char *escaped_path;
	char *uri;
	int num;
	CajaFile *file = NULL;

	/* We know this directory belongs to the template dir, so it must exist */
	g_assert (templates_directory_uri);

	uri = caja_directory_get_uri (directory);
	escaped_path = escape_action_path (uri + strlen (templates_directory_uri));
	g_free (uri);
	menu_path = g_strconcat (FM_DIRECTORY_VIEW_MENU_PATH_NEW_DOCUMENTS_PLACEHOLDER,
				 escaped_path,
				 NULL);
	popup_bg_path = g_strconcat (FM_DIRECTORY_VIEW_POPUP_PATH_BACKGROUND_NEW_DOCUMENTS_PLACEHOLDER,
				     escaped_path,
				     NULL);
	g_free (escaped_path);

	file_list = caja_directory_get_file_list (directory);
	filtered = caja_file_list_filter_hidden (file_list, FALSE);
	caja_file_list_free (file_list);

	file_list = caja_file_list_sort_by_display_name (filtered);

	num = 0;
	any_templates = FALSE;
	for (node = file_list; num < TEMPLATE_LIMIT && node != NULL; node = node->next, num++) {
		file = node->data;

		if (caja_file_is_directory (file)) {
			uri = caja_file_get_uri (file);
			if (directory_belongs_in_templates_menu (templates_directory_uri, uri)) {
				dir = caja_directory_get_by_uri (uri);
				add_directory_to_templates_directory_list (view, dir);
				caja_directory_unref (dir);

				add_submenu_to_directory_menus (view,
								view->details->templates_action_group,
								view->details->templates_merge_id,
								file, menu_path, NULL, popup_bg_path);

				any_templates = TRUE;
			}
			g_free (uri);
		} else if (caja_file_can_read (file)) {
			add_template_to_templates_menus (view, file, menu_path, popup_bg_path);
			any_templates = TRUE;
		}
	}

	caja_file_list_free (file_list);

	g_free (popup_bg_path);
	g_free (menu_path);

	return any_templates;
}

static void
update_templates_menu (FMDirectoryView *view)
{
	gboolean any_templates;
	GList *sorted_copy, *node;
	GtkUIManager *ui_manager;
	GtkAction *action;
	char *templates_directory_uri;
	CajaDirectory *directory = NULL;

	if (caja_should_use_templates_directory ()) {
		templates_directory_uri = caja_get_templates_directory_uri ();
	} else {
		templates_directory_uri = NULL;
	}

	/* There is a race condition here.  If we don't mark the scripts menu as
	   valid before we begin our task then we can lose template menu updates that
	   occur before we finish. */
	view->details->templates_invalid = FALSE;

	ui_manager = caja_window_info_get_ui_manager (view->details->window);
	caja_ui_unmerge_ui (ui_manager,
				&view->details->templates_merge_id,
				&view->details->templates_action_group);

	caja_ui_prepare_merge_ui (ui_manager,
				      "TemplatesGroup",
				      &view->details->templates_merge_id,
				      &view->details->templates_action_group);

	/* As we walk through the directories, remove any that no longer belong. */
	any_templates = FALSE;
	sorted_copy = caja_directory_list_sort_by_uri
		(caja_directory_list_copy (view->details->templates_directory_list));
	for (node = sorted_copy; node != NULL; node = node->next) {
		char *uri;

		directory = node->data;

		uri = caja_directory_get_uri (directory);
		if (!directory_belongs_in_templates_menu (templates_directory_uri, uri)) {
			remove_directory_from_templates_directory_list (view, directory);
		} else if (update_directory_in_templates_menu (view,
							       templates_directory_uri,
							       directory)) {
			any_templates = TRUE;
		}
		g_free (uri);
	}
	caja_directory_list_free (sorted_copy);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group, FM_ACTION_NO_TEMPLATES);
	gtk_action_set_visible (action, !any_templates);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	g_free (templates_directory_uri);
}

static void
action_open_scripts_folder_callback (GtkAction *action,
				     gpointer callback_data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (callback_data);

	open_location (view, scripts_directory_uri, CAJA_WINDOW_OPEN_ACCORDING_TO_MODE, 0);

	eel_show_info_dialog_with_details
		(_("All executable files in this folder will appear in the "
		   "Scripts menu."),
		 _("Choosing a script from the menu will run "
		   "that script with any selected items as input."),
		 _("All executable files in this folder will appear in the "
		   "Scripts menu. Choosing a script from the menu will run "
		   "that script.\n\n"
		   "When executed from a local folder, scripts will be passed "
		   "the selected file names. When executed from a remote folder "
		   "(e.g. a folder showing web or ftp content), scripts will "
		   "be passed no parameters.\n\n"
		   "In all cases, the following environment variables will be "
		   "set by Caja, which the scripts may use:\n\n"
		   "CAJA_SCRIPT_SELECTED_FILE_PATHS: newline-delimited paths for selected files (only if local)\n\n"
		   "CAJA_SCRIPT_SELECTED_URIS: newline-delimited URIs for selected files\n\n"
		   "CAJA_SCRIPT_CURRENT_URI: URI for current location\n\n"
		   "CAJA_SCRIPT_WINDOW_GEOMETRY: position and size of current window\n\n"
		   "CAJA_SCRIPT_NEXT_PANE_SELECTED_FILE_PATHS: newline-delimited paths for selected files in the inactive pane of a split-view window (only if local)\n\n"
		   "CAJA_SCRIPT_NEXT_PANE_SELECTED_URIS: newline-delimited URIs for selected files in the inactive pane of a split-view window\n\n"
		   "CAJA_SCRIPT_NEXT_PANE_CURRENT_URI: URI for current location in the inactive pane of a split-view window"),
		 fm_directory_view_get_containing_window (view));
}

static GtkMenu *
create_popup_menu (FMDirectoryView *view, const char *popup_path)
{
	GtkWidget *menu;

	menu = gtk_ui_manager_get_widget (caja_window_info_get_ui_manager (view->details->window),
					  popup_path);
	gtk_menu_set_screen (GTK_MENU (menu),
			     gtk_widget_get_screen (GTK_WIDGET (view)));
	gtk_widget_show (GTK_WIDGET (menu));

	return GTK_MENU (menu);
}

static void
copy_or_cut_files (FMDirectoryView *view,
		   GList           *clipboard_contents,
		   gboolean         cut)
{
	int count;
	char *status_string;
	CajaClipboardInfo info;
        GtkTargetList *target_list;
        GtkTargetEntry *targets;
        int n_targets;

	info.files = clipboard_contents;
	info.cut = cut;

        target_list = gtk_target_list_new (NULL, 0);
        gtk_target_list_add (target_list, copied_files_atom, 0, 0);
        gtk_target_list_add_uri_targets (target_list, 0);
        gtk_target_list_add_text_targets (target_list, 0);

        targets = gtk_target_table_new_from_list (target_list, &n_targets);
        gtk_target_list_unref (target_list);

	gtk_clipboard_set_with_data (caja_clipboard_get (GTK_WIDGET (view)),
				     targets, n_targets,
				     caja_get_clipboard_callback, caja_clear_clipboard_callback,
				     NULL);
        gtk_target_table_free (targets, n_targets);

	caja_clipboard_monitor_set_clipboard_info (caja_clipboard_monitor_get (), &info);

	count = g_list_length (clipboard_contents);
	if (count == 1) {
		char *name;

		name = caja_file_get_display_name (clipboard_contents->data);
		if (cut) {
			status_string = g_strdup_printf (_("\"%s\" will be moved "
							   "if you select the Paste command"),
							 name);
		} else {
			status_string = g_strdup_printf (_("\"%s\" will be copied "
							   "if you select the Paste command"),
							 name);
		}
		g_free (name);
	} else {
		if (cut) {
			status_string = g_strdup_printf (ngettext("The %'d selected item will be moved "
								  "if you select the Paste command",
								  "The %'d selected items will be moved "
								  "if you select the Paste command",
								  count),
							 count);
		} else {
			status_string = g_strdup_printf (ngettext("The %'d selected item will be copied "
								  "if you select the Paste command",
								  "The %'d selected items will be copied "
								  "if you select the Paste command",
								  count),
							 count);
		}
	}

	caja_window_slot_info_set_status (view->details->slot,
					      status_string);
	g_free (status_string);
}

static void
action_copy_files_callback (GtkAction *action,
			    gpointer callback_data)
{
	FMDirectoryView *view;
	GList *selection;

	view = FM_DIRECTORY_VIEW (callback_data);

	selection = fm_directory_view_get_selection_for_file_transfer (view);
	copy_or_cut_files (view, selection, FALSE);
	caja_file_list_free (selection);
}

static void
move_copy_selection_to_location (FMDirectoryView *view,
				 int copy_action,
				 char *target_uri)
{
	GList *selection, *uris, *l;

	selection = fm_directory_view_get_selection_for_file_transfer (view);
	if (selection == NULL) {
		return;
	}

	uris = NULL;
	for (l = selection; l != NULL; l = l->next) {
		uris = g_list_prepend (uris,
				       caja_file_get_uri ((CajaFile *) l->data));
	}
	uris = g_list_reverse (uris);

	fm_directory_view_move_copy_items (uris, NULL, target_uri,
					   copy_action,
					   0, 0,
					   view);

	g_list_free_full (uris, g_free);
	caja_file_list_free (selection);
}

static void
move_copy_selection_to_next_pane (FMDirectoryView *view,
				  int copy_action)
{
	CajaWindowSlotInfo *slot;
	char *dest_location;

	slot = caja_window_info_get_extra_slot (fm_directory_view_get_caja_window (view));
	g_return_if_fail (slot != NULL);

	dest_location = caja_window_slot_info_get_current_location (slot);
	g_return_if_fail (dest_location != NULL);

	move_copy_selection_to_location (view, copy_action, dest_location);
}

static void
action_copy_to_next_pane_callback (GtkAction *action, gpointer callback_data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (callback_data);
	move_copy_selection_to_next_pane (view,
					  GDK_ACTION_COPY);
}

static void
action_move_to_next_pane_callback (GtkAction *action, gpointer callback_data)
{
	CajaWindowSlotInfo *slot;
	char *dest_location;
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (callback_data);

	slot = caja_window_info_get_extra_slot (fm_directory_view_get_caja_window (view));
	g_return_if_fail (slot != NULL);

	dest_location = caja_window_slot_info_get_current_location (slot);
	g_return_if_fail (dest_location != NULL);

	move_copy_selection_to_location (view, GDK_ACTION_MOVE, dest_location);
}

static void
action_copy_to_home_callback (GtkAction *action, gpointer callback_data)
{
	FMDirectoryView *view;
	char *dest_location;

	view = FM_DIRECTORY_VIEW (callback_data);

	dest_location = caja_get_home_directory_uri ();
	move_copy_selection_to_location (view, GDK_ACTION_COPY, dest_location);
	g_free (dest_location);
}

static void
action_move_to_home_callback (GtkAction *action, gpointer callback_data)
{
	FMDirectoryView *view;
	char *dest_location;

	view = FM_DIRECTORY_VIEW (callback_data);

	dest_location = caja_get_home_directory_uri ();
	move_copy_selection_to_location (view, GDK_ACTION_MOVE, dest_location);
	g_free (dest_location);
}

static void
action_copy_to_desktop_callback (GtkAction *action, gpointer callback_data)
{
	FMDirectoryView *view;
	char *dest_location;

	view = FM_DIRECTORY_VIEW (callback_data);

	dest_location = caja_get_desktop_directory_uri ();
	move_copy_selection_to_location (view, GDK_ACTION_COPY, dest_location);
	g_free (dest_location);
}

static void
action_move_to_desktop_callback (GtkAction *action, gpointer callback_data)
{
	FMDirectoryView *view;
	char *dest_location;

	view = FM_DIRECTORY_VIEW (callback_data);

	dest_location = caja_get_desktop_directory_uri ();
	move_copy_selection_to_location (view, GDK_ACTION_MOVE, dest_location);
	g_free (dest_location);
}

static void
action_cut_files_callback (GtkAction *action,
			   gpointer callback_data)
{
	FMDirectoryView *view;
	GList *selection;

	view = FM_DIRECTORY_VIEW (callback_data);

	selection = fm_directory_view_get_selection_for_file_transfer (view);
	copy_or_cut_files (view, selection, TRUE);
	caja_file_list_free (selection);
}

static void
paste_clipboard_data (FMDirectoryView *view,
		      GtkSelectionData *selection_data,
		      char *destination_uri)
{
	gboolean cut;
	GList *item_uris;

	cut = FALSE;
	item_uris = caja_clipboard_get_uri_list_from_selection_data (selection_data, &cut,
									 copied_files_atom);

	if (item_uris == NULL|| destination_uri == NULL) {
		caja_window_slot_info_set_status (view->details->slot,
						      _("There is nothing on the clipboard to paste."));
	} else {
		fm_directory_view_move_copy_items (item_uris, NULL, destination_uri,
						   cut ? GDK_ACTION_MOVE : GDK_ACTION_COPY,
						   0, 0,
						   view);

		/* If items are cut then remove from clipboard */
		if (cut) {
			gtk_clipboard_clear (caja_clipboard_get (GTK_WIDGET (view)));
		}

    		g_list_free_full (item_uris, g_free);
	}
}

static void
paste_clipboard_received_callback (GtkClipboard     *clipboard,
				   GtkSelectionData *selection_data,
				   gpointer          data)
{
	FMDirectoryView *view;
	char *view_uri;

	view = FM_DIRECTORY_VIEW (data);

	view_uri = fm_directory_view_get_backing_uri (view);

	if (view->details->window != NULL) {
		paste_clipboard_data (view, selection_data, view_uri);
	}

	g_free (view_uri);

	g_object_unref (view);
}

typedef struct {
	FMDirectoryView *view;
	CajaFile *target;
} PasteIntoData;

static void
paste_into_clipboard_received_callback (GtkClipboard     *clipboard,
					GtkSelectionData *selection_data,
					gpointer          callback_data)
{
	PasteIntoData *data;
	FMDirectoryView *view;

	data = (PasteIntoData *) callback_data;

	view = FM_DIRECTORY_VIEW (data->view);

	if (view->details->window != NULL) {
		char *directory_uri;

		directory_uri = caja_file_get_activation_uri (data->target);

		paste_clipboard_data (view, selection_data, directory_uri);

		g_free (directory_uri);
	}

	g_object_unref (view);
	caja_file_unref (data->target);
	g_free (data);
}

static void
action_paste_files_callback (GtkAction *action,
			     gpointer callback_data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (callback_data);

	g_object_ref (view);
	gtk_clipboard_request_contents (caja_clipboard_get (GTK_WIDGET (view)),
					copied_files_atom,
					paste_clipboard_received_callback,
					view);
}

static void
paste_into (FMDirectoryView *view,
	    CajaFile *target)
{
	PasteIntoData *data;

	g_assert (FM_IS_DIRECTORY_VIEW (view));
	g_assert (CAJA_IS_FILE (target));

	data = g_new (PasteIntoData, 1);

	data->view = g_object_ref (view);
	data->target = caja_file_ref (target);

	gtk_clipboard_request_contents (caja_clipboard_get (GTK_WIDGET (view)),
					copied_files_atom,
					paste_into_clipboard_received_callback,
					data);
}

static void
action_paste_files_into_callback (GtkAction *action,
				  gpointer callback_data)
{
	FMDirectoryView *view;
	GList *selection;

	view = FM_DIRECTORY_VIEW (callback_data);
	selection = fm_directory_view_get_selection (view);
	if (selection != NULL) {
		paste_into (view, CAJA_FILE (selection->data));
		caja_file_list_free (selection);
	}

}

static void
real_action_undo (FMDirectoryView *view)
{
	CajaUndoStackManager *manager = caja_undostack_manager_instance ();

	/* Disable menus because they are in an untrustworthy status */
	view->details->undo_active = FALSE;
	view->details->redo_active = FALSE;
	fm_directory_view_update_menus (view);

	caja_undostack_manager_undo (manager, GTK_WIDGET (view), finish_undoredo_callback);
}

static void
real_action_redo (FMDirectoryView *view)
{
	CajaUndoStackManager *manager = caja_undostack_manager_instance ();

	/* Disable menus because they are in an untrustworthy status */
	view->details->undo_active = FALSE;
	view->details->redo_active = FALSE;
	fm_directory_view_update_menus (view);

	caja_undostack_manager_redo (manager, GTK_WIDGET (view), finish_undoredo_callback);
}

static void
real_action_rename (FMDirectoryView *view,
		    gboolean select_all)
{
	GList *selection;

	g_assert (FM_IS_DIRECTORY_VIEW (view));

	selection = fm_directory_view_get_selection (view);

	if (selection_not_empty_in_menu_callback (view, selection)) {
		CajaFile *file;

		file = CAJA_FILE (selection->data);

		if (!select_all) {
			/* directories don't have a file extension, so
			 * they are always pre-selected as a whole */
			select_all = caja_file_is_directory (file);
		}
		EEL_CALL_METHOD (FM_DIRECTORY_VIEW_CLASS, view, start_renaming_file, (view, file, select_all));
	}

	caja_file_list_free (selection);
}

static void
action_rename_callback (GtkAction *action,
			gpointer callback_data)
{
	real_action_rename (FM_DIRECTORY_VIEW (callback_data), FALSE);
}

static void
action_rename_select_all_callback (GtkAction *action,
				   gpointer callback_data)
{
	real_action_rename (FM_DIRECTORY_VIEW (callback_data), TRUE);
}

static void
file_mount_callback (CajaFile  *file,
		     GFile         *result_location,
		     GError        *error,
		     gpointer       callback_data)
{
	if (error != NULL &&
	    (error->domain != G_IO_ERROR ||
	     (error->code != G_IO_ERROR_CANCELLED &&
	      error->code != G_IO_ERROR_FAILED_HANDLED &&
	      error->code != G_IO_ERROR_ALREADY_MOUNTED))) {
		eel_show_error_dialog (_("Unable to mount location"),
				       error->message, NULL);
	}
}

static void
file_unmount_callback (CajaFile  *file,
		       GFile         *result_location,
		       GError        *error,
		       gpointer       callback_data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (callback_data);
	fm_directory_view_set_initiated_unmount (view, FALSE);
	g_object_unref (view);

	if (error != NULL &&
	    (error->domain != G_IO_ERROR ||
	     (error->code != G_IO_ERROR_CANCELLED &&
	      error->code != G_IO_ERROR_FAILED_HANDLED))) {
		eel_show_error_dialog (_("Unable to unmount location"),
				       error->message, NULL);
	}
}

static void
file_eject_callback (CajaFile  *file,
		     GFile         *result_location,
		     GError        *error,
		     gpointer       callback_data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (callback_data);
	fm_directory_view_set_initiated_unmount (view, FALSE);
	g_object_unref (view);

	if (error != NULL &&
	    (error->domain != G_IO_ERROR ||
	     (error->code != G_IO_ERROR_CANCELLED &&
	      error->code != G_IO_ERROR_FAILED_HANDLED))) {
		eel_show_error_dialog (_("Unable to eject location"),
				       error->message, NULL);
	}
}

static void
file_stop_callback (CajaFile  *file,
		    GFile         *result_location,
		    GError        *error,
		    gpointer       callback_data)
{
	if (error != NULL &&
	    (error->domain != G_IO_ERROR ||
	     (error->code != G_IO_ERROR_CANCELLED &&
	      error->code != G_IO_ERROR_FAILED_HANDLED))) {
		eel_show_error_dialog (_("Unable to stop drive"),
				       error->message, NULL);
	}
}

static void
action_mount_volume_callback (GtkAction *action,
			      gpointer data)
{
	GList *selection, *l;
	FMDirectoryView *view;
	GMountOperation *mount_op;
	CajaFile *file = NULL;

        view = FM_DIRECTORY_VIEW (data);

	selection = fm_directory_view_get_selection (view);
	for (l = selection; l != NULL; l = l->next) {
		file = CAJA_FILE (l->data);

		if (caja_file_can_mount (file)) {
			mount_op = gtk_mount_operation_new (fm_directory_view_get_containing_window (view));
			g_mount_operation_set_password_save (mount_op, G_PASSWORD_SAVE_FOR_SESSION);
			caja_file_mount (file, mount_op, NULL,
					     file_mount_callback, NULL);
			g_object_unref (mount_op);
		}
	}
	caja_file_list_free (selection);
}

static void
action_unmount_volume_callback (GtkAction *action,
				gpointer data)
{
	GList *selection, *l;
	FMDirectoryView *view;
	CajaFile *file = NULL;

        view = FM_DIRECTORY_VIEW (data);

	selection = fm_directory_view_get_selection (view);

	for (l = selection; l != NULL; l = l->next) {
		file = CAJA_FILE (l->data);
		if (caja_file_can_unmount (file)) {
			GMountOperation *mount_op;
			mount_op = gtk_mount_operation_new (fm_directory_view_get_containing_window (view));
			fm_directory_view_set_initiated_unmount (view, TRUE);
			caja_file_unmount (file, mount_op, NULL,
					       file_unmount_callback, g_object_ref (view));
			g_object_unref (mount_op);
		}
	}
	caja_file_list_free (selection);
}

static void
action_format_volume_callback (GtkAction *action,
			       gpointer   data)
{
#ifdef TODO_GIO
	GList *selection, *l;
	FMDirectoryView *view;
	CajaFile *file = NULL;

        view = FM_DIRECTORY_VIEW (data);

	selection = fm_directory_view_get_selection (view);
	for (l = selection; l != NULL; l = l->next) {
		file = CAJA_FILE (l->data);

		if (something) {
			g_spawn_command_line_async ("gfloppy", NULL);
		}
	}
	caja_file_list_free (selection);
#endif
}

static void
action_eject_volume_callback (GtkAction *action,
			      gpointer data)
{
	GList *selection, *l;
	FMDirectoryView *view;
	CajaFile *file = NULL;

        view = FM_DIRECTORY_VIEW (data);

	selection = fm_directory_view_get_selection (view);
	for (l = selection; l != NULL; l = l->next) {
		file = CAJA_FILE (l->data);

		if (caja_file_can_eject (file)) {
			GMountOperation *mount_op;
			mount_op = gtk_mount_operation_new (fm_directory_view_get_containing_window (view));
			fm_directory_view_set_initiated_unmount (view, TRUE);
			caja_file_eject (file, mount_op, NULL,
					     file_eject_callback, g_object_ref (view));
			g_object_unref (mount_op);
		}
	}
	caja_file_list_free (selection);
}

static void
file_start_callback (CajaFile  *file,
		     GFile         *result_location,
		     GError        *error,
		     gpointer       callback_data)
{
	if (error != NULL &&
	    (error->domain != G_IO_ERROR ||
	     (error->code != G_IO_ERROR_CANCELLED &&
	      error->code != G_IO_ERROR_FAILED_HANDLED &&
	      error->code != G_IO_ERROR_ALREADY_MOUNTED))) {
		eel_show_error_dialog (_("Unable to start location"),
				       error->message, NULL);
	}
}

static void
action_start_volume_callback (GtkAction *action,
			      gpointer   data)
{
	GList *selection, *l;
	FMDirectoryView *view;
	GMountOperation *mount_op;
	CajaFile *file = NULL;

        view = FM_DIRECTORY_VIEW (data);

	selection = fm_directory_view_get_selection (view);
	for (l = selection; l != NULL; l = l->next) {
		file = CAJA_FILE (l->data);

		if (caja_file_can_start (file) || caja_file_can_start_degraded (file)) {
			mount_op = gtk_mount_operation_new (fm_directory_view_get_containing_window (view));
			caja_file_start (file, mount_op, NULL,
					     file_start_callback, NULL);
			g_object_unref (mount_op);
		}
	}
	caja_file_list_free (selection);
}

static void
action_stop_volume_callback (GtkAction *action,
			     gpointer   data)
{
	GList *selection, *l;
	FMDirectoryView *view;
	CajaFile *file = NULL;

        view = FM_DIRECTORY_VIEW (data);

	selection = fm_directory_view_get_selection (view);
	for (l = selection; l != NULL; l = l->next) {
		file = CAJA_FILE (l->data);

		if (caja_file_can_stop (file)) {
			GMountOperation *mount_op;
			mount_op = gtk_mount_operation_new (fm_directory_view_get_containing_window (view));
			caja_file_stop (file, mount_op, NULL,
					    file_stop_callback, NULL);
			g_object_unref (mount_op);
		}
	}
	caja_file_list_free (selection);
}

static void
action_detect_media_callback (GtkAction *action,
			      gpointer   data)
{
	GList *selection, *l;
	FMDirectoryView *view;
	CajaFile *file = NULL;

        view = FM_DIRECTORY_VIEW (data);

	selection = fm_directory_view_get_selection (view);
	for (l = selection; l != NULL; l = l->next) {
		file = CAJA_FILE (l->data);

		if (caja_file_can_poll_for_media (file) && !caja_file_is_media_check_automatic (file)) {
			caja_file_poll_for_media (file);
		}
	}
	caja_file_list_free (selection);
}

static void
action_self_mount_volume_callback (GtkAction *action,
				   gpointer data)
{
	CajaFile *file;
	FMDirectoryView *view;
	GMountOperation *mount_op;

	view = FM_DIRECTORY_VIEW (data);

	file = fm_directory_view_get_directory_as_file (view);
	if (file == NULL) {
		return;
	}

	mount_op = gtk_mount_operation_new (fm_directory_view_get_containing_window (view));
	g_mount_operation_set_password_save (mount_op, G_PASSWORD_SAVE_FOR_SESSION);
	caja_file_mount (file, mount_op, NULL, file_mount_callback, NULL);
	g_object_unref (mount_op);
}

static void
action_self_unmount_volume_callback (GtkAction *action,
				     gpointer data)
{
	CajaFile *file;
	FMDirectoryView *view;
	GMountOperation *mount_op;

	view = FM_DIRECTORY_VIEW (data);

	file = fm_directory_view_get_directory_as_file (view);
	if (file == NULL) {
		return;
	}

	mount_op = gtk_mount_operation_new (fm_directory_view_get_containing_window (view));
	fm_directory_view_set_initiated_unmount (view, TRUE);
	caja_file_unmount (file, mount_op, NULL, file_unmount_callback, g_object_ref (view));
	g_object_unref (mount_op);
}

static void
action_self_eject_volume_callback (GtkAction *action,
				   gpointer data)
{
	CajaFile *file;
	FMDirectoryView *view;
	GMountOperation *mount_op;

	view = FM_DIRECTORY_VIEW (data);

	file = fm_directory_view_get_directory_as_file (view);
	if (file == NULL) {
		return;
	}

	mount_op = gtk_mount_operation_new (fm_directory_view_get_containing_window (view));
	fm_directory_view_set_initiated_unmount (view, TRUE);
	caja_file_eject (file, mount_op, NULL, file_eject_callback, g_object_ref (view));
	g_object_unref (mount_op);
}

static void
action_self_format_volume_callback (GtkAction *action,
				    gpointer   data)
{
	CajaFile *file;
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (data);

	file = fm_directory_view_get_directory_as_file (view);
	if (file == NULL) {
		return;
	}

#ifdef TODO_GIO
	if (something) {
		g_spawn_command_line_async ("gfloppy", NULL);
	}
#endif
}

static void
action_self_start_volume_callback (GtkAction *action,
				   gpointer   data)
{
	CajaFile *file;
	FMDirectoryView *view;
	GMountOperation *mount_op;

	view = FM_DIRECTORY_VIEW (data);

	file = fm_directory_view_get_directory_as_file (view);
	if (file == NULL) {
		return;
	}

	mount_op = gtk_mount_operation_new (fm_directory_view_get_containing_window (view));
	caja_file_start (file, mount_op, NULL, file_start_callback, NULL);
	g_object_unref (mount_op);
}

static void
action_self_stop_volume_callback (GtkAction *action,
				  gpointer   data)
{
	CajaFile *file;
	FMDirectoryView *view;
	GMountOperation *mount_op;

	view = FM_DIRECTORY_VIEW (data);

	file = fm_directory_view_get_directory_as_file (view);
	if (file == NULL) {
		return;
	}

	mount_op = gtk_mount_operation_new (fm_directory_view_get_containing_window (view));
	caja_file_stop (file, mount_op, NULL,
			    file_stop_callback, NULL);
	g_object_unref (mount_op);
}

static void
action_self_detect_media_callback (GtkAction *action,
				   gpointer   data)
{
	CajaFile *file;
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (data);

	file = fm_directory_view_get_directory_as_file (view);
	if (file == NULL) {
		return;
	}

	caja_file_poll_for_media (file);
}

static void
action_location_mount_volume_callback (GtkAction *action,
				       gpointer data)
{
	CajaFile *file;
	FMDirectoryView *view;
	GMountOperation *mount_op;

	view = FM_DIRECTORY_VIEW (data);

	file = view->details->location_popup_directory_as_file;
	if (file == NULL) {
		return;
	}

	mount_op = gtk_mount_operation_new (fm_directory_view_get_containing_window (view));
	g_mount_operation_set_password_save (mount_op, G_PASSWORD_SAVE_FOR_SESSION);
	caja_file_mount (file, mount_op, NULL, file_mount_callback, NULL);
	g_object_unref (mount_op);
}

static void
action_location_unmount_volume_callback (GtkAction *action,
					 gpointer data)
{
	CajaFile *file;
	FMDirectoryView *view;
	GMountOperation *mount_op;

	view = FM_DIRECTORY_VIEW (data);

	file = view->details->location_popup_directory_as_file;
	if (file == NULL) {
		return;
	}

	mount_op = gtk_mount_operation_new (fm_directory_view_get_containing_window (view));
	fm_directory_view_set_initiated_unmount (view, TRUE);
	caja_file_unmount (file, mount_op, NULL,
			       file_unmount_callback, g_object_ref (view));
	g_object_unref (mount_op);
}

static void
action_location_eject_volume_callback (GtkAction *action,
				       gpointer data)
{
	CajaFile *file;
	FMDirectoryView *view;
	GMountOperation *mount_op;

	view = FM_DIRECTORY_VIEW (data);

	file = view->details->location_popup_directory_as_file;
	if (file == NULL) {
		return;
	}

	mount_op = gtk_mount_operation_new (fm_directory_view_get_containing_window (view));
	fm_directory_view_set_initiated_unmount (view, TRUE);
	caja_file_eject (file, mount_op, NULL,
			     file_eject_callback, g_object_ref (view));
	g_object_unref (mount_op);
}

static void
action_location_format_volume_callback (GtkAction *action,
					gpointer   data)
{
	CajaFile *file;
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (data);

	file = view->details->location_popup_directory_as_file;
	if (file == NULL) {
		return;
	}

#ifdef TODO_GIO
	if (something) {
		g_spawn_command_line_async ("gfloppy", NULL);
	}
#endif
}

static void
action_location_start_volume_callback (GtkAction *action,
				       gpointer   data)
{
	CajaFile *file;
	FMDirectoryView *view;
	GMountOperation *mount_op;

	view = FM_DIRECTORY_VIEW (data);

	file = view->details->location_popup_directory_as_file;
	if (file == NULL) {
		return;
	}

	mount_op = gtk_mount_operation_new (fm_directory_view_get_containing_window (view));
	caja_file_start (file, mount_op, NULL, file_start_callback, NULL);
	g_object_unref (mount_op);
}

static void
action_location_stop_volume_callback (GtkAction *action,
				      gpointer   data)
{
	CajaFile *file;
	FMDirectoryView *view;
	GMountOperation *mount_op;

	view = FM_DIRECTORY_VIEW (data);

	file = view->details->location_popup_directory_as_file;
	if (file == NULL) {
		return;
	}

	mount_op = gtk_mount_operation_new (fm_directory_view_get_containing_window (view));
	caja_file_stop (file, mount_op, NULL,
			    file_stop_callback, NULL);
	g_object_unref (mount_op);
}

static void
action_location_detect_media_callback (GtkAction *action,
				       gpointer   data)
{
	CajaFile *file;
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (data);

	file = view->details->location_popup_directory_as_file;
	if (file == NULL) {
		return;
	}

	caja_file_poll_for_media (file);
}

static void
connect_to_server_response_callback (GtkDialog *dialog,
				     int response_id,
				     gpointer data)
{
#ifdef GIO_CONVERSION_DONE
	GtkEntry *entry;
	char *uri;
	const char *name;
	char *icon;

	entry = GTK_ENTRY (data);

	switch (response_id) {
	case GTK_RESPONSE_OK:
		uri = g_object_get_data (G_OBJECT (dialog), "link-uri");
		icon = g_object_get_data (G_OBJECT (dialog), "link-icon");
		name = gtk_entry_get_text (entry);
		mate_vfs_connect_to_server (uri, (char *)name, icon);
		gtk_widget_destroy (GTK_WIDGET (dialog));
		break;
	case GTK_RESPONSE_NONE:
	case GTK_RESPONSE_DELETE_EVENT:
	case GTK_RESPONSE_CANCEL:
		gtk_widget_destroy (GTK_WIDGET (dialog));
		break;
	default :
		g_assert_not_reached ();
	}
#endif
	/* FIXME: the above code should make a server connection permanent */
	gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
entry_activate_callback (GtkEntry *entry,
			 gpointer user_data)
{
	GtkDialog *dialog;

	dialog = GTK_DIALOG (user_data);
	gtk_dialog_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
}

static void
action_connect_to_server_link_callback (GtkAction *action,
					gpointer data)
{
	CajaFile *file;
	GList *selection;
	FMDirectoryView *view;
	char *uri;
	CajaIconInfo *icon;
	const char *icon_name;
	char *name;
	gint scale;

        view = FM_DIRECTORY_VIEW (data);

	selection = fm_directory_view_get_selection (view);

	if (!eel_g_list_exactly_one_item (selection)) {
		caja_file_list_free (selection);
		return;
	}

	file = CAJA_FILE (selection->data);
	scale = gtk_widget_get_scale_factor (GTK_WIDGET (view));

	uri = caja_file_get_activation_uri (file);
	icon = caja_file_get_icon (file, CAJA_ICON_SIZE_STANDARD, scale, 0);
	icon_name = caja_icon_info_get_used_name (icon);
	name = caja_file_get_display_name (file);

	if (uri != NULL) {
		GtkWidget *dialog;
		GtkWidget *label;
		GtkWidget *entry;
		GtkWidget *box;
		char *title;

		title = g_strdup_printf (_("Connect to Server %s"), name);

		dialog = gtk_dialog_new ();
		gtk_window_set_title (GTK_WINDOW (dialog), title);
		gtk_window_set_transient_for (GTK_WINDOW (dialog), fm_directory_view_get_containing_window (view));

		eel_dialog_add_button (GTK_DIALOG (dialog),
				       _("_Cancel"),
				       "process-stop",
				       GTK_RESPONSE_CANCEL);

		gtk_dialog_add_button (GTK_DIALOG (dialog),
				       _("_Connect"),
				       GTK_RESPONSE_OK);

		g_object_set_data_full (G_OBJECT (dialog), "link-uri", g_strdup (uri), g_free);
		g_object_set_data_full (G_OBJECT (dialog), "link-icon", g_strdup (icon_name), g_free);

		gtk_container_set_border_width (GTK_CONTAINER (dialog), 5);
		gtk_box_set_spacing (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))), 2);

		box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
		gtk_widget_show (box);
		gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))),
				    box, TRUE, TRUE, 0);

		label = gtk_label_new_with_mnemonic (_("Link _name:"));
		gtk_widget_show (label);

		gtk_box_pack_start (GTK_BOX (box), label, TRUE, TRUE, 12);

		entry = gtk_entry_new ();
		if (name) {
			gtk_entry_set_text (GTK_ENTRY (entry), name);
		}
		g_signal_connect (entry,
				  "activate",
				  G_CALLBACK (entry_activate_callback),
				  dialog);

		gtk_widget_show (entry);
		gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);

		gtk_box_pack_start (GTK_BOX (box), entry, TRUE, TRUE, 12);

		gtk_dialog_set_default_response (GTK_DIALOG (dialog),
						 GTK_RESPONSE_OK);
		g_signal_connect (dialog, "response",
				  G_CALLBACK (connect_to_server_response_callback),
				  entry);
		gtk_widget_show (dialog);
	}

	g_free (uri);
	g_object_unref (icon);
	g_free (name);
}

static void
action_location_open_alternate_callback (GtkAction *action,
					 gpointer   callback_data)
{
	FMDirectoryView *view;
	CajaFile *file;

	view = FM_DIRECTORY_VIEW (callback_data);

	file = view->details->location_popup_directory_as_file;
	if (file == NULL) {
		return;
	}

	fm_directory_view_activate_file (view,
					 file,
					 CAJA_WINDOW_OPEN_IN_NAVIGATION,
					 0);
}

static void
action_location_open_in_new_tab_callback (GtkAction *action,
					  gpointer   callback_data)
{
	FMDirectoryView *view;
	CajaFile *file;

	view = FM_DIRECTORY_VIEW (callback_data);

	file = view->details->location_popup_directory_as_file;
	if (file == NULL) {
		return;
	}

	fm_directory_view_activate_file (view,
					 file,
					 CAJA_WINDOW_OPEN_ACCORDING_TO_MODE,
					 CAJA_WINDOW_OPEN_FLAG_NEW_TAB);
}

static void
action_location_open_folder_window_callback (GtkAction *action,
					     gpointer   callback_data)
{
	FMDirectoryView *view;
	CajaFile *file;

	view = FM_DIRECTORY_VIEW (callback_data);

	file = view->details->location_popup_directory_as_file;
	g_return_if_fail (file != NULL);

	fm_directory_view_activate_file (view,
					 file,
					 CAJA_WINDOW_OPEN_IN_SPATIAL,
					 0);
}

static void
action_location_cut_callback (GtkAction *action,
			      gpointer   callback_data)
{
	FMDirectoryView *view;
	CajaFile *file;
	GList *files;

	view = FM_DIRECTORY_VIEW (callback_data);

	file = view->details->location_popup_directory_as_file;
	g_return_if_fail (file != NULL);

	files = g_list_append (NULL, file);
	copy_or_cut_files (view, files, TRUE);
	g_list_free (files);
}

static void
action_location_copy_callback (GtkAction *action,
			       gpointer   callback_data)
{
	FMDirectoryView *view;
	CajaFile *file;
	GList *files;

	view = FM_DIRECTORY_VIEW (callback_data);

	file = view->details->location_popup_directory_as_file;
	g_return_if_fail (file != NULL);

	files = g_list_append (NULL, file);
	copy_or_cut_files (view, files, FALSE);
	g_list_free (files);
}

static void
action_location_paste_files_into_callback (GtkAction *action,
					   gpointer callback_data)
{
	FMDirectoryView *view;
	CajaFile *file;

	view = FM_DIRECTORY_VIEW (callback_data);

	file = view->details->location_popup_directory_as_file;
	g_return_if_fail (file != NULL);

	paste_into (view, file);
}

static void
action_location_trash_callback (GtkAction *action,
				gpointer   callback_data)
{
	FMDirectoryView *view;
	CajaFile *file;
	GList *files;

	view = FM_DIRECTORY_VIEW (callback_data);

	file = view->details->location_popup_directory_as_file;
	g_return_if_fail (file != NULL);

	files = g_list_append (NULL, file);
	trash_or_delete_files (fm_directory_view_get_containing_window (view),
			       files, TRUE,
			       view);
	g_list_free (files);
}

static void
action_location_delete_callback (GtkAction *action,
				 gpointer   callback_data)
{
	FMDirectoryView *view;
	CajaFile *file;
	GFile *location;
	GList *files;

	view = FM_DIRECTORY_VIEW (callback_data);

	file = view->details->location_popup_directory_as_file;
	g_return_if_fail (file != NULL);

	location = caja_file_get_location (file);

	files = g_list_append (NULL, location);
	caja_file_operations_delete (files, fm_directory_view_get_containing_window (view),
					 NULL, NULL);

	g_list_free_full (files, g_object_unref);
}

static void
action_location_restore_from_trash_callback (GtkAction *action,
					     gpointer callback_data)
{
	FMDirectoryView *view;
	CajaFile *file;
	GList l;

	view = FM_DIRECTORY_VIEW (callback_data);
	file = view->details->location_popup_directory_as_file;

	l.prev = NULL;
	l.next = NULL;
	l.data = file;
	caja_restore_files_from_trash (&l,
					   fm_directory_view_get_containing_window (view));
}

static void
fm_directory_view_init_show_hidden_files (FMDirectoryView *view)
{
	CajaWindowShowHiddenFilesMode mode;
	gboolean show_hidden_changed;
	gboolean show_hidden_default_setting;

	if (view->details->ignore_hidden_file_preferences) {
		return;
	}

	show_hidden_changed = FALSE;
	mode = caja_window_info_get_hidden_files_mode (view->details->window);

	if (mode == CAJA_WINDOW_SHOW_HIDDEN_FILES_DEFAULT) {
		show_hidden_default_setting = g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_SHOW_HIDDEN_FILES);
		if (show_hidden_default_setting != view->details->show_hidden_files) {
			view->details->show_hidden_files = show_hidden_default_setting;
			show_hidden_changed = TRUE;
		}
	} else {
		if (mode == CAJA_WINDOW_SHOW_HIDDEN_FILES_ENABLE) {
			show_hidden_changed = !view->details->show_hidden_files;
			view->details->show_hidden_files = TRUE;
		} else {
			show_hidden_changed = view->details->show_hidden_files;
			view->details->show_hidden_files = FALSE;
		}
	}

	if (show_hidden_changed && (view->details->model != NULL)) {
		load_directory (view, view->details->model);
	}

}

static void
fm_directory_view_init_show_backup_files (FMDirectoryView *view)
{
	CajaWindowShowBackupFilesMode mode;
	gboolean show_backup_changed;
	gboolean show_backup_default_setting;

	show_backup_changed = FALSE;
	mode = caja_window_info_get_backup_files_mode (view->details->window);

	if (mode == CAJA_WINDOW_SHOW_BACKUP_FILES_DEFAULT) {
		show_backup_default_setting = g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_SHOW_BACKUP_FILES);
		if (show_backup_default_setting != view->details->show_backup_files) {
			view->details->show_backup_files = show_backup_default_setting;
			show_backup_changed = TRUE;
		}
	} else {
		if (mode == CAJA_WINDOW_SHOW_BACKUP_FILES_ENABLE) {
			show_backup_changed = !view->details->show_backup_files;
			view->details->show_backup_files = TRUE;
		} else {
			show_backup_changed = view->details->show_backup_files;
			view->details->show_backup_files = FALSE;
		}
	}

	if (show_backup_changed && (view->details->model != NULL)) {
		load_directory (view, view->details->model);
	}

}

static const GtkActionEntry directory_view_entries[] = {
  /* name, icon name, label */ { "New Documents", "document-new", N_("Create _Document"),
                                 NULL, NULL, NULL },
  /* name, icon name, label */ { "Open With", NULL, N_("Open Wit_h"),
                                 NULL, N_("Choose a program with which to open the selected item"), NULL },
  /* name, icon name */        { "Properties", "document-properties",
  /* label, accelerator */       N_("_Properties"), "<alt>Return",
  /* tooltip */                  N_("View or modify the properties of each selected item"),
                                 G_CALLBACK (action_properties_callback) },
  /* name, icon name */        { "PropertiesAccel", NULL,
  /* label, accelerator */       "PropertiesAccel", "<control>I",
  /* tooltip */                  NULL,
                                 G_CALLBACK (action_properties_callback) },
  /* name, icon name */        { "New Folder", "folder-new",
  /* label, accelerator */       N_("Create _Folder"), "<control><shift>N",
  /* tooltip */                  N_("Create a new empty folder inside this folder"),
                                 G_CALLBACK (action_new_folder_callback) },
  /* name, icon name, label */ { "No Templates", NULL, N_("No templates installed"), NULL, NULL, NULL },
  /* name, icon name */        { "New Empty File", NULL,
  /* Translators: this is used to indicate that a file doesn't contain anything */
  /* label, accelerator */       N_("_Empty File"), NULL,
  /* tooltip */                  N_("Create a new empty file inside this folder"),
                                 G_CALLBACK (action_new_empty_file_callback) },
  /* name, icon name */        { "New Launcher", NULL,
  /* label, accelerator */       N_("Create L_auncher..."), NULL,
  /* tooltip */                  N_("Create a new launcher"),
                                 G_CALLBACK (action_new_launcher_callback) },
  /* name, icon name */        { "Open", NULL,
  /* label, accelerator */       N_("_Open"), "<control>o",
  /* tooltip */                  N_("Open the selected item in this window"),
                                 G_CALLBACK (action_open_callback) },
  /* name, icon name */        { "OpenAccel", NULL,
  /* label, accelerator */       "OpenAccel", "<alt>Down",
  /* tooltip */                  NULL,
                                 G_CALLBACK (action_open_callback) },
  /* name, icon name */        { "OpenAlternate", NULL,
  /* label, accelerator */       N_("Open in Navigation Window"), "<control><shift>w",
  /* tooltip */                  N_("Open each selected item in a navigation window"),
                                 G_CALLBACK (action_open_alternate_callback) },
  /* name, icon name */        { "OpenInNewTab", NULL,
  /* label, accelerator */       N_("Open in New _Tab"), "<control><shift>o",
  /* tooltip */                  N_("Open each selected item in a new tab"),
                                 G_CALLBACK (action_open_new_tab_callback) },
  /* name, icon name */        { "OpenFolderWindow", NULL,
  /* label, accelerator */       N_("Open in _Folder Window"), NULL,
  /* tooltip */                  N_("Open each selected item in a folder window"),
                                 G_CALLBACK (action_open_folder_window_callback) },
  /* name, icon name */        { "OtherApplication1", NULL,
  /* label, accelerator */       N_("Other _Application..."), NULL,
  /* tooltip */                  N_("Choose another application with which to open the selected item"),
                                 G_CALLBACK (action_other_application_callback) },
  /* name, icon name */        { "OtherApplication2", NULL,
  /* label, accelerator */       N_("Open With Other _Application..."), NULL,
  /* tooltip */                  N_("Choose another application with which to open the selected item"),
                                 G_CALLBACK (action_other_application_callback) },
  /* name, icon name */        { "Open Scripts Folder", NULL,
  /* label, accelerator */       N_("_Open Scripts Folder"), NULL,
  /* tooltip */                  N_("Show the folder containing the scripts that appear in this menu"),
                                 G_CALLBACK (action_open_scripts_folder_callback) },
  /* name, icon name */        { "Empty Trash", CAJA_ICON_TRASH,
  /* label, accelerator */       N_("E_mpty Trash"), NULL,
  /* tooltip */                  N_("Delete all items in the Trash"),
                                 G_CALLBACK (action_empty_trash_callback) },
  /* name, icon name */        { "Cut", "edit-cut",
  /* label, accelerator */       N_("Cu_t"), "<control>X",
  /* tooltip */                  N_("Prepare the selected files to be moved with a Paste command"),
                                 G_CALLBACK (action_cut_files_callback) },
  /* name, icon name */        { "Copy", "edit-copy",
  /* label, accelerator */       N_("_Copy"), "<control>C",
  /* tooltip */                  N_("Prepare the selected files to be copied with a Paste command"),
                                 G_CALLBACK (action_copy_files_callback) },
  /* name, icon name */        { "Paste", "edit-paste",
  /* label, accelerator */       N_("_Paste"), "<control>V",
  /* tooltip */                  N_("Move or copy files previously selected by a Cut or Copy command"),
                                 G_CALLBACK (action_paste_files_callback) },
  /* We make accelerator "" instead of null here to not inherit the stock
     accelerator for paste */
  /* name, icon name */        { "Paste Files Into", "edit-paste",
  /* label, accelerator */       N_("_Paste Into Folder"), "",
  /* tooltip */                  N_("Move or copy files previously selected by a Cut or Copy command into the selected folder"),
                                 G_CALLBACK (action_paste_files_into_callback) },
  /* name, icon name, label */ { "CopyToMenu", NULL, N_("Cop_y to"),
                                 NULL, NULL, NULL },
  /* name, icon name, label */ { "MoveToMenu", NULL, N_("M_ove to"),
                                 NULL, NULL, NULL },
  /* name, icon name */        { "Select All", NULL,
  /* label, accelerator */       N_("Select _All"), "<control>A",
  /* tooltip */                  N_("Select all items in this window"),
                                 G_CALLBACK (action_select_all_callback) },
  /* name, icon name */        { "Select Pattern", NULL,
  /* label, accelerator */       N_("Select I_tems Matching..."), "<control>S",
  /* tooltip */                  N_("Select items in this window matching a given pattern"),
                                 G_CALLBACK (action_select_pattern_callback) },
  /* name, icon name */        { "Invert Selection", NULL,
  /* label, accelerator */       N_("_Invert Selection"), "<control><shift>I",
  /* tooltip */                  N_("Select all and only the items that are not currently selected"),
                                 G_CALLBACK (action_invert_selection_callback) },
  /* name, icon name */        { "Duplicate", NULL,
  /* label, accelerator */       N_("D_uplicate"), NULL,
  /* tooltip */                  N_("Duplicate each selected item"),
                                 G_CALLBACK (action_duplicate_callback) },
  /* name, icon name */        { "Create Link", NULL,
  /* label, accelerator */       N_("Ma_ke Link"), "<control>M",
  /* tooltip */                  N_("Create a symbolic link for each selected item"),
                                 G_CALLBACK (action_create_link_callback) },
  /* name, icon name */        { "Rename", NULL,
  /* label, accelerator */       N_("_Rename..."), "F2",
  /* tooltip */                  N_("Rename selected item"),
                                 G_CALLBACK (action_rename_callback) },
  /* name, icon name */        { "RenameSelectAll", NULL,
  /* label, accelerator */       "RenameSelectAll", "<shift>F2",
  /* tooltip */                  NULL,
                                 G_CALLBACK (action_rename_select_all_callback) },
  /* name, icon name */        { "Trash", NULL,
  /* label, accelerator */       N_("Mo_ve to Trash"), NULL,
  /* tooltip */                  N_("Move each selected item to the Trash"),
                                 G_CALLBACK (action_trash_callback) },
  /* name, icon name */        { "Delete", NULL,
  /* label, accelerator */       N_("_Delete"), NULL,
  /* tooltip */                  N_("Delete each selected item, without moving to the Trash"),
                                 G_CALLBACK (action_delete_callback) },
  /* name, icon name */        { "Restore From Trash", NULL,
  /* label, accelerator */       N_("_Restore"), NULL,
				 NULL,
                                 G_CALLBACK (action_restore_from_trash_callback) },
  /* name, icon name */        { FM_ACTION_UNDO, "edit-undo",
  /* label, accelerator */       N_("_Undo"), "<control>Z",
  /* tooltip */                  N_("Undo the last action"),
                                 G_CALLBACK (action_undo_callback) },
  /* name, icon name */        { FM_ACTION_REDO, "edit-redo",
  /* label, accelerator */       N_("_Redo"), "<control>Y",
  /* tooltip */                  N_("Redo the last undone action"),
                                 G_CALLBACK (action_redo_callback) },

  /*
   * multiview-TODO: decide whether "Reset to Defaults" should
   * be window-wide, and not just view-wide.
   * Since this also resets the "Show hidden files" mode,
   * it is a mixture of both ATM.
   */
  /* name, icon name */        { "Reset to Defaults", NULL,
  /* label, accelerator */       N_("Reset View to _Defaults"), NULL,
  /* tooltip */                  N_("Reset sorting order and zoom level to match preferences for this view"),
                                 G_CALLBACK (action_reset_to_defaults_callback) },
  /* name, icon name */        { "Connect To Server Link", NULL,
  /* label, accelerator */       N_("Connect To This Server"), NULL,
  /* tooltip */                  N_("Make a permanent connection to this server"),
                                 G_CALLBACK (action_connect_to_server_link_callback) },
  /* name, icon name */        { "Mount Volume", NULL,
  /* label, accelerator */       N_("_Mount"), NULL,
  /* tooltip */                  N_("Mount the selected volume"),
                                 G_CALLBACK (action_mount_volume_callback) },
  /* name, icon name */        { "Unmount Volume", NULL,
  /* label, accelerator */       N_("_Unmount"), NULL,
  /* tooltip */                  N_("Unmount the selected volume"),
                                 G_CALLBACK (action_unmount_volume_callback) },
  /* name, icon name */        { "Eject Volume", NULL,
  /* label, accelerator */       N_("_Eject"), NULL,
  /* tooltip */                  N_("Eject the selected volume"),
                                 G_CALLBACK (action_eject_volume_callback) },
  /* name, icon name */        { "Format Volume", NULL,
  /* label, accelerator */       N_("_Format"), NULL,
  /* tooltip */                  N_("Format the selected volume"),
                                 G_CALLBACK (action_format_volume_callback) },
  /* name, icon name */        { "Start Volume", NULL,
  /* label, accelerator */       N_("_Start"), NULL,
  /* tooltip */                  N_("Start the selected volume"),
                                 G_CALLBACK (action_start_volume_callback) },
  /* name, icon name */        { "Stop Volume", NULL,
  /* label, accelerator */       N_("_Stop"), NULL,
  /* tooltip */                  N_("Stop the selected volume"),
                                 G_CALLBACK (action_stop_volume_callback) },
  /* name, icon name */        { "Poll", NULL,
  /* label, accelerator */       N_("_Detect Media"), NULL,
  /* tooltip */                  N_("Detect media in the selected drive"),
                                 G_CALLBACK (action_detect_media_callback) },
  /* name, icon name */        { "Self Mount Volume", NULL,
  /* label, accelerator */       N_("_Mount"), NULL,
  /* tooltip */                  N_("Mount the volume associated with the open folder"),
                                 G_CALLBACK (action_self_mount_volume_callback) },
  /* name, icon name */        { "Self Unmount Volume", NULL,
  /* label, accelerator */       N_("_Unmount"), NULL,
  /* tooltip */                  N_("Unmount the volume associated with the open folder"),
                                 G_CALLBACK (action_self_unmount_volume_callback) },
  /* name, icon name */        { "Self Eject Volume", NULL,
  /* label, accelerator */       N_("_Eject"), NULL,
  /* tooltip */                  N_("Eject the volume associated with the open folder"),
                                 G_CALLBACK (action_self_eject_volume_callback) },
  /* name, icon name */        { "Self Format Volume", NULL,
  /* label, accelerator */       N_("_Format"), NULL,
  /* tooltip */                  N_("Format the volume associated with the open folder"),
                                 G_CALLBACK (action_self_format_volume_callback) },
  /* name, icon name */        { "Self Start Volume", NULL,
  /* label, accelerator */       N_("_Start"), NULL,
  /* tooltip */                  N_("Start the volume associated with the open folder"),
                                 G_CALLBACK (action_self_start_volume_callback) },
  /* name, icon name */        { "Self Stop Volume", NULL,
  /* label, accelerator */       N_("_Stop"), NULL,
  /* tooltip */                  N_("Stop the volume associated with the open folder"),
                                 G_CALLBACK (action_self_stop_volume_callback) },
  /* name, icon name */        { "Self Poll", NULL,
  /* label, accelerator */       N_("_Detect Media"), NULL,
  /* tooltip */                  N_("Detect media in the selected drive"),
                                 G_CALLBACK (action_self_detect_media_callback) },
  /* name, icon name */        { "OpenCloseParent", NULL,
  /* label, accelerator */       N_("Open File and Close window"), "<alt><shift>Down",
  /* tooltip */                  NULL,
                                 G_CALLBACK (action_open_close_parent_callback) },
  /* name, icon name */        { "Save Search", NULL,
  /* label, accelerator */       N_("Sa_ve Search"), NULL,
  /* tooltip */                  N_("Save the edited search"),
                                 G_CALLBACK (action_save_search_callback) },
  /* name, icon name */        { "Save Search As", NULL,
  /* label, accelerator */       N_("Sa_ve Search As..."), NULL,
  /* tooltip */                  N_("Save the current search as a file"),
                                 G_CALLBACK (action_save_search_as_callback) },

  /* Location-specific actions */
  /* name, icon name */        { FM_ACTION_LOCATION_OPEN_ALTERNATE, NULL,
  /* label, accelerator */       N_("Open in Navigation Window"), "",
  /* tooltip */                  N_("Open this folder in a navigation window"),
                                 G_CALLBACK (action_location_open_alternate_callback) },
  /* name, icon name */        { FM_ACTION_LOCATION_OPEN_IN_NEW_TAB, NULL,
  /* label, accelerator */       N_("Open in New _Tab"), "",
  /* tooltip */                  N_("Open this folder in a new tab"),
                                 G_CALLBACK (action_location_open_in_new_tab_callback) },

  /* name, icon name */        { FM_ACTION_LOCATION_OPEN_FOLDER_WINDOW, NULL,
  /* label, accelerator */       N_("Open in _Folder Window"), "",
  /* tooltip */                  N_("Open this folder in a folder window"),
                                 G_CALLBACK (action_location_open_folder_window_callback) },

  /* name, icon name */        { FM_ACTION_LOCATION_CUT, "edit-cut",
  /* label, accelerator */       N_("Cu_t"), "",
  /* tooltip */                  N_("Prepare this folder to be moved with a Paste command"),
                                 G_CALLBACK (action_location_cut_callback) },
  /* name, icon name */        { FM_ACTION_LOCATION_COPY, "edit-copy",
  /* label, accelerator */       N_("_Copy"), "",
  /* tooltip */                  N_("Prepare this folder to be copied with a Paste command"),
                                 G_CALLBACK (action_location_copy_callback) },
  /* name, icon name */        { FM_ACTION_LOCATION_PASTE_FILES_INTO, "edit-paste",
  /* label, accelerator */       N_("_Paste Into Folder"), "",
  /* tooltip */                  N_("Move or copy files previously selected by a Cut or Copy command into this folder"),
                                 G_CALLBACK (action_location_paste_files_into_callback) },

  /* name, icon name */        { FM_ACTION_LOCATION_TRASH, NULL,
  /* label, accelerator */       N_("Mo_ve to Trash"), "",
  /* tooltip */                  N_("Move this folder to the Trash"),
                                 G_CALLBACK (action_location_trash_callback) },
  /* name, icon name */        { FM_ACTION_LOCATION_DELETE, CAJA_ICON_DELETE,
  /* label, accelerator */       N_("_Delete"), "",
  /* tooltip */                  N_("Delete this folder, without moving to the Trash"),
                                 G_CALLBACK (action_location_delete_callback) },
  /* name, icon name */        { FM_ACTION_LOCATION_RESTORE_FROM_TRASH, NULL,
  /* label, accelerator */       N_("_Restore"), NULL, NULL,
                                 G_CALLBACK (action_location_restore_from_trash_callback) },

  /* name, icon name */        { "Location Mount Volume", NULL,
  /* label, accelerator */       N_("_Mount"), NULL,
  /* tooltip */                  N_("Mount the volume associated with this folder"),
                                 G_CALLBACK (action_location_mount_volume_callback) },
  /* name, icon name */        { "Location Unmount Volume", NULL,
  /* label, accelerator */       N_("_Unmount"), NULL,
  /* tooltip */                  N_("Unmount the volume associated with this folder"),
                                 G_CALLBACK (action_location_unmount_volume_callback) },
  /* name, icon name */        { "Location Eject Volume", NULL,
  /* label, accelerator */       N_("_Eject"), NULL,
  /* tooltip */                  N_("Eject the volume associated with this folder"),
                                 G_CALLBACK (action_location_eject_volume_callback) },
  /* name, icon name */        { "Location Format Volume", NULL,
  /* label, accelerator */       N_("_Format"), NULL,
  /* tooltip */                  N_("Format the volume associated with this folder"),
                                 G_CALLBACK (action_location_format_volume_callback) },
  /* name, icon name */        { "Location Start Volume", NULL,
  /* label, accelerator */       N_("_Start"), NULL,
  /* tooltip */                  N_("Start the volume associated with this folder"),
                                 G_CALLBACK (action_location_start_volume_callback) },
  /* name, icon name */        { "Location Stop Volume", NULL,
  /* label, accelerator */       N_("_Stop"), NULL,
  /* tooltip */                  N_("Stop the volume associated with this folder"),
                                 G_CALLBACK (action_location_stop_volume_callback) },
  /* name, icon name */        { "Location Poll", NULL,
  /* label, accelerator */       N_("_Detect Media"), NULL,
  /* tooltip */                  N_("Detect media in the selected drive"),
                                 G_CALLBACK (action_location_detect_media_callback) },

  /* name, icon name */        { "LocationProperties", "document-properties",
  /* label, accelerator */       N_("_Properties"), NULL,
  /* tooltip */                  N_("View or modify the properties of this folder"),
                                 G_CALLBACK (action_location_properties_callback) },

  /* name, icon name, label */ {FM_ACTION_COPY_TO_NEXT_PANE, NULL, N_("_Other pane"),
				NULL, N_("Copy the current selection to the other pane in the window"),
				G_CALLBACK (action_copy_to_next_pane_callback) },
  /* name, icon name, label */ {FM_ACTION_MOVE_TO_NEXT_PANE, NULL, N_("_Other pane"),
				NULL, N_("Move the current selection to the other pane in the window"),
				G_CALLBACK (action_move_to_next_pane_callback) },
  /* name, icon name, label */ {FM_ACTION_COPY_TO_HOME, CAJA_ICON_HOME,
				N_("_Home Folder"), NULL,
				N_("Copy the current selection to the home folder"),
				G_CALLBACK (action_copy_to_home_callback) },
  /* name, icon name, label */ {FM_ACTION_MOVE_TO_HOME, CAJA_ICON_HOME,
				N_("_Home Folder"), NULL,
				N_("Move the current selection to the home folder"),
				G_CALLBACK (action_move_to_home_callback) },
  /* name, icon name, label */ {FM_ACTION_COPY_TO_DESKTOP, CAJA_ICON_DESKTOP,
				N_("_Desktop"), NULL,
				N_("Copy the current selection to the desktop"),
				G_CALLBACK (action_copy_to_desktop_callback) },
  /* name, icon name, label */ {FM_ACTION_MOVE_TO_DESKTOP, CAJA_ICON_DESKTOP,
				N_("_Desktop"), NULL,
				N_("Move the current selection to the desktop"),
				G_CALLBACK (action_move_to_desktop_callback) },
};

static void
connect_proxy (FMDirectoryView *view,
	       GtkAction *action,
	       GtkWidget *proxy,
	       GtkActionGroup *action_group)
{
	const gchar *action_name;

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action_name = gtk_action_get_name (action);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	if (strcmp (action_name, FM_ACTION_NEW_EMPTY_FILE) == 0 &&
	    GTK_IS_IMAGE_MENU_ITEM (proxy)) {
		cairo_surface_t *surface;

		surface = get_menu_icon ("text-x-generic", GTK_WIDGET (view));

		if (surface != NULL) {
			GtkWidget *image;

			image = gtk_image_new_from_surface (surface);
			gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (proxy), image);

			cairo_surface_destroy (surface);
		}
	}
}

static void
pre_activate (FMDirectoryView *view,
	      GtkAction *action,
	      GtkActionGroup *action_group)
{
	GdkEvent *event;
	GtkWidget *proxy;
	gboolean activated_from_popup;

	/* check whether action was activated through a popup menu.
	 * If not, unset the last stored context menu popup position */
	activated_from_popup = FALSE;

	event = gtk_get_current_event ();
	proxy = gtk_get_event_widget (event);

	if (proxy != NULL) {
		GtkWidget *toplevel;
		GdkWindowTypeHint hint;

		toplevel = gtk_widget_get_toplevel (proxy);

		if (GTK_IS_WINDOW (toplevel)) {
			hint = gtk_window_get_type_hint (GTK_WINDOW (toplevel));

			if (hint == GDK_WINDOW_TYPE_HINT_POPUP_MENU) {
				activated_from_popup = TRUE;
			}
		}
	}

	if (!activated_from_popup) {
		update_context_menu_position_from_event (view, NULL);
	}
}

static void
real_merge_menus (FMDirectoryView *view)
{
	GtkActionGroup *action_group;
	GtkUIManager *ui_manager;
	GtkAction *action;
	const char *ui;
	char *tooltip;

	ui_manager = caja_window_info_get_ui_manager (view->details->window);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action_group = gtk_action_group_new ("DirViewActions");
#ifdef ENABLE_NLS
	gtk_action_group_set_translation_domain (action_group, GETTEXT_PACKAGE);
#endif /* ENABLE_NLS */
	view->details->dir_action_group = action_group;
	gtk_action_group_add_actions (action_group,
				      directory_view_entries, G_N_ELEMENTS (directory_view_entries),
				      view);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	/* Translators: %s is a directory */
	tooltip = g_strdup_printf(_("Run or manage scripts from %s"), "~/.config/caja/scripts");
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	/* Create a script action here specially because its tooltip is dynamic */
	action = gtk_action_new ("Scripts", _("_Scripts"), tooltip, NULL);
	gtk_action_group_add_action (action_group, action);
	g_object_unref (action);
	g_free (tooltip);

	action = gtk_action_group_get_action (action_group, FM_ACTION_NO_TEMPLATES);
	gtk_action_set_sensitive (action, FALSE);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	g_signal_connect_object (action_group, "connect-proxy",
				 G_CALLBACK (connect_proxy), G_OBJECT (view),
				 G_CONNECT_SWAPPED);
	g_signal_connect_object (action_group, "pre-activate",
				 G_CALLBACK (pre_activate), G_OBJECT (view),
				 G_CONNECT_SWAPPED);

	/* Insert action group at end so clipboard action group ends up before it */
	gtk_ui_manager_insert_action_group (ui_manager, action_group, -1);
	g_object_unref (action_group); /* owned by ui manager */

	ui = caja_ui_string_get ("caja-directory-view-ui.xml");
	view->details->dir_merge_id = gtk_ui_manager_add_ui_from_string (ui_manager, ui, -1, NULL);
	g_signal_connect_object (fm_directory_view_get_background (view), "settings_changed",
				 G_CALLBACK (schedule_update_menus), G_OBJECT (view),
				 G_CONNECT_SWAPPED);
	view->details->scripts_invalid = TRUE;
	view->details->templates_invalid = TRUE;
}

static gboolean
can_paste_into_file (CajaFile *file)
{
	if (caja_file_is_directory (file) &&
	    caja_file_can_write (file)) {
		return TRUE;
	}
	if (caja_file_has_activation_uri (file)) {
		GFile *location;
		CajaFile *activation_file;
		gboolean res;

		location = caja_file_get_activation_location (file);
		activation_file = caja_file_get (location);
		g_object_unref (location);

		/* The target location might not have data for it read yet,
		   and we can't want to do sync I/O, so treat the unknown
		   case as can-write */
		res = (caja_file_get_file_type (activation_file) == G_FILE_TYPE_UNKNOWN) ||
			(caja_file_get_file_type (activation_file) == G_FILE_TYPE_DIRECTORY &&
			 caja_file_can_write (activation_file));

		caja_file_unref (activation_file);

		return res;
	}

	return FALSE;
}

static void
clipboard_targets_received (GtkClipboard     *clipboard,
                            GdkAtom          *targets,
                            int               n_targets,
			    gpointer          user_data)
{
	FMDirectoryView *view;
	gboolean can_paste;
	GList *selection;
	int count;
	GtkAction *action;

	view = FM_DIRECTORY_VIEW (user_data);
	can_paste = FALSE;

	if (view->details->window == NULL ||
	    !view->details->active) {
		/* We've been destroyed or became inactive since call */
		g_object_unref (view);
		return;
	}

	if (targets) {
		int i;

		for (i=0; i < n_targets; i++) {
			if (targets[i] == copied_files_atom) {
				can_paste = TRUE;
			}
		}
	}

	selection = fm_directory_view_get_selection (view);
	count = g_list_length (selection);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_PASTE);
	gtk_action_set_sensitive (action,
				  can_paste && !fm_directory_view_is_read_only (view));

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_PASTE_FILES_INTO);
	gtk_action_set_sensitive (action,
	                          can_paste && count == 1 &&
	                          can_paste_into_file (CAJA_FILE (selection->data)));

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_PASTE_FILES_INTO);
	G_GNUC_END_IGNORE_DEPRECATIONS;
	g_object_set_data (G_OBJECT (action),
			   "can-paste-according-to-clipboard",
			   GINT_TO_POINTER (can_paste));
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	gtk_action_set_sensitive (action,
				  GPOINTER_TO_INT (g_object_get_data (G_OBJECT (action),
						   "can-paste-according-to-clipboard")) &&
				  GPOINTER_TO_INT (g_object_get_data (G_OBJECT (action),
						   "can-paste-according-to-destination")));
	G_GNUC_END_IGNORE_DEPRECATIONS;

	caja_file_list_free (selection);

	g_object_unref (view);
}

static gboolean
showing_trash_directory (FMDirectoryView *view)
{
	CajaFile *file;

	file = fm_directory_view_get_directory_as_file (view);
	if (file != NULL) {
		return caja_file_is_in_trash (file);
	}
	return FALSE;
}

static gboolean
should_show_empty_trash (FMDirectoryView *view)
{
	return (showing_trash_directory (view) || caja_window_info_get_window_type (view->details->window) == CAJA_WINDOW_NAVIGATION);
}

static gboolean
file_list_all_are_folders (GList *file_list)
{
	GList *l;
	char *activation_uri;
	gboolean is_dir;
	CajaFile *linked_file;
	CajaFile *file = NULL;

	for (l = file_list; l != NULL; l = l->next) {
		file = CAJA_FILE (l->data);
		if (caja_file_is_caja_link (file) &&
		    !CAJA_IS_DESKTOP_ICON_FILE (file)) {
			if (caja_file_is_launcher (file)) {
				return FALSE;
			}

			activation_uri = caja_file_get_activation_uri (file);

			if (activation_uri == NULL) {
				g_free (activation_uri);
				return FALSE;
			}

			linked_file = caja_file_get_existing_by_uri (activation_uri);

			/* We might not actually know the type of the linked file yet,
			 * however we don't want to schedule a read, since that might do things
			 * like ask for password etc. This is a bit unfortunate, but I don't
			 * know any way around it, so we do various heuristics here
			 * to get things mostly right
			 */
			is_dir =
				(linked_file != NULL &&
				 caja_file_is_directory (linked_file)) ||
				(activation_uri != NULL &&
				 activation_uri[strlen (activation_uri) - 1] == '/');

			caja_file_unref (linked_file);
			g_free (activation_uri);

			if (!is_dir) {
				return FALSE;
			}
		} else if (!(caja_file_is_directory (file) ||
			     CAJA_IS_DESKTOP_ICON_FILE (file))) {
			return FALSE;
		}
	}
	return TRUE;
}

static void
file_should_show_foreach (CajaFile        *file,
			  gboolean            *show_mount,
			  gboolean            *show_unmount,
			  gboolean            *show_eject,
			  gboolean            *show_connect,
			  gboolean            *show_format,
			  gboolean            *show_start,
			  gboolean            *show_stop,
			  gboolean            *show_poll,
			  GDriveStartStopType *start_stop_type)
{
	*show_mount = FALSE;
	*show_unmount = FALSE;
	*show_eject = FALSE;
	*show_connect = FALSE;
	*show_format = FALSE;
	*show_start = FALSE;
	*show_stop = FALSE;
	*show_poll = FALSE;

	if (caja_file_can_eject (file)) {
		*show_eject = TRUE;
	}

	if (caja_file_can_mount (file)) {
		*show_mount = TRUE;

#ifdef TODO_GIO
		if (something &&
		    g_find_program_in_path ("gfloppy")) {
			*show_format = TRUE;
		}
#endif
	}

	if (caja_file_can_start (file) || caja_file_can_start_degraded (file)) {
		*show_start = TRUE;
	}

	if (caja_file_can_stop (file)) {
		*show_stop = TRUE;
	}

	/* Dot not show both Unmount and Eject/Safe Removal; too confusing to
	 * have too many menu entries */
	if (caja_file_can_unmount (file) && !*show_eject && !*show_stop) {
		*show_unmount = TRUE;
	}

	if (caja_file_can_poll_for_media (file) && !caja_file_is_media_check_automatic (file)) {
		*show_poll = TRUE;
	}

	*start_stop_type = caja_file_get_start_stop_type (file);

	if (caja_file_is_caja_link (file)) {
		char *uri;

		uri = caja_file_get_activation_uri (file);
		if (uri != NULL &&
		    (eel_istr_has_prefix (uri, "ftp:") ||
		     eel_istr_has_prefix (uri, "ssh:") ||
		     eel_istr_has_prefix (uri, "sftp:") ||
		     eel_istr_has_prefix (uri, "dav:") ||
		     eel_istr_has_prefix (uri, "davs:"))) {
			*show_connect = TRUE;
		}
		g_free (uri);
	}
}

static void
file_should_show_self (CajaFile        *file,
		       gboolean            *show_mount,
		       gboolean            *show_unmount,
		       gboolean            *show_eject,
		       gboolean            *show_format,
		       gboolean            *show_start,
		       gboolean            *show_stop,
		       gboolean            *show_poll,
		       GDriveStartStopType *start_stop_type)
{
	*show_mount = FALSE;
	*show_unmount = FALSE;
	*show_eject = FALSE;
	*show_format = FALSE;
	*show_start = FALSE;
	*show_stop = FALSE;
	*show_poll = FALSE;

	if (file == NULL) {
		return;
	}

	if (caja_file_can_eject (file)) {
		*show_eject = TRUE;
	}

	if (caja_file_can_mount (file)) {
		*show_mount = TRUE;
	}

#ifdef TODO_GIO
	if (something && g_find_program_in_path ("gfloppy")) {
		*show_format = TRUE;
	}
#endif

	if (caja_file_can_start (file) || caja_file_can_start_degraded (file)) {
		*show_start = TRUE;
	}

	if (caja_file_can_stop (file)) {
		*show_stop = TRUE;
	}

	/* Dot not show both Unmount and Eject/Safe Removal; too confusing to
	 * have too many menu entries */
	if (caja_file_can_unmount (file) && !*show_eject && !*show_stop) {
		*show_unmount = TRUE;
	}

	if (caja_file_can_poll_for_media (file) && !caja_file_is_media_check_automatic (file)) {
		*show_poll = TRUE;
	}

	*start_stop_type = caja_file_get_start_stop_type (file);

}

static gboolean
files_are_all_directories (GList *files)
{
	GList *l;
	gboolean all_directories;
	CajaFile *file = NULL;

	all_directories = TRUE;

	for (l = files; l != NULL; l = l->next) {
		file = CAJA_FILE (l->data);
		all_directories &= caja_file_is_directory (file);
	}

	return all_directories;
}

static gboolean
files_is_none_directory (GList *files)
{
	GList *l;
	gboolean no_directory;
	CajaFile *file = NULL;

	no_directory = TRUE;

	for (l = files; l != NULL; l = l->next) {
		file = CAJA_FILE (l->data);
		no_directory &= !caja_file_is_directory (file);
	}

	return no_directory;
}

static void
update_restore_from_trash_action (GtkAction *action,
				  GList *files,
				  gboolean is_self)
{
	CajaFile *original_file;
	CajaFile *original_dir;
	GHashTable *original_dirs_hash;
	GList *original_dirs;
	GFile *original_location;
	char *original_name;

	original_file = NULL;
	original_dir = NULL;
	original_dirs = NULL;
	original_dirs_hash = NULL;
	original_location = NULL;
	original_name = NULL;

	if (files != NULL) {
		if (g_list_length (files) == 1) {
			original_file = caja_file_get_trash_original_file (files->data);
		} else {
			original_dirs_hash = caja_trashed_files_get_original_directories (files, NULL);
			if (original_dirs_hash != NULL) {
				original_dirs = g_hash_table_get_keys (original_dirs_hash);
				if (g_list_length (original_dirs) == 1) {
					original_dir = caja_file_ref (CAJA_FILE (original_dirs->data));
				}
			}
		}
	}

	if (original_file != NULL || original_dirs != NULL) {
		char *tooltip;

		G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
		gtk_action_set_visible (action, TRUE);
		G_GNUC_END_IGNORE_DEPRECATIONS;

		if (original_file != NULL) {
			original_location = caja_file_get_location (original_file);
		} else if (original_dir != NULL) {
			original_location = caja_file_get_location (original_dir);
		}

		if (original_location != NULL) {
			original_name = g_file_get_parse_name (original_location);
		}

		if (is_self) {
			g_assert (g_list_length (files) == 1);
			g_assert (original_location != NULL);
			tooltip = g_strdup_printf (_("Move the open folder out of the trash to \"%s\""), original_name);
		} else if (files_are_all_directories (files)) {
			if (original_name != NULL) {
				tooltip = g_strdup_printf (ngettext ("Move the selected folder out of the trash to \"%s\"",
								     "Move the selected folders out of the trash to \"%s\"",
								     g_list_length (files)), original_name);
			} else {
				tooltip = g_strdup_printf (ngettext ("Move the selected folder out of the trash",
								     "Move the selected folders out of the trash",
								     g_list_length (files)));
			}
		} else if (files_is_none_directory (files)) {
			if (original_name != NULL) {
				tooltip = g_strdup_printf (ngettext ("Move the selected file out of the trash to \"%s\"",
								     "Move the selected files out of the trash to \"%s\"",
								     g_list_length (files)), original_name);
			} else {
				tooltip = g_strdup_printf (ngettext ("Move the selected file out of the trash",
								     "Move the selected files out of the trash",
								     g_list_length (files)));
			}
		} else {
			if (original_name != NULL) {
				tooltip = g_strdup_printf (ngettext ("Move the selected item out of the trash to \"%s\"",
								     "Move the selected items out of the trash to \"%s\"",
								     g_list_length (files)), original_name);
			} else {
				tooltip = g_strdup_printf (ngettext ("Move the selected item out of the trash",
								     "Move the selected items out of the trash",
								     g_list_length (files)));
			}
		}
		g_free (original_name);

		g_object_set (action, "tooltip", tooltip, NULL);

		if (original_location != NULL) {
			g_object_unref (original_location);
		}
	} else {
		G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
		gtk_action_set_visible (action, FALSE);
		G_GNUC_END_IGNORE_DEPRECATIONS;
	}

	caja_file_unref (original_file);
	caja_file_unref (original_dir);
	g_list_free (original_dirs);

	if (original_dirs_hash != NULL) {
		g_hash_table_destroy (original_dirs_hash);
	}
}

static void
real_update_menus_volumes (FMDirectoryView *view,
			   GList *selection,
			   gint selection_count)
{
	GList *l;
	CajaFile *file;
	gboolean show_mount;
	gboolean show_unmount;
	gboolean show_eject;
	gboolean show_connect;
	gboolean show_format;
	gboolean show_start;
	gboolean show_stop;
	gboolean show_poll;
	GDriveStartStopType start_stop_type;
	gboolean show_self_mount;
	gboolean show_self_unmount;
	gboolean show_self_eject;
	gboolean show_self_format;
	gboolean show_self_start;
	gboolean show_self_stop;
	gboolean show_self_poll;
	GDriveStartStopType self_start_stop_type;
	GtkAction *action;

	show_mount = (selection != NULL);
	show_unmount = (selection != NULL);
	show_eject = (selection != NULL);
	show_connect = (selection != NULL && selection_count == 1);
	show_format = (selection != NULL && selection_count == 1);
	show_start = (selection != NULL && selection_count == 1);
	show_stop = (selection != NULL && selection_count == 1);
	show_poll = (selection != NULL && selection_count == 1);
	start_stop_type = G_DRIVE_START_STOP_TYPE_UNKNOWN;
	self_start_stop_type = G_DRIVE_START_STOP_TYPE_UNKNOWN;

	for (l = selection; l != NULL && (show_mount || show_unmount
					  || show_eject || show_connect
                                          || show_format || show_start
					  || show_stop || show_poll);
	     l = l->next) {
		gboolean show_mount_one;
		gboolean show_unmount_one;
		gboolean show_eject_one;
		gboolean show_connect_one;
		gboolean show_format_one;
		gboolean show_start_one;
		gboolean show_stop_one;
		gboolean show_poll_one;

		file = CAJA_FILE (l->data);
		file_should_show_foreach (file,
					  &show_mount_one,
					  &show_unmount_one,
					  &show_eject_one,
					  &show_connect_one,
                                          &show_format_one,
                                          &show_start_one,
                                          &show_stop_one,
					  &show_poll_one,
					  &start_stop_type);

		show_mount &= show_mount_one;
		show_unmount &= show_unmount_one;
		show_eject &= show_eject_one;
		show_connect &= show_connect_one;
		show_format &= show_format_one;
		show_start &= show_start_one;
		show_stop &= show_stop_one;
		show_poll &= show_poll_one;
	}

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_CONNECT_TO_SERVER_LINK);
	gtk_action_set_visible (action, show_connect);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_MOUNT_VOLUME);
	gtk_action_set_visible (action, show_mount);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_UNMOUNT_VOLUME);
	gtk_action_set_visible (action, show_unmount);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_EJECT_VOLUME);
	gtk_action_set_visible (action, show_eject);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_FORMAT_VOLUME);
	gtk_action_set_visible (action, show_format);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_START_VOLUME);
	gtk_action_set_visible (action, show_start);
	if (show_start) {
		switch (start_stop_type) {
		default:
		case G_DRIVE_START_STOP_TYPE_UNKNOWN:
			gtk_action_set_label (action, _("_Start"));
			gtk_action_set_tooltip (action, _("Start the selected drive"));
			break;
		case G_DRIVE_START_STOP_TYPE_SHUTDOWN:
			gtk_action_set_label (action, _("_Start"));
			gtk_action_set_tooltip (action, _("Start the selected drive"));
			break;
		case G_DRIVE_START_STOP_TYPE_NETWORK:
			gtk_action_set_label (action, _("_Connect"));
			gtk_action_set_tooltip (action, _("Connect to the selected drive"));
			break;
		case G_DRIVE_START_STOP_TYPE_MULTIDISK:
			gtk_action_set_label (action, _("_Start Multi-disk Drive"));
			gtk_action_set_tooltip (action, _("Start the selected multi-disk drive"));
			break;
		case G_DRIVE_START_STOP_TYPE_PASSWORD:
			gtk_action_set_label (action, _("U_nlock Drive"));
			gtk_action_set_tooltip (action, _("Unlock the selected drive"));
			break;
		}
	}

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_STOP_VOLUME);
	gtk_action_set_visible (action, show_stop);
	if (show_stop) {
		switch (start_stop_type) {
		default:
		case G_DRIVE_START_STOP_TYPE_UNKNOWN:
			gtk_action_set_label (action, _("_Stop"));
			gtk_action_set_tooltip (action, _("Stop the selected drive"));
			break;
		case G_DRIVE_START_STOP_TYPE_SHUTDOWN:
			gtk_action_set_label (action, _("_Safely Remove Drive"));
			gtk_action_set_tooltip (action, _("Safely remove the selected drive"));
			break;
		case G_DRIVE_START_STOP_TYPE_NETWORK:
			gtk_action_set_label (action, _("_Disconnect"));
			gtk_action_set_tooltip (action, _("Disconnect the selected drive"));
			break;
		case G_DRIVE_START_STOP_TYPE_MULTIDISK:
			gtk_action_set_label (action, _("_Stop Multi-disk Drive"));
			gtk_action_set_tooltip (action, _("Stop the selected multi-disk drive"));
			break;
		case G_DRIVE_START_STOP_TYPE_PASSWORD:
			gtk_action_set_label (action, _("_Lock Drive"));
			gtk_action_set_tooltip (action, _("Lock the selected drive"));
			break;
		}
	}

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_POLL);
	gtk_action_set_visible (action, show_poll);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	show_self_mount = show_self_unmount = show_self_eject =
		show_self_format = show_self_start = show_self_stop = show_self_poll = FALSE;

	file = fm_directory_view_get_directory_as_file (view);
	file_should_show_self (file,
			       &show_self_mount,
			       &show_self_unmount,
			       &show_self_eject,
			       &show_self_format,
			       &show_self_start,
			       &show_self_stop,
			       &show_self_poll,
			       &self_start_stop_type);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_SELF_MOUNT_VOLUME);
	gtk_action_set_visible (action, show_self_mount);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_SELF_UNMOUNT_VOLUME);
	gtk_action_set_visible (action, show_self_unmount);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_SELF_EJECT_VOLUME);
	gtk_action_set_visible (action, show_self_eject);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_SELF_FORMAT_VOLUME);
	gtk_action_set_visible (action, show_self_format);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_SELF_START_VOLUME);
	gtk_action_set_visible (action, show_self_start);
	if (show_self_start) {
		switch (self_start_stop_type) {
		default:
		case G_DRIVE_START_STOP_TYPE_UNKNOWN:
			gtk_action_set_label (action, _("_Start"));
			gtk_action_set_tooltip (action, _("Start the drive associated with the open folder"));
			break;
		case G_DRIVE_START_STOP_TYPE_SHUTDOWN:
			gtk_action_set_label (action, _("_Start"));
			gtk_action_set_tooltip (action, _("Start the drive associated with the open folder"));
			break;
		case G_DRIVE_START_STOP_TYPE_NETWORK:
			gtk_action_set_label (action, _("_Connect"));
			gtk_action_set_tooltip (action, _("Connect to the drive associated with the open folder"));
			break;
		case G_DRIVE_START_STOP_TYPE_MULTIDISK:
			gtk_action_set_label (action, _("_Start Multi-disk Drive"));
			gtk_action_set_tooltip (action, _("Start the multi-disk drive associated with the open folder"));
			break;
		case G_DRIVE_START_STOP_TYPE_PASSWORD:
			gtk_action_set_label (action, _("_Unlock Drive"));
			gtk_action_set_tooltip (action, _("Unlock the drive associated with the open folder"));
			break;
		}
	}

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_SELF_STOP_VOLUME);
	gtk_action_set_visible (action, show_self_stop);
	if (show_self_stop) {
		switch (self_start_stop_type) {
		default:
		case G_DRIVE_START_STOP_TYPE_UNKNOWN:
			gtk_action_set_label (action, _("_Stop"));
			gtk_action_set_tooltip (action, _("_Stop the drive associated with the open folder"));
			break;
		case G_DRIVE_START_STOP_TYPE_SHUTDOWN:
			gtk_action_set_label (action, _("_Safely Remove Drive"));
			gtk_action_set_tooltip (action, _("Safely remove the drive associated with the open folder"));
			break;
		case G_DRIVE_START_STOP_TYPE_NETWORK:
			gtk_action_set_label (action, _("_Disconnect"));
			gtk_action_set_tooltip (action, _("Disconnect the drive associated with the open folder"));
			break;
		case G_DRIVE_START_STOP_TYPE_MULTIDISK:
			gtk_action_set_label (action, _("_Stop Multi-disk Drive"));
			gtk_action_set_tooltip (action, _("Stop the multi-disk drive associated with the open folder"));
			break;
		case G_DRIVE_START_STOP_TYPE_PASSWORD:
			gtk_action_set_label (action, _("_Lock Drive"));
			gtk_action_set_tooltip (action, _("Lock the drive associated with the open folder"));
			break;
		}
	}

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_SELF_POLL);
	gtk_action_set_visible (action, show_self_poll);
	G_GNUC_END_IGNORE_DEPRECATIONS;

}

static void
real_update_location_menu_volumes (FMDirectoryView *view)
{
	GtkAction *action;
	CajaFile *file;
	gboolean show_mount;
	gboolean show_unmount;
	gboolean show_eject;
	gboolean show_connect;
	gboolean show_format;
	gboolean show_start;
	gboolean show_stop;
	gboolean show_poll;
	GDriveStartStopType start_stop_type;

	g_assert (FM_IS_DIRECTORY_VIEW (view));
	g_assert (CAJA_IS_FILE (view->details->location_popup_directory_as_file));

	file = CAJA_FILE (view->details->location_popup_directory_as_file);
	file_should_show_foreach (file,
				  &show_mount,
				  &show_unmount,
				  &show_eject,
				  &show_connect,
				  &show_format,
				  &show_start,
				  &show_stop,
				  &show_poll,
				  &start_stop_type);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_MOUNT_VOLUME);
	gtk_action_set_visible (action, show_mount);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_UNMOUNT_VOLUME);
	gtk_action_set_visible (action, show_unmount);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_EJECT_VOLUME);
	gtk_action_set_visible (action, show_eject);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_FORMAT_VOLUME);
	gtk_action_set_visible (action, show_format);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_START_VOLUME);
	gtk_action_set_visible (action, show_start);
	if (show_start) {
		switch (start_stop_type) {
		default:
		case G_DRIVE_START_STOP_TYPE_UNKNOWN:
			gtk_action_set_label (action, _("_Start"));
			gtk_action_set_tooltip (action, _("Start the selected drive"));
			break;
		case G_DRIVE_START_STOP_TYPE_SHUTDOWN:
			gtk_action_set_label (action, _("_Start"));
			gtk_action_set_tooltip (action, _("Start the selected drive"));
			break;
		case G_DRIVE_START_STOP_TYPE_NETWORK:
			gtk_action_set_label (action, _("_Connect"));
			gtk_action_set_tooltip (action, _("Connect to the selected drive"));
			break;
		case G_DRIVE_START_STOP_TYPE_MULTIDISK:
			gtk_action_set_label (action, _("_Start Multi-disk Drive"));
			gtk_action_set_tooltip (action, _("Start the selected multi-disk drive"));
			break;
		case G_DRIVE_START_STOP_TYPE_PASSWORD:
			gtk_action_set_label (action, _("_Unlock Drive"));
			gtk_action_set_tooltip (action, _("Unlock the selected drive"));
			break;
		}
	}

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_STOP_VOLUME);
	gtk_action_set_visible (action, show_stop);
	if (show_stop) {
		switch (start_stop_type) {
		default:
		case G_DRIVE_START_STOP_TYPE_UNKNOWN:
			gtk_action_set_label (action, _("_Stop"));
			gtk_action_set_tooltip (action, _("Stop the selected volume"));
			break;
		case G_DRIVE_START_STOP_TYPE_SHUTDOWN:
			gtk_action_set_label (action, _("_Safely Remove Drive"));
			gtk_action_set_tooltip (action, _("Safely remove the selected drive"));
			break;
		case G_DRIVE_START_STOP_TYPE_NETWORK:
			gtk_action_set_label (action, _("_Disconnect"));
			gtk_action_set_tooltip (action, _("Disconnect the selected drive"));
			break;
		case G_DRIVE_START_STOP_TYPE_MULTIDISK:
			gtk_action_set_label (action, _("_Stop Multi-disk Drive"));
			gtk_action_set_tooltip (action, _("Stop the selected multi-disk drive"));
			break;
		case G_DRIVE_START_STOP_TYPE_PASSWORD:
			gtk_action_set_label (action, _("_Lock Drive"));
			gtk_action_set_tooltip (action, _("Lock the selected drive"));
			break;
		}
	}

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_POLL);
	gtk_action_set_visible (action, show_poll);
	G_GNUC_END_IGNORE_DEPRECATIONS;
}

/* TODO: we should split out this routine into two functions:
 * Update on clipboard changes
 * Update on selection changes
 */
static void
real_update_paste_menu (FMDirectoryView *view,
			GList *selection,
			gint selection_count)
{
	gboolean can_paste_files_into;
	gboolean selection_is_read_only;
	gboolean is_read_only;
	GtkAction *action;

	selection_is_read_only = selection_count == 1 &&
		(!caja_file_can_write (CAJA_FILE (selection->data)) &&
		 !caja_file_has_activation_uri (CAJA_FILE (selection->data)));

	is_read_only = fm_directory_view_is_read_only (view);

	can_paste_files_into = (selection_count == 1 &&
	                        can_paste_into_file (CAJA_FILE (selection->data)));

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_PASTE);
	gtk_action_set_sensitive (action, !is_read_only);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_PASTE_FILES_INTO);
	gtk_action_set_visible (action, can_paste_files_into);
	gtk_action_set_sensitive (action, !selection_is_read_only);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	/* Ask the clipboard */
	g_object_ref (view); /* Need to keep the object alive until we get the reply */
	gtk_clipboard_request_targets (caja_clipboard_get (GTK_WIDGET (view)),
				       clipboard_targets_received,
				       view);
}

static void
real_update_location_menu (FMDirectoryView *view)
{
	GtkAction *action;
	CajaFile *file;
	gboolean is_special_link;
	gboolean is_desktop_or_home_dir;
	gboolean can_delete_file, show_delete;
	gboolean show_separate_delete_command;
	gboolean show_open_folder_window;
	gboolean show_open_in_new_tab;
	GList l;
	char *label;
	char *tip;

	show_open_folder_window = FALSE;
	show_open_in_new_tab = FALSE;

	if (caja_window_info_get_window_type (view->details->window) == CAJA_WINDOW_NAVIGATION) {
		if (g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_ALWAYS_USE_BROWSER)) {
			label = _("Open in New _Window");
		} else {
			label = _("Browse in New _Window");
			show_open_folder_window = TRUE;
		}

		show_open_in_new_tab = TRUE;
	} else {
		label = g_strdup (ngettext ("_Browse Folder",
					    "_Browse Folders", 1));
	}
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_OPEN_ALTERNATE);
	g_object_set (action,
		      "label", label,
		      NULL);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_OPEN_IN_NEW_TAB);
	gtk_action_set_visible (action, show_open_in_new_tab);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	if (show_open_in_new_tab) {
		if (g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_ALWAYS_USE_BROWSER)) {
			label = _("Open in New _Tab");
		} else {
			label = _("Browse in New _Tab");
		}
		g_object_set (action,
			      "label", label,
			      NULL);
	}

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_OPEN_FOLDER_WINDOW);
	gtk_action_set_visible (action, show_open_folder_window);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	file = view->details->location_popup_directory_as_file;
	g_assert (CAJA_IS_FILE (file));
	g_assert (caja_file_check_if_ready (file, CAJA_FILE_ATTRIBUTE_INFO |
						      CAJA_FILE_ATTRIBUTE_MOUNT |
						      CAJA_FILE_ATTRIBUTE_FILESYSTEM_INFO));

	is_special_link = CAJA_IS_DESKTOP_ICON_FILE (file);
	is_desktop_or_home_dir = caja_file_is_home (file)
		|| caja_file_is_desktop_directory (file);

	can_delete_file =
		caja_file_can_delete (file) &&
		!is_special_link &&
		!is_desktop_or_home_dir;

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_CUT);
	gtk_action_set_sensitive (action, can_delete_file);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_PASTE_FILES_INTO);
	G_GNUC_END_IGNORE_DEPRECATIONS;
	g_object_set_data (G_OBJECT (action),
			   "can-paste-according-to-destination",
			   GINT_TO_POINTER (can_paste_into_file (file)));
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	gtk_action_set_sensitive (action,
				  GPOINTER_TO_INT (g_object_get_data (G_OBJECT (action),
						   "can-paste-according-to-clipboard")) &&
				  GPOINTER_TO_INT (g_object_get_data (G_OBJECT (action),
						   "can-paste-according-to-destination")));
	G_GNUC_END_IGNORE_DEPRECATIONS;

	show_delete = TRUE;

	if (file != NULL &&
	    caja_file_is_in_trash (file)) {
		if (caja_file_is_self_owned (file)) {
			show_delete = FALSE;
		}

		label = _("_Delete Permanently");
		tip = _("Delete the open folder permanently");
		show_separate_delete_command = FALSE;
	} else {
		label = _("Mo_ve to Trash");
		tip = _("Move the open folder to the Trash");
		show_separate_delete_command = g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_ENABLE_DELETE);
	}

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_TRASH);
	g_object_set (action,
		      "label", label,
		      "tooltip", tip,
		      "icon-name", (file != NULL &&
				   caja_file_is_in_trash (file)) ?
					CAJA_ICON_DELETE : CAJA_ICON_TRASH_FULL,
		      NULL);
	gtk_action_set_sensitive (action, can_delete_file);
	gtk_action_set_visible (action, show_delete);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_DELETE);
	gtk_action_set_visible (action, show_separate_delete_command);
	if (show_separate_delete_command) {
		gtk_action_set_sensitive (action, can_delete_file);
		g_object_set (action,
			      "icon-name", CAJA_ICON_DELETE,
			      "sensitive", can_delete_file,
			      NULL);
	}

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_LOCATION_RESTORE_FROM_TRASH);
	G_GNUC_END_IGNORE_DEPRECATIONS;
	l.prev = NULL;
	l.next = NULL;
	l.data = file;
	update_restore_from_trash_action (action, &l, TRUE);

	real_update_location_menu_volumes (view);

	/* we silently assume that fm_directory_view_supports_properties always returns the same value.
	 * Therefore, we don't update the sensitivity of FM_ACTION_LOCATION_PROPERTIES */
}

static void
clipboard_changed_callback (CajaClipboardMonitor *monitor, FMDirectoryView *view)
{
	GList *selection;
	gint selection_count;

	if (!view->details->active) {
		return;
	}

	selection = fm_directory_view_get_selection (view);
	selection_count = g_list_length (selection);

	real_update_paste_menu (view, selection, selection_count);

	caja_file_list_free (selection);

}

static gboolean
can_delete_all (GList *files)
{
	GList *l;
	CajaFile *file = NULL;

	for (l = files; l != NULL; l = l->next) {
		file = l->data;
		if (!caja_file_can_delete (file)) {
			return FALSE;
		}
	}
	return TRUE;
}

static gboolean
has_writable_extra_pane (FMDirectoryView *view)
{
	FMDirectoryView *other_view;

	other_view = get_directory_view_of_extra_pane (view);
	if (other_view != NULL) {
		return !fm_directory_view_is_read_only (other_view);
	}
	return FALSE;
}

static void
real_update_menus (FMDirectoryView *view)
{
	GList *selection, *l;
	gint selection_count;
	const char *tip, *label;
	char *label_with_underscore;
	gboolean selection_contains_special_link;
	gboolean selection_contains_desktop_or_home_dir;
	gboolean can_create_files;
	gboolean can_delete_files;
	gboolean can_copy_files;
	gboolean can_link_files;
	gboolean can_duplicate_files;
	gboolean show_separate_delete_command;
	gboolean vfolder_directory;
	gboolean disable_command_line;
	gboolean show_open_alternate;
	gboolean can_open;
	gboolean show_app;
	gboolean show_save_search;
	gboolean save_search_sensitive;
	gboolean show_save_search_as;
	gboolean show_open_folder_window;
	GtkAction *action;
	GAppInfo *app;
	GIcon *app_icon;
	GtkWidget *menuitem;
	gboolean next_pane_is_writable;
	gboolean show_properties;

	selection = fm_directory_view_get_selection (view);
	selection_count = g_list_length (selection);

	selection_contains_special_link = special_link_in_selection (view);
	selection_contains_desktop_or_home_dir = desktop_or_home_dir_in_selection (view);

	can_create_files = fm_directory_view_supports_creating_files (view);
	can_delete_files =
		can_delete_all (selection) &&
		selection_count != 0 &&
		!selection_contains_special_link &&
		!selection_contains_desktop_or_home_dir;
	can_copy_files = selection_count != 0
		&& !selection_contains_special_link;

	can_duplicate_files = can_create_files && can_copy_files;
	can_link_files = can_create_files && can_copy_files;

	vfolder_directory = we_are_in_vfolder_desktop_dir (view);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_RENAME);
	gtk_action_set_sensitive (action,
				  selection_count == 1 &&
				  fm_directory_view_can_rename_file (view, selection->data));

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_NEW_FOLDER);
	gtk_action_set_sensitive (action, can_create_files);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_OPEN);
	gtk_action_set_sensitive (action, selection_count != 0);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	can_open = show_app = selection_count != 0;

	for (l = selection; l != NULL; l = l->next) {
		CajaFile *file;

		file = CAJA_FILE (l->data);

		/* Double-check if the files' MIME types have changed before we
		   commit to a choice of applications for them. This can happen
		   if, for instance, a file was originally created with 0 bytes
		   and then content was added to it later-- it will change from
		   plaintext to something else. */
		caja_file_refresh_info (file);

		if (!caja_mime_file_opens_in_external_app (file)) {
			show_app = FALSE;
		}

		if (!show_app) {
			break;
		}
	}

	label_with_underscore = NULL;

	app = NULL;
	app_icon = NULL;

	if (can_open && show_app) {
		app = caja_mime_get_default_application_for_files (selection);
	}

	if (app != NULL) {
		char *escaped_app;

		escaped_app = eel_str_double_underscores (g_app_info_get_display_name (app));
		label_with_underscore = g_strdup_printf (_("_Open With %s"),
							 escaped_app);

		app_icon = g_app_info_get_icon (app);
		if (app_icon != NULL) {
			g_object_ref (app_icon);
		}

		g_free (escaped_app);
		g_object_unref (app);
	}

	g_object_set (action, "label",
		      label_with_underscore ? label_with_underscore : _("_Open"),
		      NULL);

	menuitem = gtk_ui_manager_get_widget (
			caja_window_info_get_ui_manager (view->details->window),
			FM_DIRECTORY_VIEW_MENU_PATH_OPEN);

	/* Only force displaying the icon if it is an application icon */
	gtk_image_menu_item_set_always_show_image (
		GTK_IMAGE_MENU_ITEM (menuitem), app_icon != NULL);

	menuitem = gtk_ui_manager_get_widget (
			caja_window_info_get_ui_manager (view->details->window),
			FM_DIRECTORY_VIEW_POPUP_PATH_OPEN);

	/* Only force displaying the icon if it is an application icon */
	gtk_image_menu_item_set_always_show_image (
		GTK_IMAGE_MENU_ITEM (menuitem), app_icon != NULL);

	if (app_icon == NULL) {
		app_icon = g_themed_icon_new ("document-open");
	}

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	gtk_action_set_gicon (action, app_icon);
	g_object_unref (app_icon);

	gtk_action_set_visible (action, can_open);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	g_free (label_with_underscore);

	show_open_alternate = file_list_all_are_folders (selection) &&
				selection_count > 0 &&
				!(caja_window_info_get_window_type (view->details->window) == CAJA_WINDOW_DESKTOP &&
					g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_ALWAYS_USE_BROWSER));
	show_open_folder_window = FALSE;
	if (caja_window_info_get_window_type (view->details->window) == CAJA_WINDOW_NAVIGATION) {
		if (g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_ALWAYS_USE_BROWSER)) {
			if (selection_count == 0 || selection_count == 1) {
				label_with_underscore = g_strdup (_("Open in New _Window"));
			} else {
				label_with_underscore = g_strdup_printf (ngettext("Open in %'d New _Window",
										  "Open in %'d New _Windows",
										  selection_count),
									 selection_count);
			}
		} else {
			if (selection_count == 0 || selection_count == 1) {
				label_with_underscore = g_strdup (_("Browse in New _Window"));
			} else {
				label_with_underscore = g_strdup_printf (ngettext("Browse in %'d New _Window",
										  "Browse in %'d New _Windows",
										  selection_count),
									 selection_count);
			}
			show_open_folder_window = show_open_alternate;
		}
	} else {
		label_with_underscore = g_strdup (ngettext ("_Browse Folder",
							    "_Browse Folders",
							    selection_count));
	}

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_OPEN_ALTERNATE);
	g_object_set (action, "label",
		      label_with_underscore,
		      NULL);
	g_free (label_with_underscore);

	gtk_action_set_sensitive (action,  selection_count != 0);
	gtk_action_set_visible (action, show_open_alternate);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	/* Open in New Tab action */
	if (caja_window_info_get_window_type (view->details->window) == CAJA_WINDOW_NAVIGATION) {

		if (g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_ALWAYS_USE_BROWSER)) {
			if (selection_count == 0 || selection_count == 1) {
				label_with_underscore = g_strdup (_("Open in New _Tab"));
			} else {
				label_with_underscore = g_strdup_printf (ngettext("Open in %'d New _Tab",
										  "Open in %'d New _Tabs",
										  selection_count),
									 selection_count);
			}
		} else {
			if (selection_count == 0 || selection_count == 1) {
				label_with_underscore = g_strdup (_("Browse in New _Tab"));
			} else {
				label_with_underscore = g_strdup_printf (ngettext("Browse in %'d New _Tab",
										  "Browse in %'d New _Tabs",
										  selection_count),
									 selection_count);
			}
		}
		G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
		action = gtk_action_group_get_action (view->details->dir_action_group,
						      FM_ACTION_OPEN_IN_NEW_TAB);
		gtk_action_set_sensitive (action, selection_count != 0);
		gtk_action_set_visible (action, show_open_alternate);
		g_object_set (action, "label",
			      label_with_underscore,
			      NULL);
		g_free (label_with_underscore);
	} else {
		action = gtk_action_group_get_action (view->details->dir_action_group,
						      FM_ACTION_OPEN_IN_NEW_TAB);
		gtk_action_set_visible (action, FALSE);
		G_GNUC_END_IGNORE_DEPRECATIONS;
	}

	/* next pane actions, only in navigation mode */
	if (caja_window_info_get_window_type (view->details->window) != CAJA_WINDOW_NAVIGATION) {
		G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
		action = gtk_action_group_get_action (view->details->dir_action_group,
						      FM_ACTION_COPY_TO_NEXT_PANE);
		gtk_action_set_visible (action, FALSE);
		action = gtk_action_group_get_action (view->details->dir_action_group,
						      FM_ACTION_MOVE_TO_NEXT_PANE);
		gtk_action_set_visible (action, FALSE);
	}

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_OPEN_FOLDER_WINDOW);
	gtk_action_set_visible (action, show_open_folder_window);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	/* Broken into its own function just for convenience */
	reset_open_with_menu (view, selection);
	reset_extension_actions_menu (view, selection);

	if (all_selected_items_in_trash (view)) {
		label = _("_Delete Permanently");
		tip = _("Delete all selected items permanently");
		show_separate_delete_command = FALSE;
	} else {
		label = _("Mo_ve to Trash");
		tip = _("Move each selected item to the Trash");
		show_separate_delete_command = g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_ENABLE_DELETE);
	}

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_TRASH);
	g_object_set (action,
		      "label", label,
		      "tooltip", tip,
		      "icon-name", all_selected_items_in_trash (view) ?
					CAJA_ICON_DELETE : CAJA_ICON_TRASH_FULL,
		      NULL);
	gtk_action_set_sensitive (action, can_delete_files);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_DELETE);
	gtk_action_set_visible (action, show_separate_delete_command);

	if (show_separate_delete_command) {
		g_object_set (action,
			      "label", _("_Delete"),
			      "icon-name", CAJA_ICON_DELETE,
			      NULL);
	}
	gtk_action_set_sensitive (action, can_delete_files);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_RESTORE_FROM_TRASH);
	update_restore_from_trash_action (action, selection, FALSE);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_DUPLICATE);
	gtk_action_set_sensitive (action, can_duplicate_files);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_CREATE_LINK);
	gtk_action_set_sensitive (action, can_link_files);
	G_GNUC_END_IGNORE_DEPRECATIONS;
	g_object_set (action, "label",
		      ngettext ("Ma_ke Link",
			      	"Ma_ke Links",
				selection_count),
		      NULL);

	show_properties = (!FM_IS_DESKTOP_ICON_VIEW (view) || selection_count > 0) &&
			   fm_directory_view_supports_properties (view);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_PROPERTIES);

	gtk_action_set_sensitive (action, show_properties);

	if (selection_count == 0) {
		gtk_action_set_tooltip (action, _("View or modify the properties of the open folder"));
	} else {
		gtk_action_set_tooltip (action, _("View or modify the properties of each selected item"));
	}

	gtk_action_set_visible (action, show_properties);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_PROPERTIES_ACCEL);

	gtk_action_set_sensitive (action, show_properties);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_EMPTY_TRASH);
	g_object_set (action,
		      "label", _("E_mpty Trash"),
		      NULL);
	gtk_action_set_sensitive (action, !caja_trash_monitor_is_empty ());
	gtk_action_set_visible (action, should_show_empty_trash (view));
	G_GNUC_END_IGNORE_DEPRECATIONS;

	show_save_search = FALSE;
	save_search_sensitive = FALSE;
	show_save_search_as = FALSE;
	if (view->details->model &&
	    CAJA_IS_SEARCH_DIRECTORY (view->details->model)) {
		CajaSearchDirectory *search;

		search = CAJA_SEARCH_DIRECTORY (view->details->model);
		if (caja_search_directory_is_saved_search (search)) {
			show_save_search = TRUE;
			save_search_sensitive = caja_search_directory_is_modified (search);
		} else {
			show_save_search_as = TRUE;
		}
	}
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_SAVE_SEARCH);
	gtk_action_set_visible (action, show_save_search);
	gtk_action_set_sensitive (action, save_search_sensitive);
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_SAVE_SEARCH_AS);
	gtk_action_set_visible (action, show_save_search_as);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_SELECT_ALL);
	gtk_action_set_sensitive (action, !fm_directory_view_is_empty (view));

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_SELECT_PATTERN);
	gtk_action_set_sensitive (action, !fm_directory_view_is_empty (view));

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_INVERT_SELECTION);
	gtk_action_set_sensitive (action, !fm_directory_view_is_empty (view));

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_CUT);
	gtk_action_set_sensitive (action, can_delete_files);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_COPY);
	gtk_action_set_sensitive (action, can_copy_files);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	real_update_paste_menu (view, selection, selection_count);

	disable_command_line = g_settings_get_boolean (mate_lockdown_preferences, CAJA_PREFERENCES_LOCKDOWN_COMMAND_LINE);
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_NEW_LAUNCHER);
	gtk_action_set_visible (action, vfolder_directory && !disable_command_line);
	gtk_action_set_sensitive (action, can_create_files);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	real_update_menus_volumes (view, selection, selection_count);

	undo_update_menu (view);

	caja_file_list_free (selection);

	if (view->details->scripts_invalid) {
		update_scripts_menu (view);
	}

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_NEW_DOCUMENTS);
	gtk_action_set_sensitive (action, can_create_files);
	G_GNUC_END_IGNORE_DEPRECATIONS;

	if (can_create_files && view->details->templates_invalid) {
		update_templates_menu (view);
	}

	next_pane_is_writable = has_writable_extra_pane (view);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	/* next pane: works if file is copyable, and next pane is writable */
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_COPY_TO_NEXT_PANE);
	gtk_action_set_sensitive (action, can_copy_files && next_pane_is_writable);

	/* move to next pane: works if file is cuttable, and next pane is writable */
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_MOVE_TO_NEXT_PANE);
	gtk_action_set_sensitive (action, can_delete_files && next_pane_is_writable);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_COPY_TO_HOME);
	gtk_action_set_sensitive (action, can_copy_files);
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_COPY_TO_DESKTOP);
	gtk_action_set_sensitive (action, can_copy_files);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_MOVE_TO_HOME);
	gtk_action_set_sensitive (action, can_delete_files);
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_MOVE_TO_DESKTOP);
	gtk_action_set_sensitive (action, can_delete_files);

	action = gtk_action_group_get_action (view->details->dir_action_group,
					      "CopyToMenu");
	gtk_action_set_sensitive (action, can_copy_files);
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      "MoveToMenu");
	gtk_action_set_sensitive (action, can_delete_files);
	G_GNUC_END_IGNORE_DEPRECATIONS;
}

/**
 * fm_directory_view_pop_up_selection_context_menu
 *
 * Pop up a context menu appropriate to the selected items.
 * @view: FMDirectoryView of interest.
 * @event: The event that triggered this context menu.
 *
 * Return value: CajaDirectory for this view.
 *
 **/
void
fm_directory_view_pop_up_selection_context_menu  (FMDirectoryView *view,
						  GdkEventButton  *event)
{
	g_assert (FM_IS_DIRECTORY_VIEW (view));

	/* Make the context menu items not flash as they update to proper disabled,
	 * etc. states by forcing menus to update now.
	 */
	update_menus_if_pending (view);

	update_context_menu_position_from_event (view, event);

	/* FIXME: passing event from here won't work
	 * for gtk_menu_popup_at_pointer (in eel_pop_up_context_menu() )
	 * if the menu is being triggered from here by the menu key
	 */
	eel_pop_up_context_menu (create_popup_menu
	                         (view, FM_DIRECTORY_VIEW_POPUP_PATH_SELECTION),
	                          NULL);
}

/**
 * fm_directory_view_pop_up_background_context_menu
 *
 * Pop up a context menu appropriate to the view globally at the last right click location.
 * @view: FMDirectoryView of interest.
 *
 * Return value: CajaDirectory for this view.
 *
 **/
void
fm_directory_view_pop_up_background_context_menu (FMDirectoryView *view,
						  GdkEventButton  *event)
{
	g_assert (FM_IS_DIRECTORY_VIEW (view));

	/* Make the context menu items not flash as they update to proper disabled,
	 * etc. states by forcing menus to update now.
	 */
	update_menus_if_pending (view);

	update_context_menu_position_from_event (view, event);

	eel_pop_up_context_menu (create_popup_menu
	                         (view, FM_DIRECTORY_VIEW_POPUP_PATH_BACKGROUND),
	                         event);
}

static void
real_pop_up_location_context_menu (FMDirectoryView *view)
{
	/* always update the menu before showing it. Shouldn't be too expensive. */
	real_update_location_menu (view);

	update_context_menu_position_from_event (view, view->details->location_popup_event);

	eel_pop_up_context_menu (create_popup_menu
	                         (view, FM_DIRECTORY_VIEW_POPUP_PATH_LOCATION),
	                         view->details->location_popup_event);
}

static void
location_popup_file_attributes_ready (CajaFile *file,
				      gpointer      data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (data);
	g_assert (FM_IS_DIRECTORY_VIEW (view));

	g_assert (file == view->details->location_popup_directory_as_file);

	real_pop_up_location_context_menu (view);
}

static void
unschedule_pop_up_location_context_menu (FMDirectoryView *view)
{
	if (view->details->location_popup_directory_as_file != NULL) {
		g_assert (CAJA_IS_FILE (view->details->location_popup_directory_as_file));
		caja_file_cancel_call_when_ready (view->details->location_popup_directory_as_file,
						      location_popup_file_attributes_ready,
						      view);
		caja_file_unref (view->details->location_popup_directory_as_file);
		view->details->location_popup_directory_as_file = NULL;
	}
}

static void
schedule_pop_up_location_context_menu (FMDirectoryView *view,
				       GdkEventButton  *event,
				       CajaFile    *file)
{
	g_assert (CAJA_IS_FILE (file));

	if (view->details->location_popup_event != NULL) {
		gdk_event_free ((GdkEvent *) view->details->location_popup_event);
	}
	view->details->location_popup_event = (GdkEventButton *) gdk_event_copy ((GdkEvent *)event);

	if (file == view->details->location_popup_directory_as_file) {
		if (caja_file_check_if_ready (file, CAJA_FILE_ATTRIBUTE_INFO |
							CAJA_FILE_ATTRIBUTE_MOUNT |
							CAJA_FILE_ATTRIBUTE_FILESYSTEM_INFO)) {
			real_pop_up_location_context_menu (view);
		}
	} else {
		unschedule_pop_up_location_context_menu (view);

		view->details->location_popup_directory_as_file = caja_file_ref (file);
		caja_file_call_when_ready (view->details->location_popup_directory_as_file,
					       CAJA_FILE_ATTRIBUTE_INFO |
					       CAJA_FILE_ATTRIBUTE_MOUNT |
					       CAJA_FILE_ATTRIBUTE_FILESYSTEM_INFO,
					       location_popup_file_attributes_ready,
					       view);
	}
}

/**
 * fm_directory_view_pop_up_location_context_menu
 *
 * Pop up a context menu appropriate to the view globally.
 * @view: FMDirectoryView of interest.
 * @event: GdkEventButton triggering the popup.
 * @location: The location the popup-menu should be created for,
 * or NULL for the currently displayed location.
 *
 **/
void
fm_directory_view_pop_up_location_context_menu (FMDirectoryView *view,
						GdkEventButton  *event,
						const char      *location)
{
	CajaFile *file;

	g_assert (FM_IS_DIRECTORY_VIEW (view));

	if (location != NULL) {
		file = caja_file_get_by_uri (location);
	} else {
		file = caja_file_ref (view->details->directory_as_file);
	}

	if (file != NULL) {
		schedule_pop_up_location_context_menu (view, event, file);
		caja_file_unref (file);
	}
}

static void
fm_directory_view_drop_proxy_received_uris (FMDirectoryView *view,
					    const GList *source_uri_list,
					    const char *target_uri,
					    GdkDragAction action)
{
	char *container_uri;

	container_uri = NULL;
	if (target_uri == NULL) {
		container_uri = fm_directory_view_get_backing_uri (view);
		g_assert (container_uri != NULL);
	}

	if (action == GDK_ACTION_ASK) {
		action = caja_drag_drop_action_ask
			(GTK_WIDGET (view),
			 GDK_ACTION_MOVE | GDK_ACTION_COPY | GDK_ACTION_LINK);
		if (action == 0) {
			return;
		}
	}

	caja_clipboard_clear_if_colliding_uris (GTK_WIDGET (view),
						    source_uri_list,
						    fm_directory_view_get_copied_files_atom (view));

	fm_directory_view_move_copy_items (source_uri_list, NULL,
					   target_uri != NULL ? target_uri : container_uri,
					   action, 0, 0, view);

	g_free (container_uri);
}

static void
fm_directory_view_drop_proxy_received_netscape_url (FMDirectoryView *view,
						    const char *netscape_url,
						    const char *target_uri,
						    GdkDragAction action)
{
	fm_directory_view_handle_netscape_url_drop (view,
						    netscape_url,
						    target_uri,
						    action, 0, 0);
}

static void
schedule_update_menus (FMDirectoryView *view)
{
	g_assert (FM_IS_DIRECTORY_VIEW (view));

	/* Don't schedule updates after destroy (#349551),
 	 * or if we are not active.
 	*/
	if (view->details->window == NULL ||
	    !view->details->active) {
		return;
	}

	view->details->menu_states_untrustworthy = TRUE;

	/* Schedule a menu update with the current update interval */
	if (view->details->update_menus_timeout_id == 0) {
		view->details->update_menus_timeout_id
			= g_timeout_add (view->details->update_interval, update_menus_timeout_callback, view);
	}
}

static void
remove_update_status_idle_callback (FMDirectoryView *view)
{
	if (view->details->update_status_idle_id != 0) {
		g_source_remove (view->details->update_status_idle_id);
		view->details->update_status_idle_id = 0;
	}
}

static gboolean
update_status_idle_callback (gpointer data)
{
	FMDirectoryView *view;

	view = FM_DIRECTORY_VIEW (data);
	fm_directory_view_display_selection_info (view);
	view->details->update_status_idle_id = 0;
	return FALSE;
}

static void
schedule_update_status (FMDirectoryView *view)
{
	g_assert (FM_IS_DIRECTORY_VIEW (view));

	/* Make sure we haven't already destroyed it */
	if (view->details->window == NULL) {
		return;
	}

	if (view->details->loading) {
		/* Don't update status bar while loading the dir */
		return;
	}

	if (view->details->update_status_idle_id == 0) {
		view->details->update_status_idle_id =
			g_idle_add_full (G_PRIORITY_DEFAULT_IDLE - 20,
					 update_status_idle_callback, view, NULL);
	}
}

/**
 * fm_directory_view_notify_selection_changed:
 *
 * Notify this view that the selection has changed. This is normally
 * called only by subclasses.
 * @view: FMDirectoryView whose selection has changed.
 *
 **/
void
fm_directory_view_notify_selection_changed (FMDirectoryView *view)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	if (caja_debug_log_is_domain_enabled (CAJA_DEBUG_LOG_DOMAIN_USER)) {
		GList *selection;
		GtkWindow *window;

		selection = fm_directory_view_get_selection (view);

		window = fm_directory_view_get_containing_window (view);
		caja_debug_log_with_file_list (FALSE, CAJA_DEBUG_LOG_DOMAIN_USER, selection,
						   "selection changed in window %p",
						   window);
		caja_file_list_free (selection);
	}

	view->details->selection_was_removed = FALSE;

	if (!view->details->selection_change_is_due_to_shell) {
		view->details->send_selection_change_to_shell = TRUE;
	}

	/* Schedule a display of the new selection. */
	if (view->details->display_selection_idle_id == 0) {
		view->details->display_selection_idle_id
			= g_idle_add (display_selection_info_idle_callback,
				      view);
	}

	if (view->details->batching_selection_level != 0) {
		view->details->selection_changed_while_batched = TRUE;
	} else {
		/* Here is the work we do only when we're not
		 * batching selection changes. In other words, it's the slower
		 * stuff that we don't want to slow down selection techniques
		 * such as rubberband-selecting in icon view.
		 */

		/* Schedule an update of menu item states to match selection */
		schedule_update_menus (view);
	}
}

static void
file_changed_callback (CajaFile *file, gpointer callback_data)
{
	FMDirectoryView *view = FM_DIRECTORY_VIEW (callback_data);

	schedule_changes (view);

	schedule_update_menus (view);
	schedule_update_status (view);

	/* We might have different capabilities, so we need to update
	 * relative icon emblems . (Writeable etc).
	 * Don't do this for trash, as it never changes writability
	 * but does change a lot for the file count attribute.
	 */
	if (!caja_file_is_in_trash (file)) {
		EEL_CALL_METHOD
			(FM_DIRECTORY_VIEW_CLASS, view, emblems_changed, (view));
	}
}

/**
 * load_directory:
 *
 * Switch the displayed location to a new uri. If the uri is not valid,
 * the location will not be switched; user feedback will be provided instead.
 * @view: FMDirectoryView whose location will be changed.
 * @uri: A string representing the uri to switch to.
 *
 **/
static void
load_directory (FMDirectoryView *view,
		CajaDirectory *directory)
{
	CajaDirectory *old_directory;
	CajaFile *old_file;
	CajaFileAttributes attributes;

	g_assert (FM_IS_DIRECTORY_VIEW (view));
	g_assert (CAJA_IS_DIRECTORY (directory));

	fm_directory_view_stop (view);
	fm_directory_view_clear (view);

	view->details->loading = TRUE;

	/* Update menus when directory is empty, before going to new
	 * location, so they won't have any false lingering knowledge
	 * of old selection.
	 */
	schedule_update_menus (view);

	while (view->details->subdirectory_list != NULL) {
		fm_directory_view_remove_subdirectory (view,
				view->details->subdirectory_list->data);
	}

	disconnect_model_handlers (view);

	old_directory = view->details->model;
	caja_directory_ref (directory);
	view->details->model = directory;
	caja_directory_unref (old_directory);

	old_file = view->details->directory_as_file;
	view->details->directory_as_file =
		caja_directory_get_corresponding_file (directory);
	caja_file_unref (old_file);

	view->details->reported_load_error = FALSE;

	/* FIXME bugzilla.gnome.org 45062: In theory, we also need to monitor metadata here (as
         * well as doing a call when ready), in case external forces
         * change the directory's file metadata.
	 */
	attributes =
		CAJA_FILE_ATTRIBUTE_INFO |
		CAJA_FILE_ATTRIBUTE_MOUNT |
		CAJA_FILE_ATTRIBUTE_FILESYSTEM_INFO;
	view->details->metadata_for_directory_as_file_pending = TRUE;
	view->details->metadata_for_files_in_directory_pending = TRUE;
	caja_file_call_when_ready
		(view->details->directory_as_file,
		 attributes,
		 metadata_for_directory_as_file_ready_callback, view);
	caja_directory_call_when_ready
		(view->details->model,
		 attributes,
		 FALSE,
		 metadata_for_files_in_directory_ready_callback, view);

	/* If capabilities change, then we need to update the menus
	 * because of New Folder, and relative emblems.
	 */
	attributes =
		CAJA_FILE_ATTRIBUTE_INFO |
		CAJA_FILE_ATTRIBUTE_FILESYSTEM_INFO;
	caja_file_monitor_add (view->details->directory_as_file,
				   &view->details->directory_as_file,
				   attributes);

	view->details->file_changed_handler_id = g_signal_connect
		(view->details->directory_as_file, "changed",
		 G_CALLBACK (file_changed_callback), view);
}

static void
finish_loading (FMDirectoryView *view)
{
	CajaFileAttributes attributes;

	caja_window_info_report_load_underway (view->details->window,
						   CAJA_VIEW (view));

	/* Tell interested parties that we've begun loading this directory now.
	 * Subclasses use this to know that the new metadata is now available.
	 */
	fm_directory_view_begin_loading (view);

	/* Assume we have now all information to show window */
	caja_window_info_view_visible  (view->details->window, CAJA_VIEW (view));

	if (caja_directory_are_all_files_seen (view->details->model)) {
		/* Unschedule a pending update and schedule a new one with the minimal
		 * update interval. This gives the view a short chance at gathering the
		 * (cached) deep counts.
		 */
		unschedule_display_of_pending_files (view);
		schedule_timeout_display_of_pending_files (view, UPDATE_INTERVAL_MIN);
	}

	/* Start loading. */

	/* Connect handlers to learn about loading progress. */
	view->details->done_loading_handler_id = g_signal_connect
		(view->details->model, "done_loading",
		 G_CALLBACK (done_loading_callback), view);
	view->details->load_error_handler_id = g_signal_connect
		(view->details->model, "load_error",
		 G_CALLBACK (load_error_callback), view);

	/* Monitor the things needed to get the right icon. Also
	 * monitor a directory's item count because the "size"
	 * attribute is based on that, and the file's metadata
	 * and possible custom name.
	 */
	attributes =
		CAJA_FILE_ATTRIBUTES_FOR_ICON |
		CAJA_FILE_ATTRIBUTE_DIRECTORY_ITEM_COUNT |
		CAJA_FILE_ATTRIBUTE_INFO |
		CAJA_FILE_ATTRIBUTE_LINK_INFO |
		CAJA_FILE_ATTRIBUTE_MOUNT |
		CAJA_FILE_ATTRIBUTE_EXTENSION_INFO;

	caja_directory_file_monitor_add (view->details->model,
					     &view->details->model,
					     view->details->show_hidden_files,
					     attributes,
					     files_added_callback, view);

    	view->details->files_added_handler_id = g_signal_connect
		(view->details->model, "files_added",
		 G_CALLBACK (files_added_callback), view);
	view->details->files_changed_handler_id = g_signal_connect
		(view->details->model, "files_changed",
		 G_CALLBACK (files_changed_callback), view);
}

static void
finish_loading_if_all_metadata_loaded (FMDirectoryView *view)
{
	if (!view->details->metadata_for_directory_as_file_pending &&
	    !view->details->metadata_for_files_in_directory_pending) {
		finish_loading (view);
	}
}

static void
metadata_for_directory_as_file_ready_callback (CajaFile *file,
			      		       gpointer callback_data)
{
	FMDirectoryView *view;

	view = callback_data;

	g_assert (FM_IS_DIRECTORY_VIEW (view));
	g_assert (view->details->directory_as_file == file);
	g_assert (view->details->metadata_for_directory_as_file_pending);

	view->details->metadata_for_directory_as_file_pending = FALSE;

	finish_loading_if_all_metadata_loaded (view);
}

static void
metadata_for_files_in_directory_ready_callback (CajaDirectory *directory,
				   		GList *files,
			           		gpointer callback_data)
{
	FMDirectoryView *view;

	view = callback_data;

	g_assert (FM_IS_DIRECTORY_VIEW (view));
	g_assert (view->details->model == directory);
	g_assert (view->details->metadata_for_files_in_directory_pending);

	view->details->metadata_for_files_in_directory_pending = FALSE;

	finish_loading_if_all_metadata_loaded (view);
}

static void
finish_undoredo_callback (gpointer data)
{
}

char **
fm_directory_view_get_emblem_names_to_exclude (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), NULL);

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 get_emblem_names_to_exclude, (view));
}

static char **
real_get_emblem_names_to_exclude (FMDirectoryView *view)
{
	char **excludes;
	int i;

	g_assert (FM_IS_DIRECTORY_VIEW (view));

	excludes = g_new (char *, 3);

	i = 0;
	excludes[i++] = g_strdup (CAJA_FILE_EMBLEM_NAME_TRASH);

	if (!caja_file_can_write (view->details->directory_as_file)) {
		excludes[i++] = g_strdup (CAJA_FILE_EMBLEM_NAME_CANT_WRITE);
	}

	excludes[i++] = NULL;

	return excludes;
}

/**
 * fm_directory_view_merge_menus:
 *
 * Add this view's menus to the window's menu bar.
 * @view: FMDirectoryView in question.
 */
static void
fm_directory_view_merge_menus (FMDirectoryView *view)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	EEL_CALL_METHOD
		(FM_DIRECTORY_VIEW_CLASS, view,
		 merge_menus, (view));
}

static void
fm_directory_view_unmerge_menus (FMDirectoryView *view)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	EEL_CALL_METHOD
		(FM_DIRECTORY_VIEW_CLASS, view,
		 unmerge_menus, (view));
}

static void
disconnect_handler (GObject *object, guint *id)
{
	if (*id != 0) {
		g_signal_handler_disconnect (object, *id);
		*id = 0;
	}
}

static void
disconnect_directory_handler (FMDirectoryView *view, guint *id)
{
	disconnect_handler (G_OBJECT (view->details->model), id);
}

static void
disconnect_directory_as_file_handler (FMDirectoryView *view, guint *id)
{
	disconnect_handler (G_OBJECT (view->details->directory_as_file), id);
}

static void
disconnect_model_handlers (FMDirectoryView *view)
{
	if (view->details->model == NULL) {
		return;
	}
	disconnect_directory_handler (view, &view->details->files_added_handler_id);
	disconnect_directory_handler (view, &view->details->files_changed_handler_id);
	disconnect_directory_handler (view, &view->details->done_loading_handler_id);
	disconnect_directory_handler (view, &view->details->load_error_handler_id);
	disconnect_directory_as_file_handler (view, &view->details->file_changed_handler_id);
	caja_file_cancel_call_when_ready (view->details->directory_as_file,
					      metadata_for_directory_as_file_ready_callback,
					      view);
	caja_directory_cancel_callback (view->details->model,
					    metadata_for_files_in_directory_ready_callback,
					    view);
	caja_directory_file_monitor_remove (view->details->model,
						&view->details->model);
	caja_file_monitor_remove (view->details->directory_as_file,
				      &view->details->directory_as_file);
}

/**
 * fm_directory_view_reset_to_defaults:
 *
 * set sorting order, zoom level, etc. to match defaults
 *
 **/
void
fm_directory_view_reset_to_defaults (FMDirectoryView *view)
{
	CajaWindowShowHiddenFilesMode mode;

	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	EEL_CALL_METHOD
		(FM_DIRECTORY_VIEW_CLASS, view,
		 reset_to_defaults, (view));
	mode = caja_window_info_get_hidden_files_mode (view->details->window);
	if (mode != CAJA_WINDOW_SHOW_HIDDEN_FILES_DEFAULT) {
		caja_window_info_set_hidden_files_mode (view->details->window,
							    CAJA_WINDOW_SHOW_HIDDEN_FILES_DEFAULT);
	}
}

/**
 * fm_directory_view_select_all:
 *
 * select all the items in the view
 *
 **/
void
fm_directory_view_select_all (FMDirectoryView *view)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	EEL_CALL_METHOD
		(FM_DIRECTORY_VIEW_CLASS, view,
		 select_all, (view));
}

/**
 * fm_directory_view_set_selection:
 *
 * set the selection to the items identified in @selection. @selection
 * should be a list of CajaFiles
 *
 **/
void
fm_directory_view_set_selection (FMDirectoryView *view, GList *selection)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	EEL_CALL_METHOD
		(FM_DIRECTORY_VIEW_CLASS, view,
		 set_selection, (view, selection));
}

static void
fm_directory_view_select_file (FMDirectoryView *view, CajaFile *file)
{
	GList file_list;

	file_list.data = file;
	file_list.next = NULL;
	file_list.prev = NULL;
	fm_directory_view_set_selection (view, &file_list);
}

/**
 * fm_directory_view_get_selected_icon_locations:
 *
 * return an array of locations of selected icons if available
 * Return value: GArray of GdkPoints
 *
 **/
GArray *
fm_directory_view_get_selected_icon_locations (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), NULL);

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 get_selected_icon_locations, (view));
}

/**
 * fm_directory_view_reveal_selection:
 *
 * Scroll as necessary to reveal the selected items.
 **/
void
fm_directory_view_reveal_selection (FMDirectoryView *view)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	EEL_CALL_METHOD
		(FM_DIRECTORY_VIEW_CLASS, view,
		 reveal_selection, (view));
}

/**
 * fm_directory_view_stop:
 *
 * Stop the current ongoing process, such as switching to a new uri.
 * @view: FMDirectoryView in question.
 *
 **/
void
fm_directory_view_stop (FMDirectoryView *view)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	unschedule_display_of_pending_files (view);
	reset_update_interval (view);

	/* Free extra undisplayed files */
	file_and_directory_list_free (view->details->new_added_files);
	view->details->new_added_files = NULL;
	file_and_directory_list_free (view->details->new_changed_files);
	view->details->new_changed_files = NULL;
	g_hash_table_remove_all (view->details->non_ready_files);
	file_and_directory_list_free (view->details->old_added_files);
	view->details->old_added_files = NULL;
	file_and_directory_list_free (view->details->old_changed_files);
	view->details->old_changed_files = NULL;
	g_list_free_full (view->details->pending_locations_selected, g_object_unref);
	view->details->pending_locations_selected = NULL;

	if (view->details->model != NULL) {
		caja_directory_file_monitor_remove (view->details->model, view);
	}
	done_loading (view, FALSE);
}

gboolean
fm_directory_view_is_read_only (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 is_read_only, (view));
}

gboolean
fm_directory_view_is_empty (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 is_empty, (view));
}

gboolean
fm_directory_view_is_editable (FMDirectoryView *view)
{
	CajaDirectory *directory;

	directory = fm_directory_view_get_model (view);

	if (directory != NULL) {
		return caja_directory_is_editable (directory);
	}

	return TRUE;
}

void
fm_directory_view_set_initiated_unmount (FMDirectoryView *view,
					 gboolean initiated_unmount)
{
	if (view->details->window != NULL) {
		caja_window_info_set_initiated_unmount(view->details->window,
							   initiated_unmount);
	}
}

static gboolean
real_is_read_only (FMDirectoryView *view)
{
	CajaFile *file;

	if (!fm_directory_view_is_editable (view)) {
		return TRUE;
	}

	file = fm_directory_view_get_directory_as_file (view);
	if (file != NULL) {
		return !caja_file_can_write (file);
	}
	return FALSE;
}

gboolean
fm_directory_view_supports_creating_files (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 supports_creating_files, (view));
}

gboolean
fm_directory_view_accepts_dragged_files (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 accepts_dragged_files, (view));
}

/**
 * fm_directory_view_should_show_file
 *
 * Returns whether or not this file should be displayed based on
 * current filtering options.
 */
gboolean
fm_directory_view_should_show_file (FMDirectoryView *view, CajaFile *file)
{
	return caja_file_should_show (file,
				      view->details->show_hidden_files,
				      view->details->show_foreign_files,
				      view->details->show_backup_files);
}

static gboolean
real_supports_creating_files (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	return !fm_directory_view_is_read_only (view) && !showing_trash_directory (view);
}

static gboolean
real_accepts_dragged_files (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	return !fm_directory_view_is_read_only (view);
}

gboolean
fm_directory_view_supports_properties (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 supports_properties, (view));
}

static gboolean
real_supports_properties (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	return TRUE;
}

gboolean
fm_directory_view_supports_zooming (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 supports_zooming, (view));
}

static gboolean
real_supports_zooming (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	return TRUE;
}

gboolean
fm_directory_view_using_manual_layout (FMDirectoryView  *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	return EEL_CALL_METHOD_WITH_RETURN_VALUE
		(FM_DIRECTORY_VIEW_CLASS, view,
		 using_manual_layout, (view));
}

static gboolean
real_using_manual_layout (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	return FALSE;
}

/**
 * fm_directory_view_update_menus:
 *
 * Update the sensitivity and wording of dynamic menu items.
 * @view: FMDirectoryView in question.
 */
void
fm_directory_view_update_menus (FMDirectoryView *view)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	if (!view->details->active) {
		return;
	}

	EEL_CALL_METHOD
		(FM_DIRECTORY_VIEW_CLASS, view,
		 update_menus, (view));

	view->details->menu_states_untrustworthy = FALSE;
}

static void
schedule_update_menus_callback (gpointer callback_data)
{
	schedule_update_menus (FM_DIRECTORY_VIEW (callback_data));
}

void
fm_directory_view_ignore_hidden_file_preferences (FMDirectoryView *view)
{
	g_return_if_fail (view->details->model == NULL);

	if (view->details->ignore_hidden_file_preferences) {
		return;
	}

	view->details->show_hidden_files = FALSE;
	view->details->ignore_hidden_file_preferences = TRUE;
}

void
fm_directory_view_set_show_foreign (FMDirectoryView *view,
		                    gboolean show_foreign)
{
	view->details->show_foreign_files = show_foreign;
}

char *
fm_directory_view_get_uri (FMDirectoryView *view)
{
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), NULL);
	if (view->details->model == NULL) {
		return NULL;
	}
	return caja_directory_get_uri (view->details->model);
}

/* Get the real directory where files will be stored and created */
char *
fm_directory_view_get_backing_uri (FMDirectoryView *view)
{
	CajaDirectory *directory;
	char *uri;

	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), NULL);

	if (view->details->model == NULL) {
		return NULL;
	}

	directory = view->details->model;

	if (CAJA_IS_DESKTOP_DIRECTORY (directory)) {
		directory = caja_desktop_directory_get_real_directory (CAJA_DESKTOP_DIRECTORY (directory));
	} else {
		caja_directory_ref (directory);
	}

	uri = caja_directory_get_uri (directory);

	caja_directory_unref (directory);

	return uri;
}

void
fm_directory_view_move_copy_items (const GList *item_uris,
				   GArray *relative_item_points,
				   const char *target_uri,
				   int copy_action,
				   int x, int y,
				   FMDirectoryView *view)
{
	CajaFile *target_file;

	g_assert (relative_item_points == NULL
		  || relative_item_points->len == 0
		  || g_list_length ((GList *)item_uris) == relative_item_points->len);

	/* add the drop location to the icon offsets */
	offset_drop_points (relative_item_points, x, y);

	target_file = caja_file_get_existing_by_uri (target_uri);
	/* special-case "command:" here instead of starting a move/copy */
	if (target_file != NULL && caja_file_is_launcher (target_file)) {
		caja_file_unref (target_file);
		caja_launch_desktop_file (
				gtk_widget_get_screen (GTK_WIDGET (view)),
				target_uri, item_uris,
				fm_directory_view_get_containing_window (view));
		return;
	} else if (copy_action == GDK_ACTION_COPY &&
		   caja_is_engrampa_installed () &&
		   target_file != NULL &&
		   caja_file_is_archive (target_file)) {
		char *command, *quoted_uri, *tmp;
		const GList *l;
		GdkScreen  *screen;

		/* Handle dropping onto a engrampa archiver file, instead of starting a move/copy */

		caja_file_unref (target_file);

		quoted_uri = g_shell_quote (target_uri);
		command = g_strconcat ("engrampa -a ", quoted_uri, NULL);
		g_free (quoted_uri);

		for (l = item_uris; l != NULL; l = l->next) {
			quoted_uri = g_shell_quote ((char *) l->data);

			tmp = g_strconcat (command, " ", quoted_uri, NULL);
			g_free (command);
			command = tmp;

			g_free (quoted_uri);
		}

		screen = gtk_widget_get_screen (GTK_WIDGET (view));
		if (screen == NULL) {
			screen = gdk_screen_get_default ();
		}

		mate_gdk_spawn_command_line_on_screen(screen, command, NULL);
		g_free (command);

		return;
	}
	caja_file_unref (target_file);

	caja_file_operations_copy_move
		(item_uris, relative_item_points,
		 target_uri, copy_action, GTK_WIDGET (view),
		 copy_move_done_callback, pre_copy_move (view));
}

gboolean
fm_directory_view_can_accept_item (CajaFile *target_item,
				   const char *item_uri,
				   FMDirectoryView *view)
{
	g_return_val_if_fail (CAJA_IS_FILE (target_item), FALSE);
	g_return_val_if_fail (item_uri != NULL, FALSE);
	g_return_val_if_fail (FM_IS_DIRECTORY_VIEW (view), FALSE);

	return caja_drag_can_accept_item (target_item, item_uri);
}

static void
fm_directory_view_trash_state_changed_callback (CajaTrashMonitor *trash_monitor,
						gboolean state, gpointer callback_data)
{
	FMDirectoryView *view;

	view = (FMDirectoryView *) callback_data;
	g_assert (FM_IS_DIRECTORY_VIEW (view));

	schedule_update_menus (view);
}

void
fm_directory_view_start_batching_selection_changes (FMDirectoryView *view)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));

	++view->details->batching_selection_level;
	view->details->selection_changed_while_batched = FALSE;
}

void
fm_directory_view_stop_batching_selection_changes (FMDirectoryView *view)
{
	g_return_if_fail (FM_IS_DIRECTORY_VIEW (view));
	g_return_if_fail (view->details->batching_selection_level > 0);

	if (--view->details->batching_selection_level == 0) {
		if (view->details->selection_changed_while_batched) {
			fm_directory_view_notify_selection_changed (view);
		}
	}
}

static void
revert_slashes (char *string)
{
	while (*string != 0) {
		if (*string == '/') {
			*string = '\\';
		}
		string++;
	}
}

static GdkDragAction
ask_link_action (FMDirectoryView *view)
{
	int button_pressed;
	GdkDragAction result;
	GtkWindow *parent_window;
	GtkWidget *dialog;

	parent_window = NULL;

	/* Don't use desktop window as parent, since that means
	   we show up an all desktops etc */
	if (! FM_IS_DESKTOP_ICON_VIEW (view)) {
		parent_window = GTK_WINDOW (fm_directory_view_get_containing_window (view));
	}

	dialog = gtk_message_dialog_new (parent_window,
					 GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_MESSAGE_QUESTION,
					 GTK_BUTTONS_NONE,
					 _("Download location?"));

	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
						  _("You can download it or make a link to it."));

	gtk_dialog_add_button (GTK_DIALOG (dialog),
			       _("Make a _Link"), 0);

	eel_dialog_add_button (GTK_DIALOG (dialog),
			       _("_Cancel"),
			       "process-stop", 1);

	gtk_dialog_add_button (GTK_DIALOG (dialog),
			       _("_Download"), 2);

	gtk_window_set_title (GTK_WINDOW (dialog), ""); /* as per HIG */
	gtk_window_set_focus_on_map (GTK_WINDOW (dialog), TRUE);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), 2);

	gtk_window_present (GTK_WINDOW (dialog));

	button_pressed = gtk_dialog_run (GTK_DIALOG (dialog));

	gtk_widget_destroy (dialog);

	switch (button_pressed) {
	case 0:
		result = GDK_ACTION_LINK;
		break;
	case 1:
	case GTK_RESPONSE_DELETE_EVENT:
		result = 0;
		break;
	case 2:
		result = GDK_ACTION_COPY;
		break;
	default:
		g_assert_not_reached ();
		result = 0;
	}

	return result;
}

typedef struct {
	FMDirectoryView  *view;
	GCancellable *cancellable;
	char *encoded_url;
	char *target_uri;
	int x;
	int y;
	guint timeout;
} NetscapeUrlDropAsk;

static void
handle_netscape_url_drop_ask_cb (GObject *source_object,
				 GAsyncResult *res,
				 gpointer user_data)
{
	NetscapeUrlDropAsk *data;
	GdkDragAction action;
	GFileInfo *info;
	GFile *f;
	const char *mime_type;

	data = user_data;
	f = G_FILE (source_object);

	info = g_file_query_info_finish (f, res, NULL);
	mime_type = NULL;

	if (info) {
		mime_type = g_file_info_get_content_type (info);
	}

	if (mime_type != NULL &&
	    (g_content_type_equals (mime_type, "text/html") ||
	     g_content_type_equals (mime_type, "text/xml")  ||
	     g_content_type_equals (mime_type, "application/xhtml+xml"))) {
		action = GDK_ACTION_LINK;
	} else if (mime_type != NULL &&
		   g_content_type_equals (mime_type, "text/plain")) {
		action = ask_link_action (data->view);
	} else {
		action = GDK_ACTION_COPY;
	}
	if (info) {
		g_object_unref (info);
	}

	if (action != 0) {
		fm_directory_view_handle_netscape_url_drop (data->view,
							    data->encoded_url,
							    data->target_uri,
							    action,
							    data->x, data->y);
	}

	g_object_unref (data->view);
	g_object_unref (data->cancellable);
	if (data->timeout != 0) {
		g_source_remove (data->timeout);
	}
	g_free (data->encoded_url);
	g_free (data->target_uri);
	g_free (data);
}

static gboolean
handle_netscape_url_drop_timeout (gpointer user_data)
{
	NetscapeUrlDropAsk *data;

	data = user_data;

	g_cancellable_cancel (data->cancellable);
	data->timeout = 0;

	return FALSE;
}

static inline void
fm_directory_view_widget_to_file_operation_position (FMDirectoryView *view,
						     GdkPoint *position)
{
	EEL_CALL_METHOD (FM_DIRECTORY_VIEW_CLASS, view,
			 widget_to_file_operation_position,
			 (view, position));
}

static void
fm_directory_view_widget_to_file_operation_position_xy (FMDirectoryView *view,
							int *x, int *y)
{
	GdkPoint position;

	position.x = *x;
	position.y = *y;
	fm_directory_view_widget_to_file_operation_position (view, &position);
	*x = position.x;
	*y = position.y;
}

void
fm_directory_view_handle_netscape_url_drop (FMDirectoryView  *view,
					    const char       *encoded_url,
					    const char       *target_uri,
					    GdkDragAction     action,
					    int               x,
					    int               y)
{
	GdkPoint point;
	char *url, *title;
	char *container_uri;
	char **bits;
	GList *uri_list = NULL;
	GFile *f;

	if (encoded_url == NULL) {
		return;
	}

	container_uri = NULL;
	if (target_uri == NULL) {
		container_uri = fm_directory_view_get_backing_uri (view);
		g_assert (container_uri != NULL);
	}

	f = g_file_new_for_uri (target_uri != NULL ? target_uri : container_uri);
	if (!g_file_is_native (f)) {
		eel_show_warning_dialog (_("Drag and drop is not supported."),
					 _("Drag and drop is only supported on local file systems."),
					 fm_directory_view_get_containing_window (view));
		g_object_unref (f);
		g_free (container_uri);
		return;
	}
	g_object_unref (f);

	/* _NETSCAPE_URL_ works like this: $URL\n$TITLE */
	bits = g_strsplit (encoded_url, "\n", 0);
	switch (g_strv_length (bits)) {
	case 0:
		g_strfreev (bits);
		g_free (container_uri);
		return;
	case 1:
		url = bits[0];
		title = NULL;
		break;
	default:
		url = bits[0];
		title = bits[1];
	}

	if (action == GDK_ACTION_ASK) {
		NetscapeUrlDropAsk *data;

		f = g_file_new_for_uri (url);
		data = g_new0 (NetscapeUrlDropAsk, 1);
		data->view = g_object_ref (view);
		data->cancellable = g_cancellable_new ();
		data->encoded_url = g_strdup (encoded_url);
		data->target_uri = g_strdup (target_uri);
		data->x = x;
		data->y = y;
		/* Ensure we wait at most 1 second for mimetype */
		data->timeout = g_timeout_add (1000,
					       handle_netscape_url_drop_timeout,
					       data);
		g_file_query_info_async (f,
					 G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, 0,
					 0, data->cancellable,
					 handle_netscape_url_drop_ask_cb,
					 data);

		g_strfreev (bits);
		g_free (container_uri);
		return;
	}

	fm_directory_view_widget_to_file_operation_position_xy (view, &x, &y);

	/* We don't support GDK_ACTION_ASK or GDK_ACTION_PRIVATE
	 * and we don't support combinations either. */
	if ((action != GDK_ACTION_DEFAULT) &&
	    (action != GDK_ACTION_COPY) &&
	    (action != GDK_ACTION_MOVE) &&
	    (action != GDK_ACTION_LINK)) {
		eel_show_warning_dialog (_("Drag and drop is not supported."),
					 _("An invalid drag type was used."),
					 fm_directory_view_get_containing_window (view));
		g_strfreev (bits);
		g_free (container_uri);
		return;
	}

	if (action == GDK_ACTION_LINK) {
		char *link_name;

		if (eel_str_is_empty (title)) {
			GFile *f;

			f = g_file_new_for_uri (url);
			link_name = g_file_get_basename (f);
			g_object_unref (f);
		} else {
			link_name = g_strdup (title);
		}

		if (!eel_str_is_empty (link_name)) {
			GdkScreen *screen;
			int screen_num;
			char *link_display_name;
			GError *error = NULL;
			gboolean success;

			link_display_name = g_strdup_printf (_("Link to %s"), link_name);

			/* The filename can't contain slashes, strip em.
			   (the basename of http://foo/ is http://foo/) */
			revert_slashes (link_name);

			point.x = x;
			point.y = y;

			screen = gtk_widget_get_screen (GTK_WIDGET (view));
			screen_num = gdk_x11_screen_get_screen_number (screen);

			success = caja_link_local_create (target_uri != NULL ? target_uri : container_uri,
			                                  link_name,
			                                  link_display_name,
			                                  "mate-fs-bookmark",
			                                  url,
			                                  &point,
			                                  screen_num,
			                                  TRUE,
			                                  &error);
			if (!success) {
				if (error) {
					eel_show_error_dialog (_("Link Creation Failed"),
					                       error->message, NULL);
					g_error_free (error);
				} else {
					gchar *error_message = g_strdup_printf (_("Cannot create the link to %s"), url);
					eel_show_error_dialog (_("Link Creation Failed"),
					                       error_message, NULL);
					g_free (error_message);
				}
			}

			g_free (link_display_name);
		}
		g_free (link_name);
	} else {
		GArray *points;

		GdkPoint tmp_point = { 0, 0 };

		/* pass in a 1-item array of icon positions, relative to x, y */
		points = g_array_new (FALSE, TRUE, sizeof (GdkPoint));
		g_array_append_val (points, tmp_point);

		uri_list = g_list_append (uri_list, url);

		fm_directory_view_move_copy_items (uri_list, points,
						   target_uri != NULL ? target_uri : container_uri,
						   action, x, y, view);

		g_list_free (uri_list);
		g_array_free (points, TRUE);
	}

	g_strfreev (bits);
	g_free (container_uri);
}

void
fm_directory_view_handle_uri_list_drop (FMDirectoryView  *view,
					const char       *item_uris,
					const char       *target_uri,
					GdkDragAction     action,
					int               x,
					int               y)
{
	gchar **uri_list;
	GList *real_uri_list = NULL;
	char *container_uri;
	int n_uris, i;
	GArray *points;

	if (item_uris == NULL) {
		return;
	}

	container_uri = NULL;
	if (target_uri == NULL) {
		container_uri = fm_directory_view_get_backing_uri (view);
		g_assert (container_uri != NULL);
	}

	if (action == GDK_ACTION_ASK) {
		action = caja_drag_drop_action_ask
			(GTK_WIDGET (view),
			 GDK_ACTION_MOVE | GDK_ACTION_COPY | GDK_ACTION_LINK);
		if (action == 0) {
			g_free (container_uri);
			return;
		}
	}

	/* We don't support GDK_ACTION_ASK or GDK_ACTION_PRIVATE
	 * and we don't support combinations either. */
	if ((action != GDK_ACTION_DEFAULT) &&
	    (action != GDK_ACTION_COPY) &&
	    (action != GDK_ACTION_MOVE) &&
	    (action != GDK_ACTION_LINK)) {
		eel_show_warning_dialog (_("Drag and drop is not supported."),
					 _("An invalid drag type was used."),
					 fm_directory_view_get_containing_window (view));
		g_free (container_uri);
		return;
	}

	n_uris = 0;
	uri_list = g_uri_list_extract_uris (item_uris);
	for (i = 0; uri_list[i] != NULL; i++) {
		real_uri_list = g_list_append (real_uri_list, uri_list[i]);
		n_uris++;
	}
	g_free (uri_list);

	/* do nothing if no real uris are left */
	if (n_uris == 0) {
		g_free (container_uri);
		return;
	}

	if (n_uris == 1) {
		GdkPoint tmp_point = { 0, 0 };

		/* pass in a 1-item array of icon positions, relative to x, y */
		points = g_array_new (FALSE, TRUE, sizeof (GdkPoint));
		g_array_append_val (points, tmp_point);
	} else {
		points = NULL;
	}

	fm_directory_view_widget_to_file_operation_position_xy (view, &x, &y);

	fm_directory_view_move_copy_items (real_uri_list, points,
					   target_uri != NULL ? target_uri : container_uri,
					   action, x, y, view);

	g_list_free_full (real_uri_list, g_free);

	if (points != NULL)
		g_array_free (points, TRUE);

	g_free (container_uri);
}

void
fm_directory_view_handle_text_drop (FMDirectoryView  *view,
				    const char       *text,
				    const char       *target_uri,
				    GdkDragAction     action,
				    int               x,
				    int               y)
{
	int length;
	char *container_uri;
	GdkPoint pos;

	if (text == NULL) {
		return;
	}

	g_return_if_fail (action == GDK_ACTION_COPY);

	container_uri = NULL;
	if (target_uri == NULL) {
		container_uri = fm_directory_view_get_backing_uri (view);
		g_assert (container_uri != NULL);
	}

	length = strlen (text);

	pos.x = x;
	pos.y = y;
	fm_directory_view_widget_to_file_operation_position (view, &pos);

	fm_directory_view_new_file_with_initial_contents (
		view, target_uri != NULL ? target_uri : container_uri,
		/* Translators: This is the filename used for when you dnd text to a directory */
		_("dropped text.txt"),
		text, length, &pos);

	g_free (container_uri);
}

void
fm_directory_view_handle_raw_drop (FMDirectoryView  *view,
				    const char       *raw_data,
				    int               length,
				    const char       *target_uri,
				    const char       *direct_save_uri,
				    GdkDragAction     action,
				    int               x,
				    int               y)
{
	char *container_uri, *filename;
	GdkPoint pos;

	if (raw_data == NULL) {
		return;
	}

	g_return_if_fail (action == GDK_ACTION_COPY);

	container_uri = NULL;
	if (target_uri == NULL) {
		container_uri = fm_directory_view_get_backing_uri (view);
		g_assert (container_uri != NULL);
	}

	pos.x = x;
	pos.y = y;
	fm_directory_view_widget_to_file_operation_position (view, &pos);

	filename = NULL;
	if (direct_save_uri != NULL) {
		GFile *direct_save_full;

		direct_save_full = g_file_new_for_uri (direct_save_uri);
		filename = g_file_get_basename (direct_save_full);
	}
	if (filename == NULL) {
		/* Translators: This is the filename used for when you dnd raw
		 * data to a directory, if the source didn't supply a name.
		 */
		filename = _("dropped data");
	}

	fm_directory_view_new_file_with_initial_contents (
		view, target_uri != NULL ? target_uri : container_uri,
		filename, raw_data, length, &pos);

	g_free (container_uri);
}

gboolean
fm_directory_view_get_active (FMDirectoryView *view)
{
	g_assert (FM_IS_DIRECTORY_VIEW (view));
	return view->details->active;
}

static GArray *
real_get_selected_icon_locations (FMDirectoryView *view)
{
        /* By default, just return an empty list. */
        return g_array_new (FALSE, TRUE, sizeof (GdkPoint));
}

static void
fm_directory_view_set_property (GObject         *object,
				guint            prop_id,
				const GValue    *value,
				GParamSpec      *pspec)
{
  FMDirectoryView *directory_view;
  CajaWindowSlotInfo *slot;
  CajaWindowInfo *window;

  directory_view = FM_DIRECTORY_VIEW (object);

  switch (prop_id)  {
  case PROP_WINDOW_SLOT:
	  g_assert (directory_view->details->slot == NULL);

	  slot = CAJA_WINDOW_SLOT_INFO (g_value_get_object (value));
          window = caja_window_slot_info_get_window (slot);

	  directory_view->details->slot = slot;
	  directory_view->details->window = window;

	  g_signal_connect_object (directory_view->details->slot,
				   "active", G_CALLBACK (slot_active),
				   directory_view, 0);
	  g_signal_connect_object (directory_view->details->slot,
				   "inactive", G_CALLBACK (slot_inactive),
				   directory_view, 0);

	  g_signal_connect_object (directory_view->details->window,
				   "hidden-files-mode-changed", G_CALLBACK (hidden_files_mode_changed),
				   directory_view, 0);
	  fm_directory_view_init_show_hidden_files (directory_view);

	  g_signal_connect_object (directory_view->details->window,
				   "backup-files-mode-changed", G_CALLBACK (backup_files_mode_changed),
				   directory_view, 0);
	  fm_directory_view_init_show_backup_files (directory_view);

      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

gboolean
fm_directory_view_handle_scroll_event (FMDirectoryView *directory_view,
				       GdkEventScroll *event)
{
	static gdouble total_delta_y = 0;
	gdouble delta_x, delta_y;

	if (event->state & GDK_CONTROL_MASK) {
		switch (event->direction) {
		case GDK_SCROLL_UP:
			/* Zoom In */
			fm_directory_view_bump_zoom_level (directory_view, 1);
			return TRUE;

		case GDK_SCROLL_DOWN:
			/* Zoom Out */
			fm_directory_view_bump_zoom_level (directory_view, -1);
			return TRUE;

		case GDK_SCROLL_SMOOTH:
			gdk_event_get_scroll_deltas ((const GdkEvent *) event,
			                             &delta_x, &delta_y);

			/* try to emulate a normal scrolling event by summing deltas */
			total_delta_y += delta_y;

			if (total_delta_y >= 1) {
				total_delta_y = 0;
				/* emulate scroll down */
				fm_directory_view_bump_zoom_level (directory_view, -1);
				return TRUE;
			} else if (total_delta_y <= - 1) {
				total_delta_y = 0;
				/* emulate scroll up */
				fm_directory_view_bump_zoom_level (directory_view, 1);
				return TRUE;
			} else {
				/* eat event */
				return TRUE;
			}

		case GDK_SCROLL_LEFT:
		case GDK_SCROLL_RIGHT:
			break;

		default:
			g_assert_not_reached ();
		}
	}

	return FALSE;
}

/* handle Shift+Scroll, which will cause a zoom-in/out */
static gboolean
fm_directory_view_scroll_event (GtkWidget *widget,
				GdkEventScroll *event)
{
	FMDirectoryView *directory_view;

	directory_view = FM_DIRECTORY_VIEW (widget);
	if (fm_directory_view_handle_scroll_event (directory_view, event)) {
		return TRUE;
	}

	return GTK_WIDGET_CLASS (parent_class)->scroll_event (widget, event);
}

static void
fm_directory_view_parent_set (GtkWidget *widget,
			      GtkWidget *old_parent)
{
	FMDirectoryView *view;
	GtkWidget *parent;

	view = FM_DIRECTORY_VIEW (widget);

	parent = gtk_widget_get_parent (widget);
	g_assert (parent == NULL || old_parent == NULL);

	if (GTK_WIDGET_CLASS (parent_class)->parent_set != NULL) {
		GTK_WIDGET_CLASS (parent_class)->parent_set (widget, old_parent);
	}

	if (parent != NULL) {
		g_assert (old_parent == NULL);

		if (view->details->slot ==
		    caja_window_info_get_active_slot (view->details->window)) {
			view->details->active = TRUE;

			fm_directory_view_merge_menus (view);
			schedule_update_menus (view);
		}
	} else {
		fm_directory_view_unmerge_menus (view);
		remove_update_menus_timeout_callback (view);
	}
}

static void
fm_directory_view_class_init (FMDirectoryViewClass *klass)
{
	GtkWidgetClass *widget_class;
	GtkScrolledWindowClass *scrolled_window_class;
	GtkBindingSet *binding_set;

	widget_class = GTK_WIDGET_CLASS (klass);
	scrolled_window_class = GTK_SCROLLED_WINDOW_CLASS (klass);

	G_OBJECT_CLASS (klass)->set_property = fm_directory_view_set_property;
	G_OBJECT_CLASS (klass)->finalize = fm_directory_view_finalize;

	widget_class->destroy = fm_directory_view_destroy;

	widget_class->scroll_event = fm_directory_view_scroll_event;
	widget_class->parent_set = fm_directory_view_parent_set;

	/* Get rid of the strange 3-pixel gap that GtkScrolledWindow
	 * uses by default. It does us no good.
	 */
	scrolled_window_class->scrollbar_spacing = 0;

	signals[ADD_FILE] =
		g_signal_new ("add_file",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (FMDirectoryViewClass, add_file),
		              NULL, NULL,
		              fm_marshal_VOID__OBJECT_OBJECT,
		              G_TYPE_NONE, 2, CAJA_TYPE_FILE, CAJA_TYPE_DIRECTORY);
	signals[BEGIN_FILE_CHANGES] =
		g_signal_new ("begin_file_changes",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (FMDirectoryViewClass, begin_file_changes),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0);
	signals[BEGIN_LOADING] =
		g_signal_new ("begin_loading",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (FMDirectoryViewClass, begin_loading),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0);
	signals[CLEAR] =
		g_signal_new ("clear",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (FMDirectoryViewClass, clear),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0);
	signals[END_FILE_CHANGES] =
		g_signal_new ("end_file_changes",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (FMDirectoryViewClass, end_file_changes),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0);
	signals[FLUSH_ADDED_FILES] =
		g_signal_new ("flush_added_files",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (FMDirectoryViewClass, flush_added_files),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0);
	signals[END_LOADING] =
		g_signal_new ("end_loading",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (FMDirectoryViewClass, end_loading),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__BOOLEAN,
		              G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
	signals[FILE_CHANGED] =
		g_signal_new ("file_changed",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (FMDirectoryViewClass, file_changed),
		              NULL, NULL,
		              fm_marshal_VOID__OBJECT_OBJECT,
		              G_TYPE_NONE, 2, CAJA_TYPE_FILE, CAJA_TYPE_DIRECTORY);
	signals[LOAD_ERROR] =
		g_signal_new ("load_error",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (FMDirectoryViewClass, load_error),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__POINTER,
		              G_TYPE_NONE, 1, G_TYPE_POINTER);
	signals[REMOVE_FILE] =
		g_signal_new ("remove_file",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (FMDirectoryViewClass, remove_file),
		              NULL, NULL,
		              fm_marshal_VOID__OBJECT_OBJECT,
		              G_TYPE_NONE, 2, CAJA_TYPE_FILE, CAJA_TYPE_DIRECTORY);

	klass->accepts_dragged_files = real_accepts_dragged_files;
	klass->file_still_belongs = real_file_still_belongs;
	klass->get_emblem_names_to_exclude = real_get_emblem_names_to_exclude;
	klass->get_selected_icon_locations = real_get_selected_icon_locations;
	klass->is_read_only = real_is_read_only;
	klass->load_error = real_load_error;
	klass->can_rename_file = can_rename_file;
	klass->start_renaming_file = start_renaming_file;
	klass->supports_creating_files = real_supports_creating_files;
	klass->supports_properties = real_supports_properties;
	klass->supports_zooming = real_supports_zooming;
	klass->using_manual_layout = real_using_manual_layout;
	klass->merge_menus = real_merge_menus;
	klass->unmerge_menus = real_unmerge_menus;
	klass->update_menus = real_update_menus;
	klass->set_is_active = real_set_is_active;
	/* Function pointers that subclasses must override */
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, add_file);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, bump_zoom_level);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, can_zoom_in);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, can_zoom_out);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, clear);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, file_changed);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, get_background_widget);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, get_selection);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, get_selection_for_file_transfer);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, get_item_count);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, is_empty);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, reset_to_defaults);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, restore_default_zoom_level);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, select_all);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, set_selection);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, invert_selection);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, zoom_to_level);
	EEL_ASSIGN_MUST_OVERRIDE_SIGNAL (klass, fm_directory_view, get_zoom_level);

	copied_files_atom = gdk_atom_intern ("x-special/mate-copied-files", FALSE);

	g_object_class_install_property (G_OBJECT_CLASS (klass),
					 PROP_WINDOW_SLOT,
					 g_param_spec_object ("window-slot",
							      "Window Slot",
							      "The parent window slot reference",
							      CAJA_TYPE_WINDOW_SLOT_INFO,
							      G_PARAM_WRITABLE |
							      G_PARAM_CONSTRUCT_ONLY));

	signals[TRASH] =
		g_signal_new ("trash",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
			      G_STRUCT_OFFSET (FMDirectoryViewClass, trash),
			      g_signal_accumulator_true_handled, NULL,
			      fm_marshal_BOOLEAN__VOID,
			      G_TYPE_BOOLEAN, 0);
	signals[DELETE] =
		g_signal_new ("delete",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
			      G_STRUCT_OFFSET (FMDirectoryViewClass, delete),
			      g_signal_accumulator_true_handled, NULL,
			      fm_marshal_BOOLEAN__VOID,
			      G_TYPE_BOOLEAN, 0);

	binding_set = gtk_binding_set_by_class (klass);
	gtk_binding_entry_add_signal (binding_set, GDK_KEY_Delete, 0,
				      "trash", 0);
	gtk_binding_entry_add_signal (binding_set, GDK_KEY_KP_Delete, 0,
				      "trash", 0);
	gtk_binding_entry_add_signal (binding_set, GDK_KEY_Delete, GDK_SHIFT_MASK,
				      "delete", 0);
	gtk_binding_entry_add_signal (binding_set, GDK_KEY_KP_Delete, GDK_SHIFT_MASK,
				      "delete", 0);

	klass->trash = real_trash;
	klass->delete = real_delete;
}

static void
undo_redo_menu_update_callback (CajaUndoStackManager* manager, gpointer arg, gpointer data)
{
	FMDirectoryView *view;
	view = FM_DIRECTORY_VIEW (data);

	CajaUndoStackMenuData* menudata = (CajaUndoStackMenuData*) arg;

	g_free(view->details->undo_action_label);
	g_free(view->details->undo_action_description);
	g_free(view->details->redo_action_label);
	g_free(view->details->redo_action_description);

	view->details->undo_active = menudata->undo_label ? TRUE : FALSE;
	view->details->redo_active = menudata->redo_label ? TRUE : FALSE;

	view->details->undo_action_label = g_strdup (menudata->undo_label);
	view->details->undo_action_description = g_strdup (menudata->undo_description);
	view->details->redo_action_label = g_strdup (menudata->redo_label);
	view->details->redo_action_description =  g_strdup (menudata->redo_description);

	schedule_update_menus (view);
}

static void
undo_update_menu (FMDirectoryView *view)
{
	GtkAction *action;
	gboolean available = FALSE;
	gchar* label;
	gchar* tooltip;

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	/* Update undo entry */
	action = gtk_action_group_get_action (view->details->dir_action_group,
					      FM_ACTION_UNDO);
	G_GNUC_END_IGNORE_DEPRECATIONS;
	available = view->details->undo_active;
	if (available) {
		label = view->details->undo_action_label;
		tooltip = view->details->undo_action_description;
	} else {
		/* Reset to default info */
		label = _("Undo");
		tooltip = _("Undo the last action");
	}
	g_object_set (action,
		      "label", label,
		      "tooltip", tooltip,
		      NULL);
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	gtk_action_set_sensitive (action, available);

	/* Update redo entry */
	action = gtk_action_group_get_action (view->details->dir_action_group,
                                          FM_ACTION_REDO);
	G_GNUC_END_IGNORE_DEPRECATIONS;
	available = view->details->redo_active;
	if (available) {
		label = view->details->redo_action_label;
		tooltip = view->details->redo_action_description;
	} else {
		/* Reset to default info */
		label = _("Redo");
		tooltip = _("Redo the last undone action");
	}
	g_object_set (action,
		      "label", label,
		      "tooltip", tooltip,
		      NULL);
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
	gtk_action_set_sensitive (action, available);
	G_GNUC_END_IGNORE_DEPRECATIONS;
}
