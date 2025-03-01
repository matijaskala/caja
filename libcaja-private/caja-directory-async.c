/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   caja-directory-async.c: Caja directory model state machine.

   Copyright (C) 1999, 2000, 2001 Eazel, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public
   License along with this program; if not, write to the
   Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
   Boston, MA 02110-1301, USA.

   Author: Darin Adler <darin@bentspoon.com>
*/

#include <config.h>
#include <gtk/gtk.h>
#include <libxml/parser.h>
#include <stdio.h>
#include <stdlib.h>

#include <eel/eel-glib-extensions.h>

#include "caja-directory-notify.h"
#include "caja-directory-private.h"
#include "caja-file-attributes.h"
#include "caja-file-private.h"
#include "caja-file-utilities.h"
#include "caja-metadata.h"
#include "caja-signaller.h"
#include "caja-global-preferences.h"
#include "caja-link.h"
#include "caja-marshal.h"

/* turn this on to see messages about each load_directory call: */
#if 0
#define DEBUG_LOAD_DIRECTORY
#endif

/* turn this on to check if async. job calls are balanced */
#if 0
#define DEBUG_ASYNC_JOBS
#endif

/* turn this on to log things starting and stopping */
#if 0
#define DEBUG_START_STOP
#endif

#define DIRECTORY_LOAD_ITEMS_PER_CALLBACK 100

/* Keep async. jobs down to this number for all directories. */
#define MAX_ASYNC_JOBS 10

struct TopLeftTextReadState
{
    CajaDirectory *directory;
    CajaFile *file;
    gboolean large;
    GCancellable *cancellable;
};

struct LinkInfoReadState
{
    CajaDirectory *directory;
    GCancellable *cancellable;
    CajaFile *file;
};

struct ThumbnailState
{
    CajaDirectory *directory;
    GCancellable *cancellable;
    CajaFile *file;
    gboolean trying_original;
    gboolean tried_original;
};

struct MountState
{
    CajaDirectory *directory;
    GCancellable *cancellable;
    CajaFile *file;
};

struct FilesystemInfoState
{
    CajaDirectory *directory;
    GCancellable *cancellable;
    CajaFile *file;
};

struct DirectoryLoadState
{
    CajaDirectory *directory;
    GCancellable *cancellable;
    GFileEnumerator *enumerator;
    GHashTable *load_mime_list_hash;
    CajaFile *load_directory_file;
    int load_file_count;
};

struct MimeListState
{
    CajaDirectory *directory;
    CajaFile *mime_list_file;
    GCancellable *cancellable;
    GFileEnumerator *enumerator;
    GHashTable *mime_list_hash;
};

struct GetInfoState
{
    CajaDirectory *directory;
    GCancellable *cancellable;
};

struct NewFilesState
{
    CajaDirectory *directory;
    GCancellable *cancellable;
    int count;
};

struct DirectoryCountState
{
    CajaDirectory *directory;
    CajaFile *count_file;
    GCancellable *cancellable;
    GFileEnumerator *enumerator;
    int file_count;
};

struct DeepCountState
{
    CajaDirectory *directory;
    GCancellable *cancellable;
    GFileEnumerator *enumerator;
    GFile *deep_count_location;
    GList *deep_count_subdirectories;
    GArray *seen_deep_count_inodes;
    char *fs_id;
};

typedef struct
{
    CajaFile *file; /* Which file, NULL means all. */
    union
    {
        CajaDirectoryCallback directory;
        CajaFileCallback file;
    } callback;
    gpointer callback_data;
    Request request;
    gboolean active; /* Set to FALSE when the callback is triggered and
			  * scheduled to be called at idle, its still kept
			  * in the list so we can kill it when the file
			  * goes away.
			  */
} ReadyCallback;

typedef struct
{
    CajaFile *file; /* Which file, NULL means all. */
    gboolean monitor_hidden_files; /* defines whether "all" includes hidden files */
    gconstpointer client;
    Request request;
} Monitor;

typedef struct
{
    CajaDirectory *directory;
    CajaInfoProvider *provider;
    CajaOperationHandle *handle;
    CajaOperationResult result;
} InfoProviderResponse;

typedef gboolean (* RequestCheck) (Request);
typedef gboolean (* FileCheck) (CajaFile *);

/* Current number of async. jobs. */
static int async_job_count;
static GHashTable *waiting_directories;
#ifdef DEBUG_ASYNC_JOBS
static GHashTable *async_jobs;
#endif

/* Forward declarations for functions that need them. */
static void     deep_count_load                               (DeepCountState         *state,
        GFile                  *location);
static gboolean request_is_satisfied                          (CajaDirectory      *directory,
        CajaFile           *file,
        Request                 request);
static void     cancel_loading_attributes                     (CajaDirectory      *directory,
        CajaFileAttributes  file_attributes);
static void     add_all_files_to_work_queue                   (CajaDirectory      *directory);
static void     link_info_done                                (CajaDirectory      *directory,
        CajaFile           *file,
        const char             *uri,
        const char             *name,
        const char             *icon,
        gboolean                is_launcher,
        gboolean                is_foreign);
static void     move_file_to_low_priority_queue               (CajaDirectory      *directory,
        CajaFile           *file);
static void     move_file_to_extension_queue                  (CajaDirectory      *directory,
        CajaFile           *file);
static void     caja_directory_invalidate_file_attributes (CajaDirectory      *directory,
        CajaFileAttributes  file_attributes);

/* Some helpers for case-insensitive strings.
 * Move to caja-glib-extensions?
 */

static gboolean
istr_equal (gconstpointer v, gconstpointer v2)
{
    return g_ascii_strcasecmp (v, v2) == 0;
}

static guint
istr_hash (gconstpointer key)
{
    const char *p;
    guint h;

    h = 0;
    for (p = key; *p != '\0'; p++)
    {
        h = (h << 5) - h + g_ascii_tolower (*p);
    }

    return h;
}

static GHashTable *
istr_set_new (void)
{
    return g_hash_table_new_full (istr_hash, istr_equal, g_free, NULL);
}

static void
add_istr_to_list (gpointer key, gpointer value, gpointer callback_data)
{
    GList **list;

    list = callback_data;
    *list = g_list_prepend (*list, g_strdup (key));
}

static GList *
istr_set_get_as_list (GHashTable *table)
{
    GList *list;

    list = NULL;
    g_hash_table_foreach (table, add_istr_to_list, &list);
    return list;
}

static void
istr_set_destroy (GHashTable *table)
{
    g_hash_table_destroy (table);
}

static void
request_counter_add_request (RequestCounter counter,
                             Request request)
{
    guint i;

    for (i = 0; i < REQUEST_TYPE_LAST; i++)
    {
        if (REQUEST_WANTS_TYPE (request, i))
        {
            counter[i]++;
        }
    }
}

static void
request_counter_remove_request (RequestCounter counter,
                                Request request)
{
    guint i;

    for (i = 0; i < REQUEST_TYPE_LAST; i++)
    {
        if (REQUEST_WANTS_TYPE (request, i))
        {
            counter[i]--;
        }
    }
}

#if 0
static void
caja_directory_verify_request_counts (CajaDirectory *directory)
{
    GList *l;
    RequestCounter counters;
    int i;
    gboolean fail;

    fail = FALSE;
    for (i = 0; i < REQUEST_TYPE_LAST; i ++)
    {
        counters[i] = 0;
    }
    for (l = directory->details->monitor_list; l != NULL; l = l->next)
    {
        Monitor *monitor = l->data;
        request_counter_add_request (counters, monitor->request);
    }
    for (i = 0; i < REQUEST_TYPE_LAST; i ++)
    {
        if (counters[i] != directory->details->monitor_counters[i])
        {
            g_warning ("monitor counter for %i is wrong, expecting %d but found %d",
                       i, counters[i], directory->details->monitor_counters[i]);
            fail = TRUE;
        }
    }
    for (i = 0; i < REQUEST_TYPE_LAST; i ++)
    {
        counters[i] = 0;
    }
    for (l = directory->details->call_when_ready_list; l != NULL; l = l->next)
    {
        ReadyCallback *callback = l->data;
        request_counter_add_request (counters, callback->request);
    }
    for (i = 0; i < REQUEST_TYPE_LAST; i ++)
    {
        if (counters[i] != directory->details->call_when_ready_counters[i])
        {
            g_warning ("call when ready counter for %i is wrong, expecting %d but found %d",
                       i, counters[i], directory->details->call_when_ready_counters[i]);
            fail = TRUE;
        }
    }
    g_assert (!fail);
}
#endif

/* Start a job. This is really just a way of limiting the number of
 * async. requests that we issue at any given time. Without this, the
 * number of requests is unbounded.
 */
static gboolean
async_job_start (CajaDirectory *directory,
                 const char *job)
{
#ifdef DEBUG_ASYNC_JOBS
    char *key;
#endif

#ifdef DEBUG_START_STOP
    g_message ("starting %s in %p", job, directory->details->location);
#endif

    g_assert (async_job_count >= 0);
    g_assert (async_job_count <= MAX_ASYNC_JOBS);

    if (async_job_count >= MAX_ASYNC_JOBS)
    {
        if (waiting_directories == NULL)
        {
            waiting_directories = eel_g_hash_table_new_free_at_exit
                                  (NULL, NULL,
                                   "caja-directory-async.c: waiting_directories");
        }

        g_hash_table_insert (waiting_directories,
                             directory,
                             directory);

        return FALSE;
    }

#ifdef DEBUG_ASYNC_JOBS
    {
        char *uri;
        if (async_jobs == NULL)
        {
            async_jobs = eel_g_hash_table_new_free_at_exit
                         (g_str_hash, g_str_equal,
                          "caja-directory-async.c: async_jobs");
        }
        uri = caja_directory_get_uri (directory);
        key = g_strconcat (uri, ": ", job, NULL);
        if (g_hash_table_lookup (async_jobs, key) != NULL)
        {
            g_warning ("same job twice: %s in %s",
                       job, uri);
        }
        g_free (uri);
        g_hash_table_insert (async_jobs, key, directory);
    }
#endif

    async_job_count += 1;
    return TRUE;
}

/* End a job. */
static void
async_job_end (CajaDirectory *directory,
               const char *job)
{
#ifdef DEBUG_ASYNC_JOBS
    char *key;
    gpointer table_key, value;
#endif

#ifdef DEBUG_START_STOP
    g_message ("stopping %s in %p", job, directory->details->location);
#endif

    g_assert (async_job_count > 0);

#ifdef DEBUG_ASYNC_JOBS
    {
        char *uri;
        uri = caja_directory_get_uri (directory);
        g_assert (async_jobs != NULL);
        key = g_strconcat (uri, ": ", job, NULL);
        if (!g_hash_table_lookup_extended (async_jobs, key, &table_key, &value))
        {
            g_warning ("ending job we didn't start: %s in %s",
                       job, uri);
        }
        else
        {
            g_hash_table_remove (async_jobs, key);
            g_free (table_key);
        }
        g_free (uri);
        g_free (key);
    }
#endif

    async_job_count -= 1;
}

/* Helper to get one value from a hash table. */
static void
get_one_value_callback (gpointer key, gpointer value, gpointer callback_data)
{
    gpointer *returned_value;

    returned_value = callback_data;
    *returned_value = value;
}

/* return a single value from a hash table. */
static gpointer
get_one_value (GHashTable *table)
{
    gpointer value;

    value = NULL;
    if (table != NULL)
    {
        g_hash_table_foreach (table, get_one_value_callback, &value);
    }
    return value;
}

/* Wake up directories that are "blocked" as long as there are job
 * slots available.
 */
static void
async_job_wake_up (void)
{
    static gboolean already_waking_up = FALSE;
    gpointer value;

    g_assert (async_job_count >= 0);
    g_assert (async_job_count <= MAX_ASYNC_JOBS);

    if (already_waking_up)
    {
        return;
    }

    already_waking_up = TRUE;
    while (async_job_count < MAX_ASYNC_JOBS)
    {
        value = get_one_value (waiting_directories);
        if (value == NULL)
        {
            break;
        }
        g_hash_table_remove (waiting_directories, value);
        caja_directory_async_state_changed
        (CAJA_DIRECTORY (value));
    }
    already_waking_up = FALSE;
}

static void
directory_count_cancel (CajaDirectory *directory)
{
    if (directory->details->count_in_progress != NULL)
    {
        g_cancellable_cancel (directory->details->count_in_progress->cancellable);
    }
}

static void
deep_count_cancel (CajaDirectory *directory)
{
    if (directory->details->deep_count_in_progress != NULL)
    {
        g_assert (CAJA_IS_FILE (directory->details->deep_count_file));

        g_cancellable_cancel (directory->details->deep_count_in_progress->cancellable);

        directory->details->deep_count_file->details->deep_counts_status = CAJA_REQUEST_NOT_STARTED;

        directory->details->deep_count_in_progress->directory = NULL;
        directory->details->deep_count_in_progress = NULL;
        directory->details->deep_count_file = NULL;

        async_job_end (directory, "deep count");
    }
}

static void
mime_list_cancel (CajaDirectory *directory)
{
    if (directory->details->mime_list_in_progress != NULL)
    {
        g_cancellable_cancel (directory->details->mime_list_in_progress->cancellable);
    }
}

static void
top_left_cancel (CajaDirectory *directory)
{
    if (directory->details->top_left_read_state != NULL)
    {
        g_cancellable_cancel (directory->details->top_left_read_state->cancellable);
        directory->details->top_left_read_state->directory = NULL;
        directory->details->top_left_read_state = NULL;

        async_job_end (directory, "top left");
    }
}

static void
link_info_cancel (CajaDirectory *directory)
{
    if (directory->details->link_info_read_state != NULL)
    {
        g_cancellable_cancel (directory->details->link_info_read_state->cancellable);
        directory->details->link_info_read_state->directory = NULL;
        directory->details->link_info_read_state = NULL;
        async_job_end (directory, "link info");
    }
}

static void
thumbnail_cancel (CajaDirectory *directory)
{
    if (directory->details->thumbnail_state != NULL)
    {
        g_cancellable_cancel (directory->details->thumbnail_state->cancellable);
        directory->details->thumbnail_state->directory = NULL;
        directory->details->thumbnail_state = NULL;
        async_job_end (directory, "thumbnail");
    }
}

static void
mount_cancel (CajaDirectory *directory)
{
    if (directory->details->mount_state != NULL)
    {
        g_cancellable_cancel (directory->details->mount_state->cancellable);
        directory->details->mount_state->directory = NULL;
        directory->details->mount_state = NULL;
        async_job_end (directory, "mount");
    }
}

static void
file_info_cancel (CajaDirectory *directory)
{
    if (directory->details->get_info_in_progress != NULL)
    {
        g_cancellable_cancel (directory->details->get_info_in_progress->cancellable);
        directory->details->get_info_in_progress->directory = NULL;
        directory->details->get_info_in_progress = NULL;
        directory->details->get_info_file = NULL;

        async_job_end (directory, "file info");
    }
}

static void
new_files_cancel (CajaDirectory *directory)
{
    GList *l;

    if (directory->details->new_files_in_progress != NULL)
    {
        NewFilesState *state = NULL;

        for (l = directory->details->new_files_in_progress; l != NULL; l = l->next)
        {
            state = l->data;
            g_cancellable_cancel (state->cancellable);
            state->directory = NULL;
        }
        g_list_free (directory->details->new_files_in_progress);
        directory->details->new_files_in_progress = NULL;
    }
}

static int
monitor_key_compare (gconstpointer a,
                     gconstpointer data)
{
    const Monitor *monitor;
    const Monitor *compare_monitor;

    monitor = a;
    compare_monitor = data;

    if (monitor->client < compare_monitor->client)
    {
        return -1;
    }
    if (monitor->client > compare_monitor->client)
    {
        return +1;
    }

    if (monitor->file < compare_monitor->file)
    {
        return -1;
    }
    if (monitor->file > compare_monitor->file)
    {
        return +1;
    }

    return 0;
}

static GList *
find_monitor (CajaDirectory *directory,
              CajaFile *file,
              gconstpointer client)
{
    Monitor monitor;

    monitor.client = client;
    monitor.file = file;

    return g_list_find_custom (directory->details->monitor_list,
                               &monitor,
                               monitor_key_compare);
}

static void
remove_monitor_link (CajaDirectory *directory,
                     GList *link)
{
    if (link != NULL)
    {
        Monitor *monitor;

        monitor = link->data;
        request_counter_remove_request (directory->details->monitor_counters,
                                        monitor->request);
        directory->details->monitor_list =
            g_list_remove_link (directory->details->monitor_list, link);
        g_free (monitor);
        g_list_free_1 (link);
    }
}

static void
remove_monitor (CajaDirectory *directory,
                CajaFile *file,
                gconstpointer client)
{
    remove_monitor_link (directory, find_monitor (directory, file, client));
}

Request
caja_directory_set_up_request (CajaFileAttributes file_attributes)
{
    Request request;

    request = 0;

    if ((file_attributes & CAJA_FILE_ATTRIBUTE_DIRECTORY_ITEM_COUNT) != 0)
    {
        REQUEST_SET_TYPE (request, REQUEST_DIRECTORY_COUNT);
    }

    if ((file_attributes & CAJA_FILE_ATTRIBUTE_DEEP_COUNTS) != 0)
    {
        REQUEST_SET_TYPE (request, REQUEST_DEEP_COUNT);
    }

    if ((file_attributes & CAJA_FILE_ATTRIBUTE_DIRECTORY_ITEM_MIME_TYPES) != 0)
    {
        REQUEST_SET_TYPE (request, REQUEST_MIME_LIST);
    }
    if ((file_attributes & CAJA_FILE_ATTRIBUTE_INFO) != 0)
    {
        REQUEST_SET_TYPE (request, REQUEST_FILE_INFO);
    }

    if (file_attributes & CAJA_FILE_ATTRIBUTE_LINK_INFO)
    {
        REQUEST_SET_TYPE (request, REQUEST_FILE_INFO);
        REQUEST_SET_TYPE (request, REQUEST_LINK_INFO);
    }

    if (file_attributes & CAJA_FILE_ATTRIBUTE_TOP_LEFT_TEXT)
    {
        REQUEST_SET_TYPE (request, REQUEST_TOP_LEFT_TEXT);
        REQUEST_SET_TYPE (request, REQUEST_FILE_INFO);
    }

    if (file_attributes & CAJA_FILE_ATTRIBUTE_LARGE_TOP_LEFT_TEXT)
    {
        REQUEST_SET_TYPE (request, REQUEST_LARGE_TOP_LEFT_TEXT);
        REQUEST_SET_TYPE (request, REQUEST_FILE_INFO);
    }

    if ((file_attributes & CAJA_FILE_ATTRIBUTE_EXTENSION_INFO) != 0)
    {
        REQUEST_SET_TYPE (request, REQUEST_EXTENSION_INFO);
    }

    if (file_attributes & CAJA_FILE_ATTRIBUTE_THUMBNAIL)
    {
        REQUEST_SET_TYPE (request, REQUEST_THUMBNAIL);
        REQUEST_SET_TYPE (request, REQUEST_FILE_INFO);
    }

    if (file_attributes & CAJA_FILE_ATTRIBUTE_MOUNT)
    {
        REQUEST_SET_TYPE (request, REQUEST_MOUNT);
        REQUEST_SET_TYPE (request, REQUEST_FILE_INFO);
    }

    if (file_attributes & CAJA_FILE_ATTRIBUTE_FILESYSTEM_INFO)
    {
        REQUEST_SET_TYPE (request, REQUEST_FILESYSTEM_INFO);
    }

    return request;
}

static void
mime_db_changed_callback (GObject *ignore, CajaDirectory *dir)
{
    CajaFileAttributes attrs;

    g_assert (dir != NULL);
    g_assert (dir->details != NULL);

    attrs = CAJA_FILE_ATTRIBUTE_INFO |
            CAJA_FILE_ATTRIBUTE_LINK_INFO |
            CAJA_FILE_ATTRIBUTE_DIRECTORY_ITEM_MIME_TYPES;

    caja_directory_force_reload_internal (dir, attrs);
}

void
caja_directory_monitor_add_internal (CajaDirectory *directory,
                                     CajaFile *file,
                                     gconstpointer client,
                                     gboolean monitor_hidden_files,
                                     CajaFileAttributes file_attributes,
                                     CajaDirectoryCallback callback,
                                     gpointer callback_data)
{
    Monitor *monitor;

    g_assert (CAJA_IS_DIRECTORY (directory));

    /* Replace any current monitor for this client/file pair. */
    remove_monitor (directory, file, client);

    /* Add the new monitor. */
    monitor = g_new (Monitor, 1);
    monitor->file = file;
    monitor->monitor_hidden_files = monitor_hidden_files;
    monitor->client = client;
    monitor->request = caja_directory_set_up_request (file_attributes);

    if (file == NULL)
    {
        REQUEST_SET_TYPE (monitor->request, REQUEST_FILE_LIST);
    }
    directory->details->monitor_list =
        g_list_prepend (directory->details->monitor_list, monitor);
    request_counter_add_request (directory->details->monitor_counters,
                                 monitor->request);

    if (callback != NULL)
    {
        GList *file_list;

        file_list = caja_directory_get_file_list (directory);
        (* callback) (directory, file_list, callback_data);
        caja_file_list_free (file_list);
    }

    /* Start the "real" monitoring (FAM or whatever). */
    /* We always monitor the whole directory since in practice
     * caja almost always shows the whole directory anyway, and
     * it allows us to avoid one file monitor per file in a directory.
     */
    if (directory->details->monitor == NULL)
    {
        directory->details->monitor = caja_monitor_directory (directory->details->location);
    }

    if (REQUEST_WANTS_TYPE (monitor->request, REQUEST_FILE_INFO) &&
            directory->details->mime_db_monitor == 0)
    {
        directory->details->mime_db_monitor =
            g_signal_connect_object (caja_signaller_get_current (),
                                     "mime_data_changed",
                                     G_CALLBACK (mime_db_changed_callback), directory, 0);
    }

    /* Put the monitor file or all the files on the work queue. */
    if (file != NULL)
    {
        caja_directory_add_file_to_work_queue (directory, file);
    }
    else
    {
        add_all_files_to_work_queue (directory);
    }

    /* Kick off I/O. */
    caja_directory_async_state_changed (directory);
}

static void
set_file_unconfirmed (CajaFile *file, gboolean unconfirmed)
{
    CajaDirectory *directory;

    g_assert (CAJA_IS_FILE (file));
    g_assert (unconfirmed == FALSE || unconfirmed == TRUE);

    if (file->details->unconfirmed == unconfirmed)
    {
        return;
    }
    file->details->unconfirmed = unconfirmed;

    directory = file->details->directory;
    if (unconfirmed)
    {
        directory->details->confirmed_file_count--;
    }
    else
    {
        directory->details->confirmed_file_count++;
    }
}

static gboolean show_hidden_files = TRUE;

static void
show_hidden_files_changed_callback (gpointer callback_data)
{
    show_hidden_files = g_settings_get_boolean (caja_preferences, CAJA_PREFERENCES_SHOW_HIDDEN_FILES);
}

static gboolean
should_skip_file (CajaDirectory *directory, GFileInfo *info)
{
    static gboolean show_hidden_files_changed_callback_installed = FALSE;

    /* Add the callback once for the life of our process */
    if (!show_hidden_files_changed_callback_installed)
    {
        g_signal_connect_swapped (caja_preferences,
                                  "changed::" CAJA_PREFERENCES_SHOW_HIDDEN_FILES,
                                  G_CALLBACK(show_hidden_files_changed_callback),
                                  NULL);
        show_hidden_files_changed_callback_installed = TRUE;

        /* Peek for the first time */
        show_hidden_files_changed_callback (NULL);
    }

    if (!show_hidden_files && g_file_info_get_is_hidden (info))
    {
        return TRUE;
    }

    return FALSE;
}

static gboolean
dequeue_pending_idle_callback (gpointer callback_data)
{
    CajaDirectory *directory;
    GList *pending_file_info;
    GList *node, *next;
    CajaFile *file;
    GList *changed_files, *added_files;
    GFileInfo *file_info;
    const char *mimetype, *name;
    DirectoryLoadState *dir_load_state;

    directory = CAJA_DIRECTORY (callback_data);

    caja_directory_ref (directory);

    directory->details->dequeue_pending_idle_id = 0;

    /* Handle the files in the order we saw them. */
    pending_file_info = g_list_reverse (directory->details->pending_file_info);
    directory->details->pending_file_info = NULL;

    /* If we are no longer monitoring, then throw away these. */
    if (!caja_directory_is_file_list_monitored (directory))
    {
        caja_directory_async_state_changed (directory);
        goto drain;
    }

    added_files = NULL;
    changed_files = NULL;

    dir_load_state = directory->details->directory_load_in_progress;

    /* Build a list of CajaFile objects. */
    for (node = pending_file_info; node != NULL; node = node->next)
    {
        file_info = node->data;

        name = g_file_info_get_name (file_info);

        /* Update the file count. */
        /* FIXME bugzilla.gnome.org 45063: This could count a
         * file twice if we get it from both load_directory
         * and from new_files_callback. Not too hard to fix by
         * moving this into the actual callback instead of
         * waiting for the idle function.
         */
        if (dir_load_state && !should_skip_file (directory, file_info))
        {
            dir_load_state->load_file_count += 1;

            /* Add the MIME type to the set. */
            if ((mimetype = g_file_info_get_content_type (file_info)) != NULL)
            {
                g_hash_table_add (dir_load_state->load_mime_list_hash,
                                  g_strdup (mimetype));
            }
        }

        /* check if the file already exists */
        file = caja_directory_find_file_by_name (directory, name);
        if (file != NULL)
        {
            /* file already exists in dir, check if we still need to
             *  emit file_added or if it changed */
            set_file_unconfirmed (file, FALSE);
            if (!file->details->is_added)
            {
                /* We consider this newly added even if its in the list.
                 * This can happen if someone called caja_file_get_by_uri()
                 * on a file in the folder before the add signal was
                 * emitted */
                caja_file_ref (file);
                file->details->is_added = TRUE;
                added_files = g_list_prepend (added_files, file);
            }
            else if (caja_file_update_info (file, file_info))
            {
                /* File changed, notify about the change. */
                caja_file_ref (file);
                changed_files = g_list_prepend (changed_files, file);
            }
        }
        else
        {
            /* new file, create a caja file object and add it to the list */
            file = caja_file_new_from_info (directory, file_info);
            caja_directory_add_file (directory, file);
            file->details->is_added = TRUE;
            added_files = g_list_prepend (added_files, file);
        }
    }

    /* If we are done loading, then we assume that any unconfirmed
         * files are gone.
     */
    if (directory->details->directory_loaded)
    {
        for (node = directory->details->file_list;
                node != NULL; node = next)
        {
            file = CAJA_FILE (node->data);
            next = node->next;

            if (file->details->unconfirmed)
            {
                caja_file_ref (file);
                changed_files = g_list_prepend (changed_files, file);

                caja_file_mark_gone (file);
            }
        }
    }

    /* Send the changed and added signals. */
    caja_directory_emit_change_signals (directory, changed_files);
    caja_file_list_free (changed_files);
    caja_directory_emit_files_added (directory, added_files);
    caja_file_list_free (added_files);

    if (directory->details->directory_loaded &&
            !directory->details->directory_loaded_sent_notification)
    {
        /* Send the done_loading signal. */
        caja_directory_emit_done_loading (directory);

        if (dir_load_state)
        {
            file = dir_load_state->load_directory_file;

            file->details->directory_count = dir_load_state->load_file_count;
            file->details->directory_count_is_up_to_date = TRUE;
            file->details->got_directory_count = TRUE;

            file->details->got_mime_list = TRUE;
            file->details->mime_list_is_up_to_date = TRUE;
            g_list_free_full (file->details->mime_list, g_free);
            file->details->mime_list = istr_set_get_as_list
                                       (dir_load_state->load_mime_list_hash);

            caja_file_changed (file);
        }

        caja_directory_async_state_changed (directory);

        directory->details->directory_loaded_sent_notification = TRUE;
    }

drain:
    g_list_free_full (pending_file_info, g_object_unref);

    /* Get the state machine running again. */
    caja_directory_async_state_changed (directory);

    caja_directory_unref (directory);
    return FALSE;
}

void
caja_directory_schedule_dequeue_pending (CajaDirectory *directory)
{
    if (directory->details->dequeue_pending_idle_id == 0)
    {
        directory->details->dequeue_pending_idle_id
            = g_idle_add (dequeue_pending_idle_callback, directory);
    }
}

static void
directory_load_one (CajaDirectory *directory,
                    GFileInfo *info)
{
    if (info == NULL)
    {
        return;
    }

    if (g_file_info_get_name (info) == NULL)
    {
        char *uri;

        uri = caja_directory_get_uri (directory);
        g_warning ("Got GFileInfo with NULL name in %s, ignoring. This shouldn't happen unless the gvfs backend is broken.\n", uri);
        g_free (uri);

        return;
    }

    /* Arrange for the "loading" part of the work. */
    g_object_ref (info);
    directory->details->pending_file_info
        = g_list_prepend (directory->details->pending_file_info, info);
    caja_directory_schedule_dequeue_pending (directory);
}

static void
directory_load_cancel (CajaDirectory *directory)
{
    DirectoryLoadState *state;

    state = directory->details->directory_load_in_progress;
    if (state != NULL)
    {
        CajaFile *file;

        file = state->load_directory_file;
        file->details->loading_directory = FALSE;
        if (file->details->directory != directory)
        {
            caja_directory_async_state_changed (file->details->directory);
        }

        g_cancellable_cancel (state->cancellable);
        state->directory = NULL;
        directory->details->directory_load_in_progress = NULL;
        async_job_end (directory, "file list");
    }
}

static void
file_list_cancel (CajaDirectory *directory)
{
    directory_load_cancel (directory);

    if (directory->details->dequeue_pending_idle_id != 0)
    {
        g_source_remove (directory->details->dequeue_pending_idle_id);
        directory->details->dequeue_pending_idle_id = 0;
    }

    if (directory->details->pending_file_info != NULL)
    {
	g_list_free_full (directory->details->pending_file_info, g_object_unref);
        directory->details->pending_file_info = NULL;
    }
}

static void
directory_load_done (CajaDirectory *directory,
                     GError *error)
{
    GList *node;

    directory->details->directory_loaded = TRUE;
    directory->details->directory_loaded_sent_notification = FALSE;

    if (error != NULL)
    {
        /* The load did not complete successfully. This means
         * we don't know the status of the files in this directory.
         * We clear the unconfirmed bit on each file here so that
         * they won't be marked "gone" later -- we don't know enough
         * about them to know whether they are really gone.
         */
        for (node = directory->details->file_list;
                node != NULL; node = node->next)
        {
            set_file_unconfirmed (CAJA_FILE (node->data), FALSE);
        }

        caja_directory_emit_load_error (directory, error);
    }

    /* Call the idle function right away. */
    if (directory->details->dequeue_pending_idle_id != 0)
    {
        g_source_remove (directory->details->dequeue_pending_idle_id);
    }
    dequeue_pending_idle_callback (directory);

    directory_load_cancel (directory);
}

void
caja_directory_monitor_remove_internal (CajaDirectory *directory,
                                        CajaFile *file,
                                        gconstpointer client)
{
    g_assert (CAJA_IS_DIRECTORY (directory));
    g_assert (file == NULL || CAJA_IS_FILE (file));
    g_assert (client != NULL);

    remove_monitor (directory, file, client);

    if (directory->details->monitor != NULL
            && directory->details->monitor_list == NULL)
    {
        caja_monitor_cancel (directory->details->monitor);
        directory->details->monitor = NULL;
    }

    /* XXX - do we need to remove anything from the work queue? */

    caja_directory_async_state_changed (directory);
}

FileMonitors *
caja_directory_remove_file_monitors (CajaDirectory *directory,
                                     CajaFile *file)
{
    GList *result, **list, *node, *next;
    Monitor *monitor = NULL;

    g_assert (CAJA_IS_DIRECTORY (directory));
    g_assert (CAJA_IS_FILE (file));
    g_assert (file->details->directory == directory);

    result = NULL;

    list = &directory->details->monitor_list;
    for (node = directory->details->monitor_list; node != NULL; node = next)
    {
        next = node->next;
        monitor = node->data;

        if (monitor->file == file)
        {
            *list = g_list_remove_link (*list, node);
            result = g_list_concat (node, result);
            request_counter_remove_request (directory->details->monitor_counters,
                                            monitor->request);
        }
    }

    /* XXX - do we need to remove anything from the work queue? */

    caja_directory_async_state_changed (directory);

    return (FileMonitors *) result;
}

void
caja_directory_add_file_monitors (CajaDirectory *directory,
                                  CajaFile *file,
                                  FileMonitors *monitors)
{
    GList **list;
    GList *l;
    Monitor *monitor = NULL;

    g_assert (CAJA_IS_DIRECTORY (directory));
    g_assert (CAJA_IS_FILE (file));
    g_assert (file->details->directory == directory);

    if (monitors == NULL)
    {
        return;
    }

    for (l = (GList *)monitors; l != NULL; l = l->next)
    {
        monitor = l->data;
        request_counter_add_request (directory->details->monitor_counters,
                                     monitor->request);
    }

    list = &directory->details->monitor_list;
    *list = g_list_concat (*list, (GList *) monitors);

    caja_directory_add_file_to_work_queue (directory, file);

    caja_directory_async_state_changed (directory);
}

static int
ready_callback_key_compare (gconstpointer a, gconstpointer b)
{
    const ReadyCallback *callback_a, *callback_b;

    callback_a = a;
    callback_b = b;

    if (callback_a->file < callback_b->file)
    {
        return -1;
    }
    if (callback_a->file > callback_b->file)
    {
        return 1;
    }
    if (callback_a->file == NULL)
    {
        /* ANSI C doesn't allow ordered compares of function pointers, so we cast them to
         * normal pointers to make some overly pedantic compilers (*cough* HP-UX *cough*)
         * compile this. Of course, on any compiler where ordered function pointers actually
         * break this probably won't work, but at least it will compile on platforms where it
         * works, but stupid compilers won't let you use it.
         */
        if ((void *)callback_a->callback.directory < (void *)callback_b->callback.directory)
        {
            return -1;
        }
        if ((void *)callback_a->callback.directory > (void *)callback_b->callback.directory)
        {
            return 1;
        }
    }
    else
    {
        if ((void *)callback_a->callback.file < (void *)callback_b->callback.file)
        {
            return -1;
        }
        if ((void *)callback_a->callback.file > (void *)callback_b->callback.file)
        {
            return 1;
        }
    }
    if (callback_a->callback_data < callback_b->callback_data)
    {
        return -1;
    }
    if (callback_a->callback_data > callback_b->callback_data)
    {
        return 1;
    }
    return 0;
}

static int
ready_callback_key_compare_only_active (gconstpointer a, gconstpointer b)
{
    const ReadyCallback *callback_a;

    callback_a = a;

    /* Non active callbacks never match */
    if (!callback_a->active)
    {
        return -1;
    }

    return ready_callback_key_compare (a, b);
}

static void
ready_callback_call (CajaDirectory *directory,
                     const ReadyCallback *callback)
{
    GList *file_list;

    /* Call the callback. */
    if (callback->file != NULL)
    {
        if (callback->callback.file)
        {
            (* callback->callback.file) (callback->file,
                                         callback->callback_data);
        }
    }
    else if (callback->callback.directory != NULL)
    {
        if (directory == NULL ||
                !REQUEST_WANTS_TYPE (callback->request, REQUEST_FILE_LIST))
        {
            file_list = NULL;
        }
        else
        {
            file_list = caja_directory_get_file_list (directory);
        }

        /* Pass back the file list if the user was waiting for it. */
        (* callback->callback.directory) (directory,
                                          file_list,
                                          callback->callback_data);

        caja_file_list_free (file_list);
    }
}

void
caja_directory_call_when_ready_internal (CajaDirectory *directory,
        CajaFile *file,
        CajaFileAttributes file_attributes,
        gboolean wait_for_file_list,
        CajaDirectoryCallback directory_callback,
        CajaFileCallback file_callback,
        gpointer callback_data)
{
    ReadyCallback callback;

    g_assert (directory == NULL || CAJA_IS_DIRECTORY (directory));
    g_assert (file == NULL || CAJA_IS_FILE (file));
    g_assert (file != NULL || directory_callback != NULL);

    /* Construct a callback object. */
    callback.active = TRUE;
    callback.file = file;
    if (file == NULL)
    {
        callback.callback.directory = directory_callback;
    }
    else
    {
        callback.callback.file = file_callback;
    }
    callback.callback_data = callback_data;
    callback.request = caja_directory_set_up_request (file_attributes);
    if (wait_for_file_list)
    {
        REQUEST_SET_TYPE (callback.request, REQUEST_FILE_LIST);
    }

    /* Handle the NULL case. */
    if (directory == NULL)
    {
        ready_callback_call (NULL, &callback);
        return;
    }

    /* Check if the callback is already there. */
    if (g_list_find_custom (directory->details->call_when_ready_list,
                            &callback,
                            ready_callback_key_compare_only_active) != NULL)
    {
        if (file_callback != NULL && directory_callback != NULL)
        {
            g_warning ("tried to add a new callback while an old one was pending");
        }
        /* NULL callback means, just read it. Conflicts are ok. */
        return;
    }

    /* Add the new callback to the list. */
    directory->details->call_when_ready_list = g_list_prepend
            (directory->details->call_when_ready_list,
             g_memdup (&callback, sizeof (callback)));
    request_counter_add_request (directory->details->call_when_ready_counters,
                                 callback.request);

    /* Put the callback file or all the files on the work queue. */
    if (file != NULL)
    {
        caja_directory_add_file_to_work_queue (directory, file);
    }
    else
    {
        add_all_files_to_work_queue (directory);
    }

    caja_directory_async_state_changed (directory);
}

gboolean
caja_directory_check_if_ready_internal (CajaDirectory *directory,
                                        CajaFile *file,
                                        CajaFileAttributes file_attributes)
{
    Request request;

    g_assert (CAJA_IS_DIRECTORY (directory));

    request = caja_directory_set_up_request (file_attributes);
    return request_is_satisfied (directory, file, request);
}

static void
remove_callback_link_keep_data (CajaDirectory *directory,
                                GList *link)
{
    ReadyCallback *callback;

    callback = link->data;

    directory->details->call_when_ready_list = g_list_remove_link
            (directory->details->call_when_ready_list, link);

    request_counter_remove_request (directory->details->call_when_ready_counters,
                                    callback->request);
    g_list_free_1 (link);
}

static void
remove_callback_link (CajaDirectory *directory,
                      GList *link)
{
    ReadyCallback *callback;

    callback = link->data;
    remove_callback_link_keep_data (directory, link);
    g_free (callback);
}

void
caja_directory_cancel_callback_internal (CajaDirectory *directory,
        CajaFile *file,
        CajaDirectoryCallback directory_callback,
        CajaFileCallback file_callback,
        gpointer callback_data)
{
    ReadyCallback callback;
    GList *node;

    if (directory == NULL)
    {
        return;
    }

    g_assert (CAJA_IS_DIRECTORY (directory));
    g_assert (file == NULL || CAJA_IS_FILE (file));
    g_assert (file != NULL || directory_callback != NULL);
    g_assert (file == NULL || file_callback != NULL);

    /* Construct a callback object. */
    callback.file = file;
    if (file == NULL)
    {
        callback.callback.directory = directory_callback;
    }
    else
    {
        callback.callback.file = file_callback;
    }
    callback.callback_data = callback_data;

    /* Remove all queued callback from the list (including non-active). */
    do
    {
        node = g_list_find_custom (directory->details->call_when_ready_list,
                                   &callback,
                                   ready_callback_key_compare);
        if (node != NULL)
        {
            remove_callback_link (directory, node);

            caja_directory_async_state_changed (directory);
        }
    }
    while (node != NULL);
}

static void
new_files_state_unref (NewFilesState *state)
{
    state->count--;

    if (state->count == 0)
    {
        if (state->directory)
        {
            state->directory->details->new_files_in_progress =
                g_list_remove (state->directory->details->new_files_in_progress,
                               state);
        }

        g_object_unref (state->cancellable);
        g_free (state);
    }
}

static void
new_files_callback (GObject *source_object,
                    GAsyncResult *res,
                    gpointer user_data)
{
    CajaDirectory *directory;
    GFileInfo *info;
    NewFilesState *state;

    state = user_data;

    if (state->directory == NULL)
    {
        /* Operation was cancelled. Bail out */
        new_files_state_unref (state);
        return;
    }

    directory = caja_directory_ref (state->directory);

    /* Queue up the new file. */
    info = g_file_query_info_finish (G_FILE (source_object), res, NULL);
    if (info != NULL)
    {
        directory_load_one (directory, info);
        g_object_unref (info);
    }

    new_files_state_unref (state);

    caja_directory_unref (directory);
}

void
caja_directory_get_info_for_new_files (CajaDirectory *directory,
                                       GList *location_list)
{
    NewFilesState *state;
    GList *l;
    GFile *location = NULL;

    if (location_list == NULL)
    {
        return;
    }

    state = g_new (NewFilesState, 1);
    state->directory = directory;
    state->cancellable = g_cancellable_new ();
    state->count = 0;

    for (l = location_list; l != NULL; l = l->next)
    {
        location = l->data;

        state->count++;

        g_file_query_info_async (location,
                                 CAJA_FILE_DEFAULT_ATTRIBUTES,
                                 0,
                                 G_PRIORITY_DEFAULT,
                                 state->cancellable,
                                 new_files_callback, state);
    }

    directory->details->new_files_in_progress
        = g_list_prepend (directory->details->new_files_in_progress,
                          state);
}

void
caja_async_destroying_file (CajaFile *file)
{
    CajaDirectory *directory;
    gboolean changed;
    GList *node, *next;
    ReadyCallback *callback = NULL;
    Monitor *monitor = NULL;

    directory = file->details->directory;
    changed = FALSE;

    /* Check for callbacks. */
    for (node = directory->details->call_when_ready_list; node != NULL; node = next)
    {
        next = node->next;
        callback = node->data;

        if (callback->file == file)
        {
            /* Client should have cancelled callback. */
            if (callback->active)
            {
                g_warning ("destroyed file has call_when_ready pending");
            }
            remove_callback_link (directory, node);
            changed = TRUE;
        }
    }

    /* Check for monitors. */
    for (node = directory->details->monitor_list; node != NULL; node = next)
    {
        next = node->next;
        monitor = node->data;

        if (monitor->file == file)
        {
            /* Client should have removed monitor earlier. */
            g_warning ("destroyed file still being monitored");
            remove_monitor_link (directory, node);
            changed = TRUE;
        }
    }

    /* Check if it's a file that's currently being worked on.
     * If so, make that NULL so it gets canceled right away.
     */
    if (directory->details->count_in_progress != NULL &&
            directory->details->count_in_progress->count_file == file)
    {
        directory->details->count_in_progress->count_file = NULL;
        changed = TRUE;
    }
    if (directory->details->deep_count_file == file)
    {
        directory->details->deep_count_file = NULL;
        changed = TRUE;
    }
    if (directory->details->mime_list_in_progress != NULL &&
            directory->details->mime_list_in_progress->mime_list_file == file)
    {
        directory->details->mime_list_in_progress->mime_list_file = NULL;
        changed = TRUE;
    }
    if (directory->details->get_info_file == file)
    {
        directory->details->get_info_file = NULL;
        changed = TRUE;
    }
    if (directory->details->top_left_read_state != NULL
            && directory->details->top_left_read_state->file == file)
    {
        directory->details->top_left_read_state->file = NULL;
        changed = TRUE;
    }
    if (directory->details->link_info_read_state != NULL &&
            directory->details->link_info_read_state->file == file)
    {
        directory->details->link_info_read_state->file = NULL;
        changed = TRUE;
    }
    if (directory->details->extension_info_file == file)
    {
        directory->details->extension_info_file = NULL;
        changed = TRUE;
    }

    if (directory->details->thumbnail_state != NULL &&
            directory->details->thumbnail_state->file ==  file)
    {
        directory->details->thumbnail_state->file = NULL;
        changed = TRUE;
    }

    if (directory->details->mount_state != NULL &&
            directory->details->mount_state->file ==  file)
    {
        directory->details->mount_state->file = NULL;
        changed = TRUE;
    }

    if (directory->details->filesystem_info_state != NULL &&
            directory->details->filesystem_info_state->file == file)
    {
        directory->details->filesystem_info_state->file = NULL;
        changed = TRUE;
    }

    /* Let the directory take care of the rest. */
    if (changed)
    {
        caja_directory_async_state_changed (directory);
    }
}

static gboolean
lacks_directory_count (CajaFile *file)
{
    return !file->details->directory_count_is_up_to_date
           && caja_file_should_show_directory_item_count (file);
}

static gboolean
should_get_directory_count_now (CajaFile *file)
{
    return lacks_directory_count (file)
           && !file->details->loading_directory;
}

static gboolean
lacks_top_left (CajaFile *file)
{
    return file->details->file_info_is_up_to_date &&
           !file->details->top_left_text_is_up_to_date
           && caja_file_should_get_top_left_text (file);
}

static gboolean
lacks_large_top_left (CajaFile *file)
{
    return file->details->file_info_is_up_to_date &&
           (!file->details->top_left_text_is_up_to_date ||
            file->details->got_large_top_left_text != file->details->got_top_left_text)
           && caja_file_should_get_top_left_text (file);
}
static gboolean
lacks_info (CajaFile *file)
{
    return !file->details->file_info_is_up_to_date
           && !file->details->is_gone;
}

static gboolean
lacks_filesystem_info (CajaFile *file)
{
    return !file->details->filesystem_info_is_up_to_date;
}

static gboolean
lacks_deep_count (CajaFile *file)
{
    return file->details->deep_counts_status != CAJA_REQUEST_DONE;
}

static gboolean
lacks_mime_list (CajaFile *file)
{
    return !file->details->mime_list_is_up_to_date;
}

static gboolean
should_get_mime_list (CajaFile *file)
{
    return lacks_mime_list (file)
           && !file->details->loading_directory;
}

static gboolean
lacks_link_info (CajaFile *file)
{
    if (file->details->file_info_is_up_to_date &&
            !file->details->link_info_is_up_to_date)
    {
        if (caja_file_is_caja_link (file))
        {
            return TRUE;
        }
        else
        {
            link_info_done (file->details->directory, file, NULL, NULL, NULL, FALSE, FALSE);
            return FALSE;
        }
    }
    else
    {
        return FALSE;
    }
}

static gboolean
lacks_extension_info (CajaFile *file)
{
    return file->details->pending_info_providers != NULL;
}

static gboolean
lacks_thumbnail (CajaFile *file)
{
    return caja_file_should_show_thumbnail (file) &&
           file->details->thumbnail_path != NULL &&
           !file->details->thumbnail_is_up_to_date;
}

static gboolean
lacks_mount (CajaFile *file)
{
    return (!file->details->mount_is_up_to_date &&
            (
                /* Unix mountpoint, could be a GMount */
                file->details->is_mountpoint ||

                /* The toplevel directory of something */
                (file->details->type == G_FILE_TYPE_DIRECTORY &&
                 caja_file_is_self_owned (file)) ||

                /* Mountable, could be a mountpoint */
                (file->details->type == G_FILE_TYPE_MOUNTABLE)

            )
           );
}

static gboolean
has_problem (CajaDirectory *directory, CajaFile *file, FileCheck problem)
{
    GList *node;

    if (file != NULL)
    {
        return (* problem) (file);
    }

    for (node = directory->details->file_list; node != NULL; node = node->next)
    {
        if ((* problem) (node->data))
        {
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
request_is_satisfied (CajaDirectory *directory,
                      CajaFile *file,
                      Request request)
{
    if (REQUEST_WANTS_TYPE (request, REQUEST_FILE_LIST) &&
            !(directory->details->directory_loaded &&
              directory->details->directory_loaded_sent_notification))
    {
        return FALSE;
    }

    if (REQUEST_WANTS_TYPE (request, REQUEST_DIRECTORY_COUNT))
    {
        if (has_problem (directory, file, lacks_directory_count))
        {
            return FALSE;
        }
    }

    if (REQUEST_WANTS_TYPE (request, REQUEST_FILE_INFO))
    {
        if (has_problem (directory, file, lacks_info))
        {
            return FALSE;
        }
    }

    if (REQUEST_WANTS_TYPE (request, REQUEST_FILESYSTEM_INFO))
    {
        if (has_problem (directory, file, lacks_filesystem_info))
        {
            return FALSE;
        }
    }

    if (REQUEST_WANTS_TYPE (request, REQUEST_TOP_LEFT_TEXT))
    {
        if (has_problem (directory, file, lacks_top_left))
        {
            return FALSE;
        }
    }

    if (REQUEST_WANTS_TYPE (request, REQUEST_LARGE_TOP_LEFT_TEXT))
    {
        if (has_problem (directory, file, lacks_large_top_left))
        {
            return FALSE;
        }
    }

    if (REQUEST_WANTS_TYPE (request, REQUEST_DEEP_COUNT))
    {
        if (has_problem (directory, file, lacks_deep_count))
        {
            return FALSE;
        }
    }

    if (REQUEST_WANTS_TYPE (request, REQUEST_THUMBNAIL))
    {
        if (has_problem (directory, file, lacks_thumbnail))
        {
            return FALSE;
        }
    }

    if (REQUEST_WANTS_TYPE (request, REQUEST_MOUNT))
    {
        if (has_problem (directory, file, lacks_mount))
        {
            return FALSE;
        }
    }

    if (REQUEST_WANTS_TYPE (request, REQUEST_MIME_LIST))
    {
        if (has_problem (directory, file, lacks_mime_list))
        {
            return FALSE;
        }
    }

    if (REQUEST_WANTS_TYPE (request, REQUEST_LINK_INFO))
    {
        if (has_problem (directory, file, lacks_link_info))
        {
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean
call_ready_callbacks_at_idle (gpointer callback_data)
{
    CajaDirectory *directory;
    GList *node, *next;
    ReadyCallback *callback;

    directory = CAJA_DIRECTORY (callback_data);
    directory->details->call_ready_idle_id = 0;

    caja_directory_ref (directory);

    callback = NULL;
    while (1)
    {
        /* Check if any callbacks are non-active and call them if they are. */
        for (node = directory->details->call_when_ready_list;
                node != NULL; node = next)
        {
            next = node->next;
            callback = node->data;
            if (!callback->active)
            {
                /* Non-active, remove and call */
                break;
            }
        }
        if (node == NULL)
        {
            break;
        }

        /* Callbacks are one-shots, so remove it now. */
        remove_callback_link_keep_data (directory, node);

        /* Call the callback. */
        ready_callback_call (directory, callback);
        g_free (callback);
    }

    caja_directory_async_state_changed (directory);

    caja_directory_unref (directory);

    return FALSE;
}

static void
schedule_call_ready_callbacks (CajaDirectory *directory)
{
    if (directory->details->call_ready_idle_id == 0)
    {
        directory->details->call_ready_idle_id
            = g_idle_add (call_ready_callbacks_at_idle, directory);
    }
}

/* Marks all callbacks that are ready as non-active and
 * calls them at idle time, unless they are removed
 * before then */
static gboolean
call_ready_callbacks (CajaDirectory *directory)
{
    gboolean found_any;
    GList *node, *next;
    ReadyCallback *callback = NULL;

    found_any = FALSE;

    /* Check if any callbacks are satisifed and mark them for call them if they are. */
    for (node = directory->details->call_when_ready_list;
            node != NULL; node = next)
    {
        next = node->next;
        callback = node->data;
        if (callback->active &&
                request_is_satisfied (directory, callback->file, callback->request))
        {
            callback->active = FALSE;
            found_any = TRUE;
        }
    }

    if (found_any)
    {
        schedule_call_ready_callbacks (directory);
    }

    return found_any;
}

gboolean
caja_directory_has_active_request_for_file (CajaDirectory *directory,
        CajaFile *file)
{
    GList *node;
    ReadyCallback *callback = NULL;
    Monitor *monitor = NULL;

    for (node = directory->details->call_when_ready_list;
            node != NULL; node = node->next)
    {
        callback = node->data;
        if (callback->file == file ||
                callback->file == NULL)
        {
            return TRUE;
        }
    }

    for (node = directory->details->monitor_list;
            node != NULL; node = node->next)
    {
        monitor = node->data;
        if (monitor->file == file ||
                monitor->file == NULL)
        {
            return TRUE;
        }
    }

    return FALSE;
}

/* This checks if there's a request for monitoring the file list. */
gboolean
caja_directory_is_anyone_monitoring_file_list (CajaDirectory *directory)
{
    if (directory->details->call_when_ready_counters[REQUEST_FILE_LIST] > 0)
    {
        return TRUE;
    }

    if (directory->details->monitor_counters[REQUEST_FILE_LIST] > 0)
    {
        return TRUE;
    }

    return FALSE;
}

/* This checks if the file list being monitored. */
gboolean
caja_directory_is_file_list_monitored (CajaDirectory *directory)
{
    return directory->details->file_list_monitored;
}

static void
mark_all_files_unconfirmed (CajaDirectory *directory)
{
    GList *node;
    CajaFile *file = NULL;

    for (node = directory->details->file_list; node != NULL; node = node->next)
    {
        file = node->data;
        set_file_unconfirmed (file, TRUE);
    }
}

static void
directory_load_state_free (DirectoryLoadState *state)
{
    if (state->enumerator)
    {
        if (!g_file_enumerator_is_closed (state->enumerator))
        {
            g_file_enumerator_close_async (state->enumerator,
                                           0, NULL, NULL, NULL);
        }
        g_object_unref (state->enumerator);
    }

    if (state->load_mime_list_hash != NULL)
    {
        istr_set_destroy (state->load_mime_list_hash);
    }
    caja_file_unref (state->load_directory_file);
    g_object_unref (state->cancellable);
    g_free (state);
}

static void
more_files_callback (GObject *source_object,
                     GAsyncResult *res,
                     gpointer user_data)
{
    DirectoryLoadState *state;
    CajaDirectory *directory;
    GError *error;
    GList *files, *l;
    GFileInfo *info = NULL;

    state = user_data;

    if (state->directory == NULL)
    {
        /* Operation was cancelled. Bail out */
        directory_load_state_free (state);
        return;
    }

    directory = caja_directory_ref (state->directory);

    g_assert (directory->details->directory_load_in_progress != NULL);
    g_assert (directory->details->directory_load_in_progress == state);

    error = NULL;
    files = g_file_enumerator_next_files_finish (state->enumerator,
            res, &error);

    for (l = files; l != NULL; l = l->next)
    {
        info = l->data;
        directory_load_one (directory, info);
        g_object_unref (info);
    }

    if (files == NULL)
    {
        directory_load_done (directory, error);
        directory_load_state_free (state);
    }
    else
    {
        g_file_enumerator_next_files_async (state->enumerator,
                                            DIRECTORY_LOAD_ITEMS_PER_CALLBACK,
                                            G_PRIORITY_DEFAULT,
                                            state->cancellable,
                                            more_files_callback,
                                            state);
    }

    caja_directory_unref (directory);

    if (error)
    {
        g_error_free (error);
    }

    g_list_free (files);
}

static void
enumerate_children_callback (GObject *source_object,
                             GAsyncResult *res,
                             gpointer user_data)
{
    DirectoryLoadState *state;
    GFileEnumerator *enumerator;
    GError *error;

    state = user_data;

    if (state->directory == NULL)
    {
        /* Operation was cancelled. Bail out */
        directory_load_state_free (state);
        return;
    }

    error = NULL;
    enumerator = g_file_enumerate_children_finish  (G_FILE (source_object),
                 res, &error);

    if (enumerator == NULL)
    {
        directory_load_done (state->directory, error);
        g_error_free (error);
        directory_load_state_free (state);
        return;
    }
    else
    {
        state->enumerator = enumerator;
        g_file_enumerator_next_files_async (state->enumerator,
                                            DIRECTORY_LOAD_ITEMS_PER_CALLBACK,
                                            G_PRIORITY_DEFAULT,
                                            state->cancellable,
                                            more_files_callback,
                                            state);
    }
}

/* Start monitoring the file list if it isn't already. */
static void
start_monitoring_file_list (CajaDirectory *directory)
{
    DirectoryLoadState *state;

    if (!directory->details->file_list_monitored)
    {
        g_assert (!directory->details->directory_load_in_progress);
        directory->details->file_list_monitored = TRUE;
        caja_file_list_ref (directory->details->file_list);
    }

    if (directory->details->directory_loaded  ||
            directory->details->directory_load_in_progress != NULL)
    {
        return;
    }

    if (!async_job_start (directory, "file list"))
    {
        return;
    }

    mark_all_files_unconfirmed (directory);

    state = g_new0 (DirectoryLoadState, 1);
    state->directory = directory;
    state->cancellable = g_cancellable_new ();
    state->load_mime_list_hash = istr_set_new ();
    state->load_file_count = 0;

    g_assert (directory->details->location != NULL);
    state->load_directory_file =
        caja_directory_get_corresponding_file (directory);
    state->load_directory_file->details->loading_directory = TRUE;

#ifdef DEBUG_LOAD_DIRECTORY
    g_message ("load_directory called to monitor file list of %p", directory->details->location);
#endif

    directory->details->directory_load_in_progress = state;

    g_file_enumerate_children_async (directory->details->location,
                                     CAJA_FILE_DEFAULT_ATTRIBUTES,
                                     0, /* flags */
                                     G_PRIORITY_DEFAULT, /* prio */
                                     state->cancellable,
                                     enumerate_children_callback,
                                     state);
}

/* Stop monitoring the file list if it is being monitored. */
void
caja_directory_stop_monitoring_file_list (CajaDirectory *directory)
{
    if (!directory->details->file_list_monitored)
    {
        g_assert (directory->details->directory_load_in_progress == NULL);
        return;
    }

    directory->details->file_list_monitored = FALSE;
    file_list_cancel (directory);
    caja_file_list_unref (directory->details->file_list);
    directory->details->directory_loaded = FALSE;
}

static void
file_list_start_or_stop (CajaDirectory *directory)
{
    if (caja_directory_is_anyone_monitoring_file_list (directory))
    {
        start_monitoring_file_list (directory);
    }
    else
    {
        caja_directory_stop_monitoring_file_list (directory);
    }
}

void
caja_file_invalidate_count_and_mime_list (CajaFile *file)
{
    CajaFileAttributes attributes;

    attributes = CAJA_FILE_ATTRIBUTE_DIRECTORY_ITEM_COUNT |
                 CAJA_FILE_ATTRIBUTE_DIRECTORY_ITEM_MIME_TYPES;

    caja_file_invalidate_attributes (file, attributes);
}

/* Reset count and mime list. Invalidating deep counts is handled by
 * itself elsewhere because it's a relatively heavyweight and
 * special-purpose operation (see bug 5863). Also, the shallow count
 * needs to be refreshed when filtering changes, but the deep count
 * deliberately does not take filtering into account.
 */
void
caja_directory_invalidate_count_and_mime_list (CajaDirectory *directory)
{
    CajaFile *file;

    file = caja_directory_get_existing_corresponding_file (directory);
    if (file != NULL)
    {
        caja_file_invalidate_count_and_mime_list (file);
    }

    caja_file_unref (file);
}

static void
caja_directory_invalidate_file_attributes (CajaDirectory      *directory,
        CajaFileAttributes  file_attributes)
{
    GList *node;

    cancel_loading_attributes (directory, file_attributes);

    for (node = directory->details->file_list; node != NULL; node = node->next)
    {
        caja_file_invalidate_attributes_internal (CAJA_FILE (node->data),
                file_attributes);
    }

    if (directory->details->as_file != NULL)
    {
        caja_file_invalidate_attributes_internal (directory->details->as_file,
                file_attributes);
    }
}

void
caja_directory_force_reload_internal (CajaDirectory     *directory,
                                      CajaFileAttributes file_attributes)
{
    /* invalidate attributes that are getting reloaded for all files */
    caja_directory_invalidate_file_attributes (directory, file_attributes);

    /* Start a new directory load. */
    file_list_cancel (directory);
    directory->details->directory_loaded = FALSE;

    /* Start a new directory count. */
    caja_directory_invalidate_count_and_mime_list (directory);

    add_all_files_to_work_queue (directory);
    caja_directory_async_state_changed (directory);
}

static gboolean
monitor_includes_file (const Monitor *monitor,
                       CajaFile *file)
{
    if (monitor->file == file)
    {
        return TRUE;
    }
    if (monitor->file != NULL)
    {
        return FALSE;
    }
    if (file == file->details->directory->details->as_file)
    {
        return FALSE;
    }
    return caja_file_should_show (file,
                                  monitor->monitor_hidden_files,
                                  TRUE,
				  FALSE);
}

static gboolean
is_needy (CajaFile *file,
          FileCheck check_missing,
          RequestType request_type_wanted)
{
    CajaDirectory *directory;
    GList *node;

    if (!(* check_missing) (file))
    {
        return FALSE;
    }

    directory = file->details->directory;
    if (directory->details->call_when_ready_counters[request_type_wanted] > 0)
    {
        ReadyCallback *callback = NULL;

        for (node = directory->details->call_when_ready_list;
                node != NULL; node = node->next)
        {
            callback = node->data;
            if (callback->active &&
                    REQUEST_WANTS_TYPE (callback->request, request_type_wanted))
            {
                if (callback->file == file)
                {
                    return TRUE;
                }
                if (callback->file == NULL
                        && file != directory->details->as_file)
                {
                    return TRUE;
                }
            }
        }
    }

    if (directory->details->monitor_counters[request_type_wanted] > 0)
    {
        Monitor *monitor = NULL;

        for (node = directory->details->monitor_list;
                node != NULL; node = node->next)
        {
            monitor = node->data;
            if (REQUEST_WANTS_TYPE (monitor->request, request_type_wanted))
            {
                if (monitor_includes_file (monitor, file))
                {
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

static void
directory_count_stop (CajaDirectory *directory)
{
    if (directory->details->count_in_progress != NULL)
    {
        CajaFile *file;

        file = directory->details->count_in_progress->count_file;
        if (file != NULL)
        {
            g_assert (CAJA_IS_FILE (file));
            g_assert (file->details->directory == directory);
            if (is_needy (file,
                          should_get_directory_count_now,
                          REQUEST_DIRECTORY_COUNT))
            {
                return;
            }
        }

        /* The count is not wanted, so stop it. */
        directory_count_cancel (directory);
    }
}

static guint
count_non_skipped_files (GList *list)
{
    guint count;
    GList *node;
    GFileInfo *info = NULL;

    count = 0;
    for (node = list; node != NULL; node = node->next)
    {
        info = node->data;
        if (!should_skip_file (NULL, info))
        {
            count += 1;
        }
    }
    return count;
}

static void
count_children_done (CajaDirectory *directory,
                     CajaFile *count_file,
                     gboolean succeeded,
                     int count)
{
    g_assert (CAJA_IS_FILE (count_file));

    count_file->details->directory_count_is_up_to_date = TRUE;

    /* Record either a failure or success. */
    if (!succeeded)
    {
        count_file->details->directory_count_failed = TRUE;
        count_file->details->got_directory_count = FALSE;
        count_file->details->directory_count = 0;
    }
    else
    {
        count_file->details->directory_count_failed = FALSE;
        count_file->details->got_directory_count = TRUE;
        count_file->details->directory_count = count;
    }
    directory->details->count_in_progress = NULL;

    /* Send file-changed even if count failed, so interested parties can
     * distinguish between unknowable and not-yet-known cases.
     */
    caja_file_changed (count_file);

    /* Start up the next one. */
    async_job_end (directory, "directory count");
    caja_directory_async_state_changed (directory);
}

static void
directory_count_state_free (DirectoryCountState *state)
{
    if (state->enumerator)
    {
        if (!g_file_enumerator_is_closed (state->enumerator))
        {
            g_file_enumerator_close_async (state->enumerator,
                                           0, NULL, NULL, NULL);
        }
        g_object_unref (state->enumerator);
    }
    g_object_unref (state->cancellable);
    caja_directory_unref (state->directory);
    g_free (state);
}

static void
count_more_files_callback (GObject *source_object,
                           GAsyncResult *res,
                           gpointer user_data)
{
    DirectoryCountState *state;
    CajaDirectory *directory;
    GError *error;
    GList *files;

    state = user_data;
    directory = state->directory;

    if (g_cancellable_is_cancelled (state->cancellable))
    {
        /* Operation was cancelled. Bail out */
        directory->details->count_in_progress = NULL;

        async_job_end (directory, "directory count");
        caja_directory_async_state_changed (directory);

        directory_count_state_free (state);

        return;
    }

    g_assert (directory->details->count_in_progress != NULL);
    g_assert (directory->details->count_in_progress == state);

    error = NULL;
    files = g_file_enumerator_next_files_finish (state->enumerator,
            res, &error);

    state->file_count += count_non_skipped_files (files);

    if (files == NULL)
    {
        count_children_done (directory, state->count_file,
                             TRUE, state->file_count);
        directory_count_state_free (state);
    }
    else
    {
        g_file_enumerator_next_files_async (state->enumerator,
                                            DIRECTORY_LOAD_ITEMS_PER_CALLBACK,
                                            G_PRIORITY_DEFAULT,
                                            state->cancellable,
                                            count_more_files_callback,
                                            state);
    }

    g_list_free_full (files, g_object_unref);

    if (error)
    {
        g_error_free (error);
    }
}

static void
count_children_callback (GObject *source_object,
                         GAsyncResult *res,
                         gpointer user_data)
{
    DirectoryCountState *state;
    GFileEnumerator *enumerator;
    GError *error;

    state = user_data;

    if (g_cancellable_is_cancelled (state->cancellable))
    {
        CajaDirectory *directory;

        /* Operation was cancelled. Bail out */
        directory = state->directory;
        directory->details->count_in_progress = NULL;

        async_job_end (directory, "directory count");
        caja_directory_async_state_changed (directory);

        directory_count_state_free (state);

        return;
    }

    error = NULL;
    enumerator = g_file_enumerate_children_finish  (G_FILE (source_object),
                 res, &error);

    if (enumerator == NULL)
    {
        count_children_done (state->directory,
                             state->count_file,
                             FALSE, 0);
        g_error_free (error);
        directory_count_state_free (state);
        return;
    }
    else
    {
        state->enumerator = enumerator;
        g_file_enumerator_next_files_async (state->enumerator,
                                            DIRECTORY_LOAD_ITEMS_PER_CALLBACK,
                                            G_PRIORITY_DEFAULT,
                                            state->cancellable,
                                            count_more_files_callback,
                                            state);
    }
}

static void
directory_count_start (CajaDirectory *directory,
                       CajaFile *file,
                       gboolean *doing_io)
{
    DirectoryCountState *state;
    GFile *location;

    if (directory->details->count_in_progress != NULL)
    {
        *doing_io = TRUE;
        return;
    }

    if (!is_needy (file,
                   should_get_directory_count_now,
                   REQUEST_DIRECTORY_COUNT))
    {
        return;
    }
    *doing_io = TRUE;

    if (!caja_file_is_directory (file))
    {
        file->details->directory_count_is_up_to_date = TRUE;
        file->details->directory_count_failed = FALSE;
        file->details->got_directory_count = FALSE;

        caja_directory_async_state_changed (directory);
        return;
    }

    if (!async_job_start (directory, "directory count"))
    {
        return;
    }

    /* Start counting. */
    state = g_new0 (DirectoryCountState, 1);
    state->count_file = file;
    state->directory = caja_directory_ref (directory);
    state->cancellable = g_cancellable_new ();

    directory->details->count_in_progress = state;

    location = caja_file_get_location (file);
#ifdef DEBUG_LOAD_DIRECTORY
    {
        char *uri;
        uri = g_file_get_uri (location);
        g_message ("load_directory called to get shallow file count for %s", uri);
        g_free (uri);
    }
#endif

    g_file_enumerate_children_async (location,
                                     G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                     G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","
                                     G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP,
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, /* flags */
                                     G_PRIORITY_DEFAULT, /* prio */
                                     state->cancellable,
                                     count_children_callback,
                                     state);
    g_object_unref (location);
}

static inline gboolean
seen_inode (DeepCountState *state,
            GFileInfo *info)
{
    guint64 inode, inode2;
    guint i;

    inode = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_INODE);

    if (inode != 0)
    {
        for (i = 0; i < state->seen_deep_count_inodes->len; i++)
        {
            inode2 = g_array_index (state->seen_deep_count_inodes, guint64, i);
            if (inode == inode2)
            {
                return TRUE;
            }
        }
    }

    return FALSE;
}

static inline void
mark_inode_as_seen (DeepCountState *state,
                    GFileInfo *info)
{
    guint64 inode;

    inode = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_INODE);
    if (inode != 0)
    {
        g_array_append_val (state->seen_deep_count_inodes, inode);
    }
}

static void
deep_count_one (DeepCountState *state,
                GFileInfo *info)
{
    CajaFile *file;
    gboolean is_seen_inode;

    if (should_skip_file (NULL, info))
    {
        return;
    }

    is_seen_inode = seen_inode (state, info);
    if (!is_seen_inode)
    {
        mark_inode_as_seen (state, info);
    }

    file = state->directory->details->deep_count_file;

    if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
        const char *fs_id;

        /* Count the directory. */
        file->details->deep_directory_count += 1;

        /* Record the fact that we have to descend into this directory. */

        fs_id = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
        if (g_strcmp0 (fs_id, state->fs_id) == 0)
        {
            GFile *subdir;

            /* only if it is on the same filesystem */
            subdir = g_file_get_child (state->deep_count_location, g_file_info_get_name (info));
            state->deep_count_subdirectories = g_list_prepend (state->deep_count_subdirectories, subdir);
        }
    }
    else
    {
        /* Even non-regular files count as files. */
        file->details->deep_file_count += 1;
    }

    /* Count the size. */
    if (!is_seen_inode && g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
    {
        file->details->deep_size += g_file_info_get_size (info);
    }
    /* Count the disk size. */
    if (!is_seen_inode && g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE))
    {
        file->details->deep_size_on_disk +=
            g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE);
    }
}

static void
deep_count_state_free (DeepCountState *state)
{
    if (state->enumerator)
    {
        if (!g_file_enumerator_is_closed (state->enumerator))
        {
            g_file_enumerator_close_async (state->enumerator,
                                           0, NULL, NULL, NULL);
        }
        g_object_unref (state->enumerator);
    }
    g_object_unref (state->cancellable);
    if (state->deep_count_location)
    {
        g_object_unref (state->deep_count_location);
    }
    g_list_free_full (state->deep_count_subdirectories, g_object_unref);
    g_array_free (state->seen_deep_count_inodes, TRUE);
    g_free (state->fs_id);
    g_free (state);
}

static void
deep_count_next_dir (DeepCountState *state)
{
    CajaFile *file;
    CajaDirectory *directory;
    gboolean done;

    directory = state->directory;

    g_object_unref (state->deep_count_location);
    state->deep_count_location = NULL;

    done = FALSE;
    file = directory->details->deep_count_file;

    if (state->deep_count_subdirectories != NULL)
    {
        GFile *location;

        /* Work on a new directory. */
        location = state->deep_count_subdirectories->data;
        state->deep_count_subdirectories = g_list_remove
                                           (state->deep_count_subdirectories, location);
        deep_count_load (state, location);
        g_object_unref (location);
    }
    else
    {
        file->details->deep_counts_status = CAJA_REQUEST_DONE;
        directory->details->deep_count_file = NULL;
        directory->details->deep_count_in_progress = NULL;
        deep_count_state_free (state);
        done = TRUE;
    }

    caja_file_updated_deep_count_in_progress (file);

    if (done)
    {
        caja_file_changed (file);
        async_job_end (directory, "deep count");
        caja_directory_async_state_changed (directory);
    }
}

static void
deep_count_more_files_callback (GObject *source_object,
                                GAsyncResult *res,
                                gpointer user_data)
{
    DeepCountState *state;
    CajaDirectory *directory;
    GList *files, *l;
    GFileInfo *info = NULL;

    state = user_data;

    if (state->directory == NULL)
    {
        /* Operation was cancelled. Bail out */
        deep_count_state_free (state);
        return;
    }

    directory = caja_directory_ref (state->directory);

    g_assert (directory->details->deep_count_in_progress != NULL);
    g_assert (directory->details->deep_count_in_progress == state);

    files = g_file_enumerator_next_files_finish (state->enumerator,
            res, NULL);

    for (l = files; l != NULL; l = l->next)
    {
        info = l->data;
        deep_count_one (state, info);
        g_object_unref (info);
    }

    if (files == NULL)
    {
        g_file_enumerator_close_async (state->enumerator, 0, NULL, NULL, NULL);
        g_object_unref (state->enumerator);
        state->enumerator = NULL;

        deep_count_next_dir (state);
    }
    else
    {
        g_file_enumerator_next_files_async (state->enumerator,
                                            DIRECTORY_LOAD_ITEMS_PER_CALLBACK,
                                            G_PRIORITY_LOW,
                                            state->cancellable,
                                            deep_count_more_files_callback,
                                            state);
    }

    g_list_free (files);

    caja_directory_unref (directory);
}

static void
deep_count_callback (GObject *source_object,
                     GAsyncResult *res,
                     gpointer user_data)
{
    DeepCountState *state;
    GFileEnumerator *enumerator;
    CajaFile *file;

    state = user_data;

    if (state->directory == NULL)
    {
        /* Operation was cancelled. Bail out */
        deep_count_state_free (state);
        return;
    }

    file = state->directory->details->deep_count_file;

    enumerator = g_file_enumerate_children_finish  (G_FILE (source_object),	res, NULL);

    if (enumerator == NULL)
    {
        file->details->deep_unreadable_count += 1;

        deep_count_next_dir (state);
    }
    else
    {
        state->enumerator = enumerator;
        g_file_enumerator_next_files_async (state->enumerator,
                                            DIRECTORY_LOAD_ITEMS_PER_CALLBACK,
                                            G_PRIORITY_LOW,
                                            state->cancellable,
                                            deep_count_more_files_callback,
                                            state);
    }
}

static void
deep_count_load (DeepCountState *state, GFile *location)
{
    state->deep_count_location = g_object_ref (location);

#ifdef DEBUG_LOAD_DIRECTORY
    g_message ("load_directory called to get deep file count for %p", location);
#endif
    g_file_enumerate_children_async (state->deep_count_location,
                                     G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                     G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                     G_FILE_ATTRIBUTE_STANDARD_SIZE ","
                                     G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE ","
                                     G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","
                                     G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP ","
                                     G_FILE_ATTRIBUTE_ID_FILESYSTEM ","
                                     G_FILE_ATTRIBUTE_UNIX_INODE,
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, /* flags */
                                     G_PRIORITY_LOW, /* prio */
                                     state->cancellable,
                                     deep_count_callback,
                                     state);
}

static void
deep_count_stop (CajaDirectory *directory)
{
    if (directory->details->deep_count_in_progress != NULL)
    {
        CajaFile *file;

        file = directory->details->deep_count_file;
        if (file != NULL)
        {
            g_assert (CAJA_IS_FILE (file));
            g_assert (file->details->directory == directory);
            if (is_needy (file,
                          lacks_deep_count,
                          REQUEST_DEEP_COUNT))
            {
                return;
            }
        }

        /* The count is not wanted, so stop it. */
        deep_count_cancel (directory);
    }
}

static void
deep_count_got_info (GObject *source_object,
                     GAsyncResult *res,
                     gpointer user_data)
{
     GFileInfo *info;
     GFile *file = (GFile *)source_object;
     DeepCountState *state = (DeepCountState *)user_data;

     info = g_file_query_info_finish (file, res, NULL);
     if (info != NULL)
     {
         const char *id;

         id = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
         state->fs_id = g_strdup (id);
         g_object_unref (info);
     }
     deep_count_load (state, file);
}

static void
deep_count_start (CajaDirectory *directory,
                  CajaFile *file,
                  gboolean *doing_io)
{
    GFile *location;
    DeepCountState *state;

    if (directory->details->deep_count_in_progress != NULL)
    {
        *doing_io = TRUE;
        return;
    }

    if (!is_needy (file,
                   lacks_deep_count,
                   REQUEST_DEEP_COUNT))
    {
        return;
    }
    *doing_io = TRUE;

    if (!caja_file_is_directory (file))
    {
        file->details->deep_counts_status = CAJA_REQUEST_DONE;

        caja_directory_async_state_changed (directory);
        return;
    }

    if (!async_job_start (directory, "deep count"))
    {
        return;
    }

    /* Start counting. */
    file->details->deep_counts_status = CAJA_REQUEST_IN_PROGRESS;
    file->details->deep_directory_count = 0;
    file->details->deep_file_count = 0;
    file->details->deep_unreadable_count = 0;
    file->details->deep_size = 0;
    file->details->deep_size_on_disk = 0;
    directory->details->deep_count_file = file;

    state = g_new0 (DeepCountState, 1);
    state->directory = directory;
    state->cancellable = g_cancellable_new ();
    state->seen_deep_count_inodes = g_array_new (FALSE, TRUE, sizeof (guint64));
    state->fs_id = NULL;

    directory->details->deep_count_in_progress = state;

    location = caja_file_get_location (file);
    g_file_query_info_async (location,
                             G_FILE_ATTRIBUTE_ID_FILESYSTEM,
                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                             G_PRIORITY_DEFAULT,
                             NULL,
                             deep_count_got_info,
                             state);
    g_object_unref (location);
}

static void
mime_list_stop (CajaDirectory *directory)
{
    if (directory->details->mime_list_in_progress != NULL)
    {
        CajaFile *file;

        file = directory->details->mime_list_in_progress->mime_list_file;
        if (file != NULL)
        {
            g_assert (CAJA_IS_FILE (file));
            g_assert (file->details->directory == directory);
            if (is_needy (file,
                          should_get_mime_list,
                          REQUEST_MIME_LIST))
            {
                return;
            }
        }

        /* The count is not wanted, so stop it. */
        mime_list_cancel (directory);
    }
}

static void
mime_list_state_free (MimeListState *state)
{
    if (state->enumerator)
    {
        if (!g_file_enumerator_is_closed (state->enumerator))
        {
            g_file_enumerator_close_async (state->enumerator,
                                           0, NULL, NULL, NULL);
        }
        g_object_unref (state->enumerator);
    }
    g_object_unref (state->cancellable);
    istr_set_destroy (state->mime_list_hash);
    caja_directory_unref (state->directory);
    g_free (state);
}

static void
mime_list_done (MimeListState *state, gboolean success)
{
    CajaFile *file;
    CajaDirectory *directory;

    directory = state->directory;
    g_assert (directory != NULL);

    file = state->mime_list_file;

    file->details->mime_list_is_up_to_date = TRUE;
    g_list_free_full (file->details->mime_list, g_free);
    if (success)
    {
        file->details->mime_list_failed = TRUE;
        file->details->mime_list = NULL;
    }
    else
    {
        file->details->got_mime_list = TRUE;
        file->details->mime_list = istr_set_get_as_list	(state->mime_list_hash);
    }
    directory->details->mime_list_in_progress = NULL;

    /* Send file-changed even if getting the item type list
     * failed, so interested parties can distinguish between
     * unknowable and not-yet-known cases.
     */
    caja_file_changed (file);

    /* Start up the next one. */
    async_job_end (directory, "MIME list");
    caja_directory_async_state_changed (directory);
}

static void
mime_list_one (MimeListState *state,
               GFileInfo     *info)
{
    const char *mime_type;

    if (should_skip_file (NULL, info))
    {
        g_object_unref (info);
        return;
    }

    if ((mime_type = g_file_info_get_content_type (info)) != NULL)
    {
        g_hash_table_add (state->mime_list_hash, g_strdup (mime_type));
    }
}

static void
mime_list_callback (GObject *source_object,
                    GAsyncResult *res,
                    gpointer user_data)
{
    MimeListState *state;
    CajaDirectory *directory;
    GError *error;
    GList *files, *l;
    GFileInfo *info = NULL;

    state = user_data;
    directory = state->directory;

    if (g_cancellable_is_cancelled (state->cancellable))
    {
        /* Operation was cancelled. Bail out */
        directory->details->mime_list_in_progress = NULL;

        async_job_end (directory, "MIME list");
        caja_directory_async_state_changed (directory);

        mime_list_state_free (state);

        return;
    }

    g_assert (directory->details->mime_list_in_progress != NULL);
    g_assert (directory->details->mime_list_in_progress == state);

    error = NULL;
    files = g_file_enumerator_next_files_finish (state->enumerator,
            res, &error);

    for (l = files; l != NULL; l = l->next)
    {
        info = l->data;
        mime_list_one (state, info);
        g_object_unref (info);
    }

    if (files == NULL)
    {
        mime_list_done (state, error != NULL);
        mime_list_state_free (state);
    }
    else
    {
        g_file_enumerator_next_files_async (state->enumerator,
                                            DIRECTORY_LOAD_ITEMS_PER_CALLBACK,
                                            G_PRIORITY_DEFAULT,
                                            state->cancellable,
                                            mime_list_callback,
                                            state);
    }

    g_list_free (files);

    if (error)
    {
        g_error_free (error);
    }
}

static void
list_mime_enum_callback (GObject *source_object,
                         GAsyncResult *res,
                         gpointer user_data)
{
    MimeListState *state;
    GFileEnumerator *enumerator;
    GError *error;

    state = user_data;

    if (g_cancellable_is_cancelled (state->cancellable))
    {
        CajaDirectory *directory;

        /* Operation was cancelled. Bail out */
        directory = state->directory;
        directory->details->mime_list_in_progress = NULL;

        async_job_end (directory, "MIME list");
        caja_directory_async_state_changed (directory);

        mime_list_state_free (state);

        return;
    }

    error = NULL;
    enumerator = g_file_enumerate_children_finish  (G_FILE (source_object),
                 res, &error);

    if (enumerator == NULL)
    {
        mime_list_done (state, FALSE);
        g_error_free (error);
        mime_list_state_free (state);
        return;
    }
    else
    {
        state->enumerator = enumerator;
        g_file_enumerator_next_files_async (state->enumerator,
                                            DIRECTORY_LOAD_ITEMS_PER_CALLBACK,
                                            G_PRIORITY_DEFAULT,
                                            state->cancellable,
                                            mime_list_callback,
                                            state);
    }
}

static void
mime_list_start (CajaDirectory *directory,
                 CajaFile *file,
                 gboolean *doing_io)
{
    MimeListState *state;
    GFile *location;

    mime_list_stop (directory);

    if (directory->details->mime_list_in_progress != NULL)
    {
        *doing_io = TRUE;
        return;
    }

    /* Figure out which file to get a mime list for. */
    if (!is_needy (file,
                   should_get_mime_list,
                   REQUEST_MIME_LIST))
    {
        return;
    }
    *doing_io = TRUE;

    if (!caja_file_is_directory (file))
    {
        g_list_free (file->details->mime_list);
        file->details->mime_list_failed = FALSE;
        file->details->got_mime_list = FALSE;
        file->details->mime_list_is_up_to_date = TRUE;

        caja_directory_async_state_changed (directory);
        return;
    }

    if (!async_job_start (directory, "MIME list"))
    {
        return;
    }

    state = g_new0 (MimeListState, 1);
    state->mime_list_file = file;
    state->directory = caja_directory_ref (directory);
    state->cancellable = g_cancellable_new ();
    state->mime_list_hash = istr_set_new ();

    directory->details->mime_list_in_progress = state;

    location = caja_file_get_location (file);
#ifdef DEBUG_LOAD_DIRECTORY
    {
        char *uri;
        uri = g_file_get_uri (location);
        g_message ("load_directory called to get MIME list of %s", uri);
        g_free (uri);
    }
#endif

    g_file_enumerate_children_async (location,
                                     G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                     0, /* flags */
                                     G_PRIORITY_LOW, /* prio */
                                     state->cancellable,
                                     list_mime_enum_callback,
                                     state);
    g_object_unref (location);
}

static void
top_left_stop (CajaDirectory *directory)
{
    if (directory->details->top_left_read_state != NULL)
    {
        CajaFile *file;

        file = directory->details->top_left_read_state->file;
        if (file != NULL)
        {
            g_assert (CAJA_IS_FILE (file));
            g_assert (file->details->directory == directory);
            if (is_needy (file,
                          lacks_top_left,
                          REQUEST_TOP_LEFT_TEXT) ||
                    is_needy (file,
                              lacks_large_top_left,
                              REQUEST_LARGE_TOP_LEFT_TEXT))
            {
                return;
            }
        }

        /* The top left is not wanted, so stop it. */
        top_left_cancel (directory);
    }
}

static void
top_left_read_state_free (TopLeftTextReadState *state)
{
    g_object_unref (state->cancellable);
    g_free (state);
}

static void
top_left_read_callback (GObject *source_object,
                        GAsyncResult *res,
                        gpointer callback_data)
{
    TopLeftTextReadState *state;
    CajaDirectory *directory;
    CajaFilePrivate *file_details;
    gsize file_size;
    char *file_contents;

    state = callback_data;

    if (state->directory == NULL)
    {
        /* Operation was cancelled. Bail out */
        top_left_read_state_free (state);
        return;
    }

    directory = caja_directory_ref (state->directory);

    file_details = state->file->details;

    file_details->top_left_text_is_up_to_date = TRUE;
    g_free (file_details->top_left_text);

    if (g_file_load_partial_contents_finish (G_FILE (source_object),
            res,
            &file_contents, &file_size,
            NULL, NULL))
    {
        file_details->top_left_text = caja_extract_top_left_text (file_contents, state->large, file_size);
        file_details->got_top_left_text = TRUE;
        file_details->got_large_top_left_text = state->large;
        g_free (file_contents);
    }
    else
    {
        file_details->top_left_text = NULL;
        file_details->got_top_left_text = FALSE;
        file_details->got_large_top_left_text = FALSE;
    }

    caja_file_changed (state->file);

    directory->details->top_left_read_state = NULL;
    async_job_end (directory, "top left");

    top_left_read_state_free (state);

    caja_directory_async_state_changed (directory);

    caja_directory_unref (directory);
}

static int
count_lines (const char *text, int length)
{
    int count, i;

    count = 0;
    for (i = 0; i < length; i++)
    {
        count += *text++ == '\n';
    }
    return count;
}

static gboolean
top_left_read_more_callback (const char *file_contents,
                             goffset bytes_read,
                             gpointer callback_data)
{
    TopLeftTextReadState *state;

    state = callback_data;

    /* Stop reading when we have enough. */
    if (state->large)
    {
        return bytes_read < CAJA_FILE_LARGE_TOP_LEFT_TEXT_MAXIMUM_BYTES &&
               count_lines (file_contents, bytes_read) <= CAJA_FILE_LARGE_TOP_LEFT_TEXT_MAXIMUM_LINES;
    }
    else
    {
        return bytes_read < CAJA_FILE_TOP_LEFT_TEXT_MAXIMUM_BYTES &&
               count_lines (file_contents, bytes_read) <= CAJA_FILE_TOP_LEFT_TEXT_MAXIMUM_LINES;
    }
}

static void
top_left_start (CajaDirectory *directory,
                CajaFile *file,
                gboolean *doing_io)
{
    GFile *location;
    gboolean needs_large;
    TopLeftTextReadState *state;

    if (directory->details->top_left_read_state != NULL)
    {
        *doing_io = TRUE;
        return;
    }

    needs_large = FALSE;

    if (is_needy (file,
                  lacks_large_top_left,
                  REQUEST_LARGE_TOP_LEFT_TEXT))
    {
        needs_large = TRUE;
    }

    /* Figure out which file to read the top left for. */
    if (!(needs_large ||
            is_needy (file,
                      lacks_top_left,
                      REQUEST_TOP_LEFT_TEXT)))
    {
        return;
    }
    *doing_io = TRUE;

    if (!caja_file_contains_text (file))
    {
        g_free (file->details->top_left_text);
        file->details->top_left_text = NULL;
        file->details->got_top_left_text = FALSE;
        file->details->got_large_top_left_text = FALSE;
        file->details->top_left_text_is_up_to_date = TRUE;

        caja_directory_async_state_changed (directory);
        return;
    }

    if (!async_job_start (directory, "top left"))
    {
        return;
    }

    /* Start reading. */
    state = g_new0 (TopLeftTextReadState, 1);
    state->directory = directory;
    state->cancellable = g_cancellable_new ();
    state->large = needs_large;
    state->file = file;

    directory->details->top_left_read_state = state;

    location = caja_file_get_location (file);
    g_file_load_partial_contents_async (location,
                                        state->cancellable,
                                        top_left_read_more_callback,
                                        top_left_read_callback,
                                        state);
    g_object_unref (location);
}

static void
get_info_state_free (GetInfoState *state)
{
    g_object_unref (state->cancellable);
    g_free (state);
}

static void
query_info_callback (GObject *source_object,
                     GAsyncResult *res,
                     gpointer user_data)
{
    CajaDirectory *directory;
    CajaFile *get_info_file;
    GFileInfo *info;
    GetInfoState *state;
    GError *error;

    state = user_data;

    if (state->directory == NULL)
    {
        /* Operation was cancelled. Bail out */
        get_info_state_free (state);
        return;
    }

    directory = caja_directory_ref (state->directory);

    get_info_file = directory->details->get_info_file;
    g_assert (CAJA_IS_FILE (get_info_file));

    directory->details->get_info_file = NULL;
    directory->details->get_info_in_progress = NULL;

    /* ref here because we might be removing the last ref when we
     * mark the file gone below, but we need to keep a ref at
     * least long enough to send the change notification.
     */
    caja_file_ref (get_info_file);

    error = NULL;
    info = g_file_query_info_finish (G_FILE (source_object), res, &error);

    if (info == NULL)
    {
        if (error->domain == G_IO_ERROR && error->code == G_IO_ERROR_NOT_FOUND)
        {
            /* mark file as gone */
            caja_file_mark_gone (get_info_file);
        }
        get_info_file->details->file_info_is_up_to_date = TRUE;
        caja_file_clear_info (get_info_file);
        get_info_file->details->get_info_failed = TRUE;
        get_info_file->details->get_info_error = error;
    }
    else
    {
        caja_file_update_info (get_info_file, info);
        g_object_unref (info);
    }

    caja_file_changed (get_info_file);
    caja_file_unref (get_info_file);

    async_job_end (directory, "file info");
    caja_directory_async_state_changed (directory);

    caja_directory_unref (directory);

    get_info_state_free (state);
}

static void
file_info_stop (CajaDirectory *directory)
{
    if (directory->details->get_info_in_progress != NULL)
    {
        CajaFile *file;

        file = directory->details->get_info_file;
        if (file != NULL)
        {
            g_assert (CAJA_IS_FILE (file));
            g_assert (file->details->directory == directory);
            if (is_needy (file, lacks_info, REQUEST_FILE_INFO))
            {
                return;
            }
        }

        /* The info is not wanted, so stop it. */
        file_info_cancel (directory);
    }
}

static void
file_info_start (CajaDirectory *directory,
                 CajaFile *file,
                 gboolean *doing_io)
{
    GFile *location;
    GetInfoState *state;

    file_info_stop (directory);

    if (directory->details->get_info_in_progress != NULL)
    {
        *doing_io = TRUE;
        return;
    }

    if (!is_needy (file, lacks_info, REQUEST_FILE_INFO))
    {
        return;
    }
    *doing_io = TRUE;

    if (!async_job_start (directory, "file info"))
    {
        return;
    }

    directory->details->get_info_file = file;
    file->details->get_info_failed = FALSE;
    if (file->details->get_info_error)
    {
        g_error_free (file->details->get_info_error);
        file->details->get_info_error = NULL;
    }

    state = g_new (GetInfoState, 1);
    state->directory = directory;
    state->cancellable = g_cancellable_new ();

    directory->details->get_info_in_progress = state;

    location = caja_file_get_location (file);
    g_file_query_info_async (location,
                             CAJA_FILE_DEFAULT_ATTRIBUTES,
                             0,
                             G_PRIORITY_DEFAULT,
                             state->cancellable, query_info_callback, state);
    g_object_unref (location);
}

static gboolean is_trusted_system_desktop_file (GFile *file)
{
    gboolean res = FALSE;
    GFileInfo *info;
    const gchar *target = NULL;
    GFile *location = NULL;

    info = g_file_query_info (file,
                              G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                              G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                              NULL,
                              NULL);

    if (info == NULL)
    {
        return FALSE;
    }

    target = g_file_info_get_symlink_target (info);
    if (!target) {
        goto done;
    }

    location = g_file_new_for_path (target);

    res = caja_is_in_system_dir (location);

done:
    if (location) {
        g_object_unref (location);
    }
    g_object_unref (info);

    return res;
}

static gboolean
is_link_trusted (CajaFile *file,
                 gboolean is_launcher)
{
    gboolean res = FALSE;

    if (!is_launcher)
    {
        return TRUE;
    }

    if (caja_file_is_local (file))
    {
        GFile *location;

        location = caja_file_get_location (file);

        res = caja_is_in_system_dir (location) ||
              is_trusted_system_desktop_file (location) ||
              caja_is_in_desktop_dir (location);

        if (!res)
        {
              res = caja_file_get_boolean_metadata (file,
                                                    CAJA_METADATA_KEY_TRUSTED_LAUNCHER,
                                                    FALSE);
        }

        g_object_unref (location);
    }

    return res;
}

static void
link_info_done (CajaDirectory *directory,
                CajaFile *file,
                const char *uri,
                const char *name,
                const char *icon,
                gboolean is_launcher,
                gboolean is_foreign)
{
    gboolean is_trusted;

    file->details->link_info_is_up_to_date = TRUE;

    is_trusted = is_link_trusted (file, is_launcher);

    if (is_trusted)
    {
        caja_file_set_display_name (file, name, name, TRUE);
    }
    else
    {
        caja_file_set_display_name (file, NULL, NULL, TRUE);
    }

    file->details->got_link_info = TRUE;
    g_free (file->details->custom_icon);
    file->details->custom_icon = NULL;
    if (uri)
    {
        g_free (file->details->activation_uri);
        file->details->activation_uri = NULL;
        file->details->got_custom_activation_uri = TRUE;
        file->details->activation_uri = g_strdup (uri);
    }
    if (is_trusted)
    {
        file->details->custom_icon = g_strdup (icon);
    }
    file->details->is_launcher = is_launcher;
    file->details->is_foreign_link = is_foreign;
    file->details->is_trusted_link = is_trusted;

    caja_directory_async_state_changed (directory);
}

static gboolean
should_read_link_info_sync (CajaFile *file)
{
#ifdef READ_LOCAL_LINKS_SYNC
    return (caja_file_is_local (file) && !caja_file_is_directory (file));
#else
    return FALSE;
#endif
}

static void
link_info_stop (CajaDirectory *directory)
{
    if (directory->details->link_info_read_state != NULL)
    {
        CajaFile *file;

        file = directory->details->link_info_read_state->file;

        if (file != NULL)
        {
            g_assert (CAJA_IS_FILE (file));
            g_assert (file->details->directory == directory);
            if (is_needy (file,
                          lacks_link_info,
                          REQUEST_LINK_INFO))
            {
                return;
            }
        }

        /* The link info is not wanted, so stop it. */
        link_info_cancel (directory);
    }
}

static void
link_info_got_data (CajaDirectory *directory,
                    CajaFile *file,
                    gboolean result,
                    goffset bytes_read,
                    char *file_contents)
{
    char *uri, *name, *icon;
    gboolean is_launcher;
    gboolean is_foreign;

    caja_directory_ref (directory);

    uri = NULL;
    name = NULL;
    icon = NULL;
    is_launcher = FALSE;
    is_foreign = FALSE;

    /* Handle the case where we read the Caja link. */
    if (result)
    {
        char *link_uri;

        link_uri = caja_file_get_uri (file);
        caja_link_get_link_info_given_file_contents (file_contents, bytes_read, link_uri,
                &uri, &name, &icon, &is_launcher, &is_foreign);
        g_free (link_uri);
    }
    else
    {
        /* FIXME bugzilla.gnome.org 42433: We should report this error to the user. */
    }

    caja_file_ref (file);
    link_info_done (directory, file, uri, name, icon, is_launcher, is_foreign);
    caja_file_changed (file);
    caja_file_unref (file);

    g_free (uri);
    g_free (name);
    g_free (icon);

    caja_directory_unref (directory);
}

static void
link_info_read_state_free (LinkInfoReadState *state)
{
    g_object_unref (state->cancellable);
    g_free (state);
}

static void
link_info_caja_link_read_callback (GObject *source_object,
                                   GAsyncResult *res,
                                   gpointer user_data)
{
    LinkInfoReadState *state;
    gsize file_size;
    char *file_contents;
    gboolean result;
    CajaDirectory *directory;

    state = user_data;

    if (state->directory == NULL)
    {
        /* Operation was cancelled. Bail out */
        link_info_read_state_free (state);
        return;
    }

    directory = caja_directory_ref (state->directory);

    result = g_file_load_contents_finish (G_FILE (source_object),
                                          res,
                                          &file_contents, &file_size,
                                          NULL, NULL);

    state->directory->details->link_info_read_state = NULL;
    async_job_end (state->directory, "link info");

    link_info_got_data (state->directory, state->file, result, file_size, file_contents);

    if (result)
    {
        g_free (file_contents);
    }

    link_info_read_state_free (state);

    caja_directory_unref (directory);
}

static void
link_info_start (CajaDirectory *directory,
                 CajaFile *file,
                 gboolean *doing_io)
{
    GFile *location;
    gboolean caja_style_link;
    gsize file_size;
    char *file_contents;
    gboolean result;
    LinkInfoReadState *state;

    if (directory->details->link_info_read_state != NULL)
    {
        *doing_io = TRUE;
        return;
    }

    if (!is_needy (file,
                   lacks_link_info,
                   REQUEST_LINK_INFO))
    {
        return;
    }
    *doing_io = TRUE;

    /* Figure out if it is a link. */
    caja_style_link = caja_file_is_caja_link (file);
    location = caja_file_get_location (file);

    /* If it's not a link we are done. If it is, we need to read it. */
    if (!caja_style_link)
    {
        link_info_done (directory, file, NULL, NULL, NULL, FALSE, FALSE);
    }
    else if (should_read_link_info_sync (file))
    {
        result = g_file_load_contents (location, NULL, &file_contents, &file_size, NULL, NULL);
        link_info_got_data (directory, file, result, file_size, file_contents);
        g_free (file_contents);
    }
    else
    {
        if (!async_job_start (directory, "link info"))
        {
            g_object_unref (location);
            return;
        }

        state = g_new0 (LinkInfoReadState, 1);
        state->directory = directory;
        state->file = file;
        state->cancellable = g_cancellable_new ();

        directory->details->link_info_read_state = state;

        g_file_load_contents_async (location,
                                    state->cancellable,
                                    link_info_caja_link_read_callback,
                                    state);
    }
    g_object_unref (location);
}

static void
thumbnail_done (CajaDirectory *directory,
                CajaFile *file,
                GdkPixbuf *pixbuf,
                gboolean tried_original)
{
    file->details->thumbnail_is_up_to_date = TRUE;
    file->details->thumbnail_tried_original  = tried_original;
    if (file->details->thumbnail)
    {
        g_object_unref (file->details->thumbnail);
        file->details->thumbnail = NULL;
    }
    if (pixbuf)
    {
        time_t thumb_mtime = 0;

        if (tried_original)
        {
            thumb_mtime = file->details->mtime;
        }
        else
        {
            const char *thumb_mtime_str;

            thumb_mtime_str = gdk_pixbuf_get_option (pixbuf, "tEXt::Thumb::MTime");
            if (thumb_mtime_str)
            {
                thumb_mtime = atol (thumb_mtime_str);
            }
        }

        if (thumb_mtime == 0 ||
                thumb_mtime == file->details->mtime)
        {
            file->details->thumbnail = g_object_ref (pixbuf);
            file->details->thumbnail_mtime = thumb_mtime;
        }
        else
        {
            g_free (file->details->thumbnail_path);
            file->details->thumbnail_path = NULL;
        }
    }

    caja_directory_async_state_changed (directory);
}

static void
thumbnail_stop (CajaDirectory *directory)
{
    if (directory->details->thumbnail_state != NULL)
    {
        CajaFile *file;

        file = directory->details->thumbnail_state->file;

        if (file != NULL)
        {
            g_assert (CAJA_IS_FILE (file));
            g_assert (file->details->directory == directory);
            if (is_needy (file,
                          lacks_thumbnail,
                          REQUEST_THUMBNAIL))
            {
                return;
            }
        }

        /* The link info is not wanted, so stop it. */
        thumbnail_cancel (directory);
    }
}

static void
thumbnail_got_pixbuf (CajaDirectory *directory,
                      CajaFile *file,
                      GdkPixbuf *pixbuf,
                      gboolean tried_original)
{
    caja_directory_ref (directory);

    caja_file_ref (file);
    thumbnail_done (directory, file, pixbuf, tried_original);
    caja_file_changed (file);
    caja_file_unref (file);

    if (pixbuf)
    {
        g_object_unref (pixbuf);
    }

    caja_directory_unref (directory);
}

static void
thumbnail_state_free (ThumbnailState *state)
{
    g_object_unref (state->cancellable);
    g_free (state);
}

extern int cached_thumbnail_size;

/* scale very large images down to the max. size we need */
static void
thumbnail_loader_size_prepared (GdkPixbufLoader *loader,
                                int width,
                                int height,
                                gpointer user_data)
{
    int max_thumbnail_size;
    double aspect_ratio;

    aspect_ratio = ((double) width) / height;

    /* cf. caja_file_get_icon() */
    max_thumbnail_size = CAJA_ICON_SIZE_LARGEST * cached_thumbnail_size / CAJA_ICON_SIZE_STANDARD;
    if (MAX (width, height) > max_thumbnail_size)
    {
        if (width > height)
        {
            width = max_thumbnail_size;
            height = width / aspect_ratio;
        }
        else
        {
            height = max_thumbnail_size;
            width = height * aspect_ratio;
        }

        gdk_pixbuf_loader_set_size (loader, width, height);
    }
}

static GdkPixbuf *
get_pixbuf_for_content (goffset file_len,
                        char *file_contents)
{
    gboolean res;
    GdkPixbuf *pixbuf, *pixbuf2;
    GdkPixbufLoader *loader;
    gsize chunk_len;
    pixbuf = NULL;

    loader = gdk_pixbuf_loader_new ();
    g_signal_connect (loader, "size-prepared",
                      G_CALLBACK (thumbnail_loader_size_prepared),
                      NULL);

    /* For some reason we have to write in chunks, or gdk-pixbuf fails */
    res = TRUE;
    while (res && file_len > 0)
    {
        chunk_len = file_len;
        res = gdk_pixbuf_loader_write (loader, (guchar *) file_contents, chunk_len, NULL);
        file_contents += chunk_len;
        file_len -= chunk_len;
    }
    if (res)
    {
        res = gdk_pixbuf_loader_close (loader, NULL);
    }
    if (res)
    {
        pixbuf = g_object_ref (gdk_pixbuf_loader_get_pixbuf (loader));
    }
    g_object_unref (G_OBJECT (loader));

    if (pixbuf)
    {
        pixbuf2 = gdk_pixbuf_apply_embedded_orientation (pixbuf);
        g_object_unref (pixbuf);
        pixbuf = pixbuf2;
    }
    return pixbuf;
}

static void
thumbnail_read_callback (GObject *source_object,
                         GAsyncResult *res,
                         gpointer user_data)
{
    ThumbnailState *state;
    gsize file_size;
    char *file_contents;
    gboolean result;
    CajaDirectory *directory;
    GdkPixbuf *pixbuf;

    state = user_data;

    if (state->directory == NULL)
    {
        /* Operation was cancelled. Bail out */
        thumbnail_state_free (state);
        return;
    }

    directory = caja_directory_ref (state->directory);

    result = g_file_load_contents_finish (G_FILE (source_object),
                                          res,
                                          &file_contents, &file_size,
                                          NULL, NULL);

    pixbuf = NULL;
    if (result)
    {
        pixbuf = get_pixbuf_for_content (file_size, file_contents);
        g_free (file_contents);
    }

    if (pixbuf == NULL && state->trying_original)
    {
        GFile *location;

        state->trying_original = FALSE;

        location = g_file_new_for_path (state->file->details->thumbnail_path);
        g_file_load_contents_async (location,
                                    state->cancellable,
                                    thumbnail_read_callback,
                                    state);
        g_object_unref (location);
    }
    else
    {
        state->directory->details->thumbnail_state = NULL;
        async_job_end (state->directory, "thumbnail");

        thumbnail_got_pixbuf (state->directory, state->file, pixbuf, state->tried_original);

        thumbnail_state_free (state);
    }

    caja_directory_unref (directory);
}

static void
thumbnail_start (CajaDirectory *directory,
                 CajaFile *file,
                 gboolean *doing_io)
{
    GFile *location;
    ThumbnailState *state;

    if (directory->details->thumbnail_state != NULL)
    {
        *doing_io = TRUE;
        return;
    }

    if (!is_needy (file,
                   lacks_thumbnail,
                   REQUEST_THUMBNAIL))
    {
        return;
    }
    *doing_io = TRUE;

    if (!async_job_start (directory, "thumbnail"))
    {
        return;
    }

    state = g_new0 (ThumbnailState, 1);
    state->directory = directory;
    state->file = file;
    state->cancellable = g_cancellable_new ();

    if (file->details->thumbnail_wants_original)
    {
        state->tried_original = TRUE;
        state->trying_original = TRUE;
        location = caja_file_get_location (file);
    }
    else
    {
        location = g_file_new_for_path (file->details->thumbnail_path);
    }

    directory->details->thumbnail_state = state;

    g_file_load_contents_async (location,
                                state->cancellable,
                                thumbnail_read_callback,
                                state);
    g_object_unref (location);
}

static void
mount_stop (CajaDirectory *directory)
{
    if (directory->details->mount_state != NULL)
    {
        CajaFile *file;

        file = directory->details->mount_state->file;

        if (file != NULL)
        {
            g_assert (CAJA_IS_FILE (file));
            g_assert (file->details->directory == directory);
            if (is_needy (file,
                          lacks_mount,
                          REQUEST_MOUNT))
            {
                return;
            }
        }

        /* The link info is not wanted, so stop it. */
        mount_cancel (directory);
    }
}

static void
mount_state_free (MountState *state)
{
    g_object_unref (state->cancellable);
    g_free (state);
}

static void
got_mount (MountState *state, GMount *mount)
{
    CajaDirectory *directory;
    CajaFile *file;

    directory = caja_directory_ref (state->directory);

    state->directory->details->mount_state = NULL;
    async_job_end (state->directory, "mount");

    file = caja_file_ref (state->file);

    file->details->mount_is_up_to_date = TRUE;
    caja_file_set_mount (file, mount);

    caja_directory_async_state_changed (directory);
    caja_file_changed (file);

    caja_file_unref (file);

    caja_directory_unref (directory);

    mount_state_free (state);

}

static void
find_enclosing_mount_callback (GObject *source_object,
                               GAsyncResult *res,
                               gpointer user_data)
{
    GMount *mount;
    MountState *state;

    state = user_data;
    if (state->directory == NULL)
    {
        /* Operation was cancelled. Bail out */
        mount_state_free (state);
        return;
    }

    mount = g_file_find_enclosing_mount_finish (G_FILE (source_object),
            res, NULL);

    if (mount)
    {
        GFile *location, *root;

        root = g_mount_get_root (mount);
        location = caja_file_get_location (state->file);
        if (!g_file_equal (location, root))
        {
            g_object_unref (mount);
            mount = NULL;
        }
        g_object_unref (root);
        g_object_unref (location);
    }

    got_mount (state, mount);

    if (mount)
    {
        g_object_unref (mount);
    }
}

static GMount *
get_mount_at (GFile *target)
{
    GVolumeMonitor *monitor;
    GFile *root;
    GList *mounts, *l;
    GMount *found;

    monitor = g_volume_monitor_get ();
    mounts = g_volume_monitor_get_mounts (monitor);

    found = NULL;
    for (l = mounts; l != NULL; l = l->next)
    {
        GMount *mount = G_MOUNT (l->data);

        if (g_mount_is_shadowed (mount))
            continue;

        root = g_mount_get_root (mount);

        if (g_file_equal (target, root))
        {
            found = g_object_ref (mount);
            break;
        }

        g_object_unref (root);
    }

    g_list_free_full (mounts, g_object_unref);

    g_object_unref (monitor);

    return found;
}

static void
mount_start (CajaDirectory *directory,
             CajaFile *file,
             gboolean *doing_io)
{
    GFile *location;
    MountState *state;

    if (directory->details->mount_state != NULL)
    {
        *doing_io = TRUE;
        return;
    }

    if (!is_needy (file,
                   lacks_mount,
                   REQUEST_MOUNT))
    {
        return;
    }
    *doing_io = TRUE;

    if (!async_job_start (directory, "mount"))
    {
        return;
    }

    state = g_new0 (MountState, 1);
    state->directory = directory;
    state->file = file;
    state->cancellable = g_cancellable_new ();

    location = caja_file_get_location (file);

    directory->details->mount_state = state;

    if (file->details->type == G_FILE_TYPE_MOUNTABLE)
    {
        GFile *target;
        GMount *mount;

        mount = NULL;
        target = caja_file_get_activation_location (file);
        if (target != NULL)
        {
            mount = get_mount_at (target);
            g_object_unref (target);
        }

        got_mount (state, mount);

        if (mount)
        {
            g_object_unref (mount);
        }
    }
    else
    {
        g_file_find_enclosing_mount_async (location,
                                           G_PRIORITY_DEFAULT,
                                           state->cancellable,
                                           find_enclosing_mount_callback,
                                           state);
    }
    g_object_unref (location);
}

static void
filesystem_info_cancel (CajaDirectory *directory)
{
    if (directory->details->filesystem_info_state != NULL)
    {
        g_cancellable_cancel (directory->details->filesystem_info_state->cancellable);
        directory->details->filesystem_info_state->directory = NULL;
        directory->details->filesystem_info_state = NULL;
        async_job_end (directory, "filesystem info");
    }
}

static void
filesystem_info_stop (CajaDirectory *directory)
{
    if (directory->details->filesystem_info_state != NULL)
    {
        CajaFile *file;

        file = directory->details->filesystem_info_state->file;

        if (file != NULL)
        {
            g_assert (CAJA_IS_FILE (file));
            g_assert (file->details->directory == directory);
            if (is_needy (file,
                          lacks_filesystem_info,
                          REQUEST_FILESYSTEM_INFO))
            {
                return;
            }
        }

        /* The filesystem info is not wanted, so stop it. */
        filesystem_info_cancel (directory);
    }
}

static void
filesystem_info_state_free (FilesystemInfoState *state)
{
    g_object_unref (state->cancellable);
    g_free (state);
}

static void
got_filesystem_info (FilesystemInfoState *state, GFileInfo *info)
{
    CajaDirectory *directory;
    CajaFile *file;

    /* careful here, info may be NULL */

    directory = caja_directory_ref (state->directory);

    state->directory->details->filesystem_info_state = NULL;
    async_job_end (state->directory, "filesystem info");

    file = caja_file_ref (state->file);

    file->details->filesystem_info_is_up_to_date = TRUE;
    if (info != NULL)
    {
        file->details->filesystem_use_preview =
            g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW);
        file->details->filesystem_readonly =
            g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY);
    }

    caja_directory_async_state_changed (directory);
    caja_file_changed (file);

    caja_file_unref (file);

    caja_directory_unref (directory);

    filesystem_info_state_free (state);
}

static void
query_filesystem_info_callback (GObject *source_object,
                                GAsyncResult *res,
                                gpointer user_data)
{
    GFileInfo *info;
    FilesystemInfoState *state;

    state = user_data;
    if (state->directory == NULL)
    {
        /* Operation was cancelled. Bail out */
        filesystem_info_state_free (state);
        return;
    }

    info = g_file_query_filesystem_info_finish (G_FILE (source_object), res, NULL);

    got_filesystem_info (state, info);

    if (info != NULL)
    {
        g_object_unref (info);
    }
}

static void
filesystem_info_start (CajaDirectory *directory,
                       CajaFile *file,
                       gboolean *doing_io)
{
    GFile *location;
    FilesystemInfoState *state;

    if (directory->details->filesystem_info_state != NULL)
    {
        *doing_io = TRUE;
        return;
    }

    if (!is_needy (file,
                   lacks_filesystem_info,
                   REQUEST_FILESYSTEM_INFO))
    {
        return;
    }
    *doing_io = TRUE;

    if (!async_job_start (directory, "filesystem info"))
    {
        return;
    }

    state = g_new0 (FilesystemInfoState, 1);
    state->directory = directory;
    state->file = file;
    state->cancellable = g_cancellable_new ();

    location = caja_file_get_location (file);

    directory->details->filesystem_info_state = state;

    g_file_query_filesystem_info_async (location,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_READONLY ","
                                        G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW,
                                        G_PRIORITY_DEFAULT,
                                        state->cancellable,
                                        query_filesystem_info_callback,
                                        state);
    g_object_unref (location);
}

static void
extension_info_cancel (CajaDirectory *directory)
{
    if (directory->details->extension_info_in_progress != NULL)
    {
        if (directory->details->extension_info_idle)
        {
            g_source_remove (directory->details->extension_info_idle);
        }
        else
        {
            caja_info_provider_cancel_update
            (directory->details->extension_info_provider,
             directory->details->extension_info_in_progress);
        }

        directory->details->extension_info_in_progress = NULL;
        directory->details->extension_info_file = NULL;
        directory->details->extension_info_provider = NULL;
        directory->details->extension_info_idle = 0;

        async_job_end (directory, "extension info");
    }
}

static void
extension_info_stop (CajaDirectory *directory)
{
    if (directory->details->extension_info_in_progress != NULL)
    {
        CajaFile *file;

        file = directory->details->extension_info_file;
        if (file != NULL)
        {
            g_assert (CAJA_IS_FILE (file));
            g_assert (file->details->directory == directory);
            if (is_needy (file, lacks_extension_info, REQUEST_EXTENSION_INFO))
            {
                return;
            }
        }

        /* The info is not wanted, so stop it. */
        extension_info_cancel (directory);
    }
}

static void
finish_info_provider (CajaDirectory *directory,
                      CajaFile *file,
                      CajaInfoProvider *provider)
{
    file->details->pending_info_providers =
        g_list_remove  (file->details->pending_info_providers,
                        provider);
    g_object_unref (provider);

    caja_directory_async_state_changed (directory);

    if (file->details->pending_info_providers == NULL)
    {
        caja_file_info_providers_done (file);
    }
}

static gboolean
info_provider_idle_callback (gpointer user_data)
{
    InfoProviderResponse *response;
    CajaDirectory *directory;

    response = user_data;
    directory = response->directory;

    if (response->handle != directory->details->extension_info_in_progress
            || response->provider != directory->details->extension_info_provider)
    {
        g_warning ("Unexpected plugin response.  This probably indicates a bug in a Caja extension: handle=%p", response->handle);
    }
    else
    {
        CajaFile *file;
        async_job_end (directory, "extension info");

        file = directory->details->extension_info_file;

        directory->details->extension_info_file = NULL;
        directory->details->extension_info_provider = NULL;
        directory->details->extension_info_in_progress = NULL;
        directory->details->extension_info_idle = 0;

        finish_info_provider (directory, file, response->provider);
    }

    return FALSE;
}

static void
info_provider_callback (CajaInfoProvider *provider,
                        CajaOperationHandle *handle,
                        CajaOperationResult result,
                        gpointer user_data)
{
    InfoProviderResponse *response;

    response = g_new0 (InfoProviderResponse, 1);
    response->provider = provider;
    response->handle = handle;
    response->result = result;
    response->directory = CAJA_DIRECTORY (user_data);

    response->directory->details->extension_info_idle =
        g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                         info_provider_idle_callback, response,
                         g_free);
}

static void
extension_info_start (CajaDirectory *directory,
                      CajaFile *file,
                      gboolean *doing_io)
{
    CajaInfoProvider *provider;
    CajaOperationResult result;
    CajaOperationHandle *handle;
    GClosure *update_complete;

    if (directory->details->extension_info_in_progress != NULL)
    {
        *doing_io = TRUE;
        return;
    }

    if (!is_needy (file, lacks_extension_info, REQUEST_EXTENSION_INFO))
    {
        return;
    }
    *doing_io = TRUE;

    if (!async_job_start (directory, "extension info"))
    {
        return;
    }

    provider = file->details->pending_info_providers->data;

    update_complete = g_cclosure_new (G_CALLBACK (info_provider_callback),
                                      directory,
                                      NULL);
    g_closure_set_marshal (update_complete,
                           caja_marshal_VOID__POINTER_ENUM);

    result = caja_info_provider_update_file_info
             (provider,
              CAJA_FILE_INFO (file),
              update_complete,
              &handle);

    g_closure_unref (update_complete);

    if (result == CAJA_OPERATION_COMPLETE ||
            result == CAJA_OPERATION_FAILED)
    {
        finish_info_provider (directory, file, provider);
        async_job_end (directory, "extension info");
    }
    else
    {
        directory->details->extension_info_in_progress = handle;
        directory->details->extension_info_provider = provider;
        directory->details->extension_info_file = file;
    }
}

static void
start_or_stop_io (CajaDirectory *directory)
{
    CajaFile *file;
    gboolean doing_io;

    /* Start or stop reading files. */
    file_list_start_or_stop (directory);

    /* Stop any no longer wanted attribute fetches. */
    file_info_stop (directory);
    directory_count_stop (directory);
    deep_count_stop (directory);
    mime_list_stop (directory);
    top_left_stop (directory);
    link_info_stop (directory);
    extension_info_stop (directory);
    mount_stop (directory);
    thumbnail_stop (directory);
    filesystem_info_stop (directory);

    doing_io = FALSE;
    /* Take files that are all done off the queue. */
    while (!caja_file_queue_is_empty (directory->details->high_priority_queue))
    {
        file = caja_file_queue_head (directory->details->high_priority_queue);

        /* Start getting attributes if possible */
        file_info_start (directory, file, &doing_io);
        link_info_start (directory, file, &doing_io);

        if (doing_io)
        {
            return;
        }

        move_file_to_low_priority_queue (directory, file);
    }

    /* High priority queue must be empty */
    while (!caja_file_queue_is_empty (directory->details->low_priority_queue))
    {
        file = caja_file_queue_head (directory->details->low_priority_queue);

        /* Start getting attributes if possible */
        mount_start (directory, file, &doing_io);
        directory_count_start (directory, file, &doing_io);
        deep_count_start (directory, file, &doing_io);
        mime_list_start (directory, file, &doing_io);
        top_left_start (directory, file, &doing_io);
        thumbnail_start (directory, file, &doing_io);
        filesystem_info_start (directory, file, &doing_io);

        if (doing_io)
        {
            return;
        }

        move_file_to_extension_queue (directory, file);
    }

    /* Low priority queue must be empty */
    while (!caja_file_queue_is_empty (directory->details->extension_queue))
    {
        file = caja_file_queue_head (directory->details->extension_queue);

        /* Start getting attributes if possible */
        extension_info_start (directory, file, &doing_io);
        if (doing_io)
        {
            return;
        }

        caja_directory_remove_file_from_work_queue (directory, file);
    }
}

/* Call this when the monitor or call when ready list changes,
 * or when some I/O is completed.
 */
void
caja_directory_async_state_changed (CajaDirectory *directory)
{
    /* Check if any callbacks are satisfied and call them if they
     * are. Do this last so that any changes done in start or stop
     * I/O functions immediately (not in callbacks) are taken into
     * consideration. If any callbacks are called, consider the
     * I/O state again so that we can release or cancel I/O that
     * is not longer needed once the callbacks are satisfied.
     */

    if (directory->details->in_async_service_loop)
    {
        directory->details->state_changed = TRUE;
        return;
    }
    directory->details->in_async_service_loop = TRUE;
    caja_directory_ref (directory);
    do
    {
        directory->details->state_changed = FALSE;
        start_or_stop_io (directory);
        if (call_ready_callbacks (directory))
        {
            directory->details->state_changed = TRUE;
        }
    }
    while (directory->details->state_changed);
    directory->details->in_async_service_loop = FALSE;
    caja_directory_unref (directory);

    /* Check if any directories should wake up. */
    async_job_wake_up ();
}

void
caja_directory_cancel (CajaDirectory *directory)
{
    /* Arbitrary order (kept alphabetical). */
    deep_count_cancel (directory);
    directory_count_cancel (directory);
    file_info_cancel (directory);
    file_list_cancel (directory);
    link_info_cancel (directory);
    mime_list_cancel (directory);
    new_files_cancel (directory);
    top_left_cancel (directory);
    extension_info_cancel (directory);
    thumbnail_cancel (directory);
    mount_cancel (directory);
    filesystem_info_cancel (directory);

    /* We aren't waiting for anything any more. */
    if (waiting_directories != NULL)
    {
        g_hash_table_remove (waiting_directories, directory);
    }

    /* Check if any directories should wake up. */
    async_job_wake_up ();
}

static void
cancel_directory_count_for_file (CajaDirectory *directory,
                                 CajaFile      *file)
{
    if (directory->details->count_in_progress != NULL &&
            directory->details->count_in_progress->count_file == file)
    {
        directory_count_cancel (directory);
    }
}

static void
cancel_deep_counts_for_file (CajaDirectory *directory,
                             CajaFile      *file)
{
    if (directory->details->deep_count_file == file)
    {
        deep_count_cancel (directory);
    }
}

static void
cancel_mime_list_for_file (CajaDirectory *directory,
                           CajaFile      *file)
{
    if (directory->details->mime_list_in_progress != NULL &&
            directory->details->mime_list_in_progress->mime_list_file == file)
    {
        mime_list_cancel (directory);
    }
}

static void
cancel_top_left_text_for_file (CajaDirectory *directory,
                               CajaFile      *file)
{
    if (directory->details->top_left_read_state != NULL &&
            directory->details->top_left_read_state->file == file)
    {
        top_left_cancel (directory);
    }
}

static void
cancel_file_info_for_file (CajaDirectory *directory,
                           CajaFile      *file)
{
    if (directory->details->get_info_file == file)
    {
        file_info_cancel (directory);
    }
}

static void
cancel_thumbnail_for_file (CajaDirectory *directory,
                           CajaFile      *file)
{
    if (directory->details->thumbnail_state != NULL &&
            directory->details->thumbnail_state->file == file)
    {
        thumbnail_cancel (directory);
    }
}

static void
cancel_mount_for_file (CajaDirectory *directory,
                       CajaFile      *file)
{
    if (directory->details->mount_state != NULL &&
            directory->details->mount_state->file == file)
    {
        mount_cancel (directory);
    }
}

static void
cancel_filesystem_info_for_file (CajaDirectory *directory,
                                 CajaFile      *file)
{
    if (directory->details->filesystem_info_state != NULL &&
            directory->details->filesystem_info_state->file == file)
    {
        filesystem_info_cancel (directory);
    }
}

static void
cancel_link_info_for_file (CajaDirectory *directory,
                           CajaFile      *file)
{
    if (directory->details->link_info_read_state != NULL &&
            directory->details->link_info_read_state->file == file)
    {
        link_info_cancel (directory);
    }
}

static void
cancel_loading_attributes (CajaDirectory *directory,
                           CajaFileAttributes file_attributes)
{
    Request request;

    request = caja_directory_set_up_request (file_attributes);

    if (REQUEST_WANTS_TYPE (request, REQUEST_DIRECTORY_COUNT))
    {
        directory_count_cancel (directory);
    }
    if (REQUEST_WANTS_TYPE (request, REQUEST_DEEP_COUNT))
    {
        deep_count_cancel (directory);
    }
    if (REQUEST_WANTS_TYPE (request, REQUEST_MIME_LIST))
    {
        mime_list_cancel (directory);
    }
    if (REQUEST_WANTS_TYPE (request, REQUEST_TOP_LEFT_TEXT))
    {
        top_left_cancel (directory);
    }
    if (REQUEST_WANTS_TYPE (request, REQUEST_FILE_INFO))
    {
        file_info_cancel (directory);
    }
    if (REQUEST_WANTS_TYPE (request, REQUEST_FILESYSTEM_INFO))
    {
        filesystem_info_cancel (directory);
    }
    if (REQUEST_WANTS_TYPE (request, REQUEST_LINK_INFO))
    {
        link_info_cancel (directory);
    }

    if (REQUEST_WANTS_TYPE (request, REQUEST_EXTENSION_INFO))
    {
        extension_info_cancel (directory);
    }

    if (REQUEST_WANTS_TYPE (request, REQUEST_THUMBNAIL))
    {
        thumbnail_cancel (directory);
    }

    if (REQUEST_WANTS_TYPE (request, REQUEST_MOUNT))
    {
        mount_cancel (directory);
    }

    caja_directory_async_state_changed (directory);
}

void
caja_directory_cancel_loading_file_attributes (CajaDirectory      *directory,
        CajaFile           *file,
        CajaFileAttributes  file_attributes)
{
    Request request;

    caja_directory_remove_file_from_work_queue (directory, file);

    request = caja_directory_set_up_request (file_attributes);

    if (REQUEST_WANTS_TYPE (request, REQUEST_DIRECTORY_COUNT))
    {
        cancel_directory_count_for_file (directory, file);
    }
    if (REQUEST_WANTS_TYPE (request, REQUEST_DEEP_COUNT))
    {
        cancel_deep_counts_for_file (directory, file);
    }
    if (REQUEST_WANTS_TYPE (request, REQUEST_MIME_LIST))
    {
        cancel_mime_list_for_file (directory, file);
    }
    if (REQUEST_WANTS_TYPE (request, REQUEST_TOP_LEFT_TEXT))
    {
        cancel_top_left_text_for_file (directory, file);
    }
    if (REQUEST_WANTS_TYPE (request, REQUEST_FILE_INFO))
    {
        cancel_file_info_for_file (directory, file);
    }
    if (REQUEST_WANTS_TYPE (request, REQUEST_FILESYSTEM_INFO))
    {
        cancel_filesystem_info_for_file (directory, file);
    }
    if (REQUEST_WANTS_TYPE (request, REQUEST_LINK_INFO))
    {
        cancel_link_info_for_file (directory, file);
    }
    if (REQUEST_WANTS_TYPE (request, REQUEST_THUMBNAIL))
    {
        cancel_thumbnail_for_file (directory, file);
    }
    if (REQUEST_WANTS_TYPE (request, REQUEST_MOUNT))
    {
        cancel_mount_for_file (directory, file);
    }

    caja_directory_async_state_changed (directory);
}

void
caja_directory_add_file_to_work_queue (CajaDirectory *directory,
                                       CajaFile *file)
{
    g_return_if_fail (file->details->directory == directory);

    caja_file_queue_enqueue (directory->details->high_priority_queue,
                             file);
}

static void
add_all_files_to_work_queue (CajaDirectory *directory)
{
    GList *node;

    for (node = directory->details->file_list; node != NULL; node = node->next)
    {
        CajaFile *file;

        file = CAJA_FILE (node->data);

        caja_directory_add_file_to_work_queue (directory, file);
    }
}

void
caja_directory_remove_file_from_work_queue (CajaDirectory *directory,
        CajaFile *file)
{
    caja_file_queue_remove (directory->details->high_priority_queue,
                            file);
    caja_file_queue_remove (directory->details->low_priority_queue,
                            file);
    caja_file_queue_remove (directory->details->extension_queue,
                            file);
}

static void
move_file_to_low_priority_queue (CajaDirectory *directory,
                                 CajaFile *file)
{
    /* Must add before removing to avoid ref underflow */
    caja_file_queue_enqueue (directory->details->low_priority_queue,
                             file);
    caja_file_queue_remove (directory->details->high_priority_queue,
                            file);
}

static void
move_file_to_extension_queue (CajaDirectory *directory,
                              CajaFile *file)
{
    /* Must add before removing to avoid ref underflow */
    caja_file_queue_enqueue (directory->details->extension_queue,
                             file);
    caja_file_queue_remove (directory->details->low_priority_queue,
                            file);
}
