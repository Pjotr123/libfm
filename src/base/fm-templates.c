/*
 *      fm-templates.c
 *
 *      Copyright 2012 Andriy Grytsenko (LStranger) <andrej@rep.kiev.ua>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *      MA 02110-1301, USA.
 */

/**
 * SECTION:fm-templates
 * @short_description: Templates for new files creation.
 * @title: FmTemplate
 *
 * @include: libfm/fm.h
 *
 * The #FmTemplate object represents description which files was set for
 * creation and how those files should be created - that includes custom
 * prompt, file name template, and template contents.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fm-templates.h"
#include "fm-monitor.h"
#include "fm-dir-list-job.h"

typedef struct _FmTemplateFile  FmTemplateFile;
typedef struct _FmTemplateDir   FmTemplateDir;

struct _FmTemplate
{
    GObject parent;
    FmTemplateFile *files; /* not referenced, it references instead */
    FmMimeType *mime_type;
    FmPath *template_file;
    FmIcon *icon;
    gchar *command;
    gchar *prompt;
    gchar *label;
};

struct _FmTemplateClass
{
    GObjectClass parent;
};


static void fm_template_finalize(GObject *object);

G_DEFINE_TYPE(FmTemplate, fm_template, G_TYPE_OBJECT);

static void fm_template_class_init(FmTemplateClass *klass)
{
    GObjectClass *g_object_class;

    g_object_class = G_OBJECT_CLASS(klass);
    g_object_class->finalize = fm_template_finalize;
}

static GList *templates = NULL; /* in appearance reversed order */
G_LOCK_DEFINE(templates);

static void fm_template_finalize(GObject *object)
{
    FmTemplate *self;

    g_return_if_fail(FM_IS_TEMPLATE(object));
    G_LOCK(templates);
    templates = g_list_remove(templates, object);
    G_UNLOCK(templates);
    self = (FmTemplate*)object;
    if(self->files)
        g_error("template reference failure");
    fm_mime_type_unref(self->mime_type);
    if(self->template_file)
        fm_path_unref(self->template_file);
    if(self->icon)
        fm_icon_unref(self->icon);
    g_free(self->command);
    g_free(self->prompt);

    G_OBJECT_CLASS(fm_template_parent_class)->finalize(object);
}

static void fm_template_init(FmTemplate *self)
{
}

static FmTemplate* fm_template_new(void)
{
    return (FmTemplate*)g_object_new(FM_TEMPLATE_TYPE, NULL);
}


struct _FmTemplateFile
{
    /* using 'built-in' GList/GSList for speed and less memory consuming */
    FmTemplateFile *next_in_dir;
    FmTemplateFile *prev_in_dir;
    FmTemplateDir *dir;
    FmTemplateFile *next_in_templ; /* in priority-less order */
    /* referenced */
    FmTemplate *templ;
    FmPath *path;
    gboolean is_desktop_entry : 1;
    gboolean inactive : 1;
};

struct _FmTemplateDir
{
    /* using 'built-in' GSList for speed and less memory consuming */
    FmTemplateDir *next;
    FmTemplateFile *files;
    /* referenced */
    FmPath *path;
    GFileMonitor *monitor;
    gboolean user_dir : 1;
};

/* allocated once, on init */
static FmTemplateDir *templates_dirs = NULL;


/* determine mime type for the template
   using fm_mime_type_* for this isn't appropriate because
   we need completely another guessing for templates content */
static FmMimeType *_fm_template_guess_mime_type(FmPath *path,
                                                gboolean is_desktop_entry)
{
    const gchar *basename = fm_path_get_basename(path);
    FmPath *subpath = NULL;
    gchar *filename, *type, *url;
    FmMimeType *mime_type;
    GKeyFile *kf;
    gboolean uncertain;
    struct stat st;

    /* if file is desktop entry then find the real template file path */
    if(is_desktop_entry)
    {
        /* parse file to find mime type */
        kf = g_key_file_new();
        filename = fm_path_to_str(path);
        type = NULL;
        if(g_key_file_load_from_file(kf, filename, G_KEY_FILE_NONE, NULL))
        {
            /* some templates may have 'MimeType' key */
            type = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                         G_KEY_FILE_DESKTOP_KEY_MIME_TYPE, NULL);
            if(!type)
            {
                /* valid template should have 'URL' key */
                url = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                            G_KEY_FILE_DESKTOP_KEY_URL, NULL);
                if(url)
                {
                    if(G_UNLIKELY(url[0] == '/')) /* absolute path */
                        subpath = fm_path_new_for_str(url);
                    else /* path relative to directory containing file */
                        subpath = fm_path_new_relative(fm_path_get_parent(path), url);
                    path = subpath; /* shift to new path */
                    basename = fm_path_get_basename(path);
                    g_free(url);
                }
            }
        }
        g_key_file_free(kf);
        g_free(filename);
        if(type)
            goto _found;
    }
    /* so we have real template file now, guess from file name first */
    if(g_str_has_suffix(basename, ".desktop")) /* template file is an entry */
    {
        kf = g_key_file_new();
        g_free(type);
        type = NULL;
        filename = fm_path_to_str(path);
        if(g_key_file_load_from_file(kf, filename, G_KEY_FILE_NONE, NULL))
        {
            url = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                        G_KEY_FILE_DESKTOP_KEY_TYPE, NULL);
            if(url)
            {
                /* TODO: we support only 'Application' type for now */
                if(strcmp(url, G_KEY_FILE_DESKTOP_TYPE_APPLICATION) == 0)
                    type = "application/x-desktop";
                g_free(url);
            }
        }
        g_key_file_free(kf);
        if(type) /* type is const string, see above */
        {
            g_free(filename);
            if(subpath)
                fm_path_unref(subpath);
            return fm_mime_type_from_name(type);
        }
    }
    else
    {
        type = g_content_type_guess(basename, NULL, 0, &uncertain);
        if(type && !uncertain)
            goto _found;
        g_free(type);
        filename = fm_path_to_str(path);
    }
    /* no result from name - try file attributes */
    uncertain = (stat(filename, &st) < 0);
    g_free(filename);
    if(subpath) /* we don't need it anymore so free now */
        fm_path_unref(subpath);
    if(uncertain) /* no such file or directory */
        return NULL;
    if(S_ISDIR(st.st_mode))
        return fm_mime_type_from_name("inode/directory");
    /* FIXME: should we support templates for devices too? */
    return NULL;

_found:
    if(subpath)
        fm_path_unref(subpath);
    mime_type = fm_mime_type_from_name(type);
    g_free(type);
    return mime_type;
}

/* find or create new FmTemplate */
static FmTemplate *_fm_template_find_for_file(FmPath *path,
                                              gboolean is_desktop_entry)
{
    GList *l;
    FmTemplate *templ;
    FmMimeType *mime_type = _fm_template_guess_mime_type(path, is_desktop_entry);

    if(!mime_type)
    {
        /* g_debug("could not guess MIME type for template %s, ignoring it",
                fm_path_get_basename(path)); */
        return NULL;
    }
    for(l = templates; l; l = l->next)
        if(((FmTemplate*)l->data)->mime_type == mime_type)
        {
            fm_mime_type_unref(mime_type);
            return g_object_ref(l->data);
        }
    templ = fm_template_new();
    templ->mime_type = mime_type;
    templates = g_list_prepend(templates, templ);
    return templ;
}

static void _fm_template_update_from_file(FmTemplate *templ, FmTemplateFile *file)
{
    if(file == NULL)
        return;
    /* update from less relevant file first */
    _fm_template_update_from_file(templ, file->next_in_templ);
    if(file->is_desktop_entry) /* desktop entry */
    {
        GKeyFile *kf = g_key_file_new();
        char *filename = fm_path_to_str(file->path);
        char *tmp;
        GError *error;
        gboolean hidden;

        if(g_key_file_load_from_file(kf, filename, G_KEY_FILE_NONE, &error))
        {
            hidden = g_key_file_get_boolean(kf, G_KEY_FILE_DESKTOP_GROUP,
                                            G_KEY_FILE_DESKTOP_KEY_HIDDEN, NULL);
            file->inactive = hidden;
            /* FIXME: test for G_KEY_FILE_DESKTOP_KEY_ONLY_SHOW_IN ? */
            if(!hidden)
            {
                tmp = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                            G_KEY_FILE_DESKTOP_KEY_URL, NULL);
                if(tmp)
                {
                    if(templ->template_file)
                        fm_path_unref(templ->template_file);
                    if(G_UNLIKELY(tmp[0] == '/')) /* absolute path */
                        templ->template_file = fm_path_new_for_str(tmp);
                    else /* path relative to directory containing file */
                        templ->template_file = fm_path_new_relative(file->dir->path, tmp);
                    g_free(tmp);
                }
                tmp = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                            G_KEY_FILE_DESKTOP_KEY_ICON, NULL);
                if(tmp)
                {
                    if(templ->icon)
                        fm_icon_unref(templ->icon);
                    templ->icon = fm_icon_from_name(tmp);
                    g_free(tmp);
                }
                tmp = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                            G_KEY_FILE_DESKTOP_KEY_EXEC, NULL);
                if(tmp)
                {
                    g_free(templ->command);
                    templ->command = tmp;
                }
                tmp = g_key_file_get_locale_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                            G_KEY_FILE_DESKTOP_KEY_NAME, NULL, NULL);
                if(tmp)
                {
                    g_free(templ->label);
                    templ->label = tmp;
                }
                tmp = g_key_file_get_locale_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                            G_KEY_FILE_DESKTOP_KEY_COMMENT, NULL, NULL);
                if(tmp)
                {
                    g_free(templ->prompt);
                    templ->prompt = tmp;
                }
                /* FIXME: forge prompt from 'Name' if not set yet? */
            }
        }
        else
        {
            g_warning("problem loading template %s: %s", filename, error->message);
            g_error_free(error);
        }
        g_key_file_free(kf);
        g_free(filename);
    }
    else /* plain file */
        if(!templ->template_file)
            templ->template_file = fm_path_ref(file->path);
}

/* recreate data in FmTemplate from entries */
static void _fm_template_update(FmTemplate *templ)
{
    /* free all data */
    if(templ->template_file)
        fm_path_unref(templ->template_file);
    templ->template_file = NULL;
    if(templ->icon)
        fm_icon_unref(templ->icon);
    templ->icon = NULL;
    g_free(templ->command);
    templ->command = NULL;
    g_free(templ->prompt);
    templ->prompt = NULL;
    /* update from file list */
    _fm_template_update_from_file(templ, templ->files);
}

/* add file into FmTemplate and update */
static void _fm_template_insert_sorted(FmTemplate *templ, FmTemplateFile *file)
{
    FmTemplateDir *last_dir = templates_dirs;
    FmTemplateFile *last = NULL, *next;

    for(next = templ->files; next; next = next->next_in_templ)
    {
        while(last_dir && last_dir != file->dir && last_dir != next->dir)
            last_dir = last_dir->next;
        if(!last_dir) /* FIXME: it must be corruption, g_error() it */
            break;
        if(last_dir == file->dir)
        {
            if(next->dir == last_dir && !file->is_desktop_entry)
            {
                if(!next->is_desktop_entry)
                    break;
                /* sort files after desktop items */
            }
            else
                break;
        }
        last = next;
    }
    file->next_in_templ = next;
    if(last)
        last->next_in_templ = file;
    else
        templ->files = file;
    _fm_template_update(templ);
}

/* delete file from FmTemplate and free it */
static void _fm_template_file_free(FmTemplate *templ, FmTemplateFile *file,
                                   gboolean do_update)
{
    FmTemplateFile *file2 = templ->files;

    if(file2 == file)
        templ->files = file->next_in_templ;
    else while(file2)
    {
        if(file2->next_in_templ == file)
        {
            file2->next_in_templ = file->next_in_templ;
            break;
        }
        file2 = file2->next_in_templ;
    }
    if(!file2)
        g_critical("FmTemplate: file being freed is missed in template");
    g_object_unref(templ);
    fm_path_unref(file->path);
    g_slice_free(FmTemplateFile, file);
    if(do_update)
        _fm_template_update(templ);
}

static void on_job_finished(FmJob *job, FmTemplateDir *dir)
{
    GList *file_infos, *l;
    FmFileInfo *fi;
    FmPath *path;
    FmTemplateFile *file;
    FmTemplate *templ;

    g_signal_handlers_disconnect_by_func(job, on_job_finished, dir);
    file_infos = fm_file_info_list_peek_head_link(fm_dir_list_job_get_files(FM_DIR_LIST_JOB(job)));
    for(l = file_infos; l; l = l->next)
    {
        fi = l->data;
        path = fm_file_info_get_path(fi);
        for(file = dir->files; file; file = file->next_in_dir)
            if(fm_path_equal(path, file->path))
                break;
        if(file) /* it's duplicate */
            continue;
        /* ensure the path is based on dir->path */
        path = fm_path_new_child(dir->path, fm_path_get_basename(path));
        templ = _fm_template_find_for_file(path, fm_file_info_is_desktop_entry(fi));
        if(!templ) /* mime type guessing error */
        {
            fm_path_unref(path);
            continue;
        }
        file = g_slice_new(FmTemplateFile);
        file->templ = templ;
        file->path = path;
        file->is_desktop_entry = fm_file_info_is_desktop_entry(fi);
        file->dir = dir;
        file->next_in_dir = dir->files;
        file->prev_in_dir = NULL;
        if(dir->files)
            dir->files->prev_in_dir = file;
        dir->files = file;
        _fm_template_insert_sorted(templ, file);
    }
}

static void on_dir_changed(GFileMonitor *mon, GFile *gf, GFile *other,
                           GFileMonitorEvent evt, FmTemplateDir *dir)
{
    GFile *gfile = fm_path_to_gfile(dir->path);
    char *basename;
    FmTemplateFile *file;
    FmPath *path;
    FmTemplate *templ;
    gboolean is_desktop_entry;

    if(g_file_equal(gf, gfile))
    {
        /* it's event on folder itself, ignoring */
        g_object_unref(gfile);
        return;
    }
    g_object_unref(gfile);
    G_LOCK(templates);
    switch(evt)
    {
    case G_FILE_MONITOR_EVENT_CHANGED:
        basename = g_file_get_basename(gf);
        for(file = dir->files; file; file = file->next_in_dir)
            if(strcmp(fm_path_get_basename(file->path), basename) == 0)
                break;
        g_free(basename);
        if(file)
        {
            if(file->is_desktop_entry) /* we aware only of entries content */
                _fm_template_update(file->templ);
        }
        else
            g_warning("templates monitor: change for unknown file");
        break;
    case G_FILE_MONITOR_EVENT_DELETED:
        basename = g_file_get_basename(gf);
        for(file = dir->files; file; file = file->next_in_dir)
            if(strcmp(fm_path_get_basename(file->path), basename) == 0)
                break;
        g_free(basename);
        if(file)
        {
            if(file == dir->files)
                dir->files = file->next_in_dir;
            else
                file->prev_in_dir->next_in_dir = file->next_in_dir;
            if(file->next_in_dir)
                file->next_in_dir->prev_in_dir = file->prev_in_dir;
            _fm_template_file_free(file->templ, file, TRUE);
        }
        /* else it is already deleted */
        break;
    case G_FILE_MONITOR_EVENT_CREATED:
        basename = g_file_get_basename(gf);
        for(file = dir->files; file; file = file->next_in_dir)
            if(strcmp(fm_path_get_basename(file->path), basename) == 0)
                break;
        if(!file)
        {
            path = fm_path_new_child(dir->path, basename);
            is_desktop_entry = g_str_has_suffix(basename, ".desktop");
            templ = _fm_template_find_for_file(path, is_desktop_entry);
            if(templ)
            {
                file = g_slice_new(FmTemplateFile);
                file->templ = templ;
                file->path = path;
                file->is_desktop_entry = is_desktop_entry;
                file->dir = dir;
                file->next_in_dir = dir->files;
                file->prev_in_dir = NULL;
                dir->files->prev_in_dir = file;
                dir->files = file;
                _fm_template_insert_sorted(templ, file);
            }
            else
            {
                fm_path_unref(path);
                g_warning("could not guess type of template %s, ignoring it",
                          basename);
            }
        }
        else
            g_debug("templates monitor: duplicate file %s", basename);
        g_free(basename);
        break;
    case G_FILE_MONITOR_EVENT_MOVED:
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
    case G_FILE_MONITOR_EVENT_UNMOUNTED:
        /* ignore those */
        break;
    }
    G_UNLOCK(templates);
}

static void _template_dir_init(FmTemplateDir *dir, GFile *gf)
{
    FmDirListJob *job = fm_dir_list_job_new_for_gfile(gf);
    GError *error = NULL;

    dir->files = NULL;
    g_signal_connect(job, "finished", G_CALLBACK(on_job_finished), dir);
    if(!fm_job_run_async(FM_JOB(job)))
        g_signal_handlers_disconnect_by_func(job, on_job_finished, dir);
    dir->monitor = fm_monitor_directory(gf, &error);
    if(dir->monitor)
        g_signal_connect(dir->monitor, "changed", G_CALLBACK(on_dir_changed), dir);
    else
    {
        g_debug("file monitor cannot be created: %s", error->message);
        g_error_free(error);
    }
    g_object_unref(job);
}

void _fm_templates_init(void)
{
    const gchar * const *data_dirs = g_get_system_data_dirs();
    const gchar * const *data_dir;
    const gchar *dir_name;
    FmPath *parent_path, *path;
    FmTemplateDir *dir = NULL;
    GFile *gfile;

    if(templates_dirs)
        return; /* someone called us again? */
    /* prepare list of system template directories */
    for(data_dir = data_dirs; *data_dir; ++data_dir)
    {
        parent_path = fm_path_new_for_str(*data_dir);
        path = fm_path_new_child(parent_path, "templates");
        fm_path_unref(parent_path);
        gfile = fm_path_to_gfile(path);
        if(g_file_query_exists(gfile, NULL))
        {
            if(G_LIKELY(dir))
            {
                dir->next = g_slice_new(FmTemplateDir);
                dir = dir->next;
            }
            else
                templates_dirs = dir = g_slice_new(FmTemplateDir);
            dir->path = path;
            _template_dir_init(dir, gfile);
        }
        else
            fm_path_unref(path);
        g_object_unref(gfile);
    }
    if(G_LIKELY(dir))
        dir->next = NULL;
    /* add templates dir in user data */
    dir = g_slice_new(FmTemplateDir);
    dir->next = templates_dirs;
    templates_dirs = dir;
    parent_path = fm_path_new_for_str(g_get_user_data_dir());
    dir->path = fm_path_new_child(parent_path, "templates");
    fm_path_unref(parent_path);
    dir->user_dir = TRUE;
    gfile = fm_path_to_gfile(dir->path);
    /* FIXME: create it if it doesn't exist? */
    _template_dir_init(dir, gfile);
    g_object_unref(gfile);
    /* add XDG_TEMPLATES_DIR at last */
    dir = g_slice_new(FmTemplateDir);
    dir->next = templates_dirs;
    templates_dirs = dir;
    dir_name = g_get_user_special_dir(G_USER_DIRECTORY_TEMPLATES);
    if(dir_name)
        dir->path = fm_path_new_for_str(dir_name);
    else
        dir->path = fm_path_new_child(fm_path_get_home(), "Templates");
    dir->user_dir = TRUE;
    gfile = fm_path_to_gfile(dir->path);
    if(!g_file_query_exists(gfile, NULL))
        /* create it if it doesn't exist -- ignore errors */
        g_file_make_directory(gfile, NULL, NULL);
    _template_dir_init(dir, gfile);
    g_object_unref(gfile);
    /* jobs will fill list of files async */
}

void _fm_templates_finalize(void)
{
    FmTemplateDir *dir;
    FmTemplateFile *file;

    while(templates_dirs)
    {
        dir = templates_dirs;
        templates_dirs = dir->next;
        fm_path_unref(dir->path);
        if(dir->monitor)
        {
            g_signal_handlers_disconnect_by_func(dir->monitor, on_dir_changed, dir);
            g_object_unref(dir->monitor);
        }
        while(dir->files)
        {
            file = dir->files;
            dir->files = file->next_in_dir;
            if(dir->files)
                dir->files->prev_in_dir = NULL;
            _fm_template_file_free(file->templ, file, FALSE);
        }
        g_slice_free(FmTemplateDir, dir);
    }
}

/**
 * fm_template_list_all
 * @user_only: %TRUE to ignore system templates
 *
 * Retrieves list of all templates. Returned data should be freed after
 * usage with g_list_free_full(list, g_object_unref).
 *
 * Returns: (transfer full) (element-type FmTemplate): list of all known templates.
 *
 * Since: 1.2.0
 */
GList *fm_template_list_all(gboolean user_only)
{
    GList *list = NULL, *l;

    G_LOCK(templates);
    for(l = templates; l; l = l->next)
        if(!((FmTemplate*)l->data)->files->inactive &&
           (!user_only || ((FmTemplate*)l->data)->files->dir->user_dir))
            list = g_list_prepend(list, g_object_ref(l->data));
    G_UNLOCK(templates);
    return list;
}

/**
 * fm_template_dup_name
 * @templ: a template descriptor
 * @nlen: (allow-none): location to get template name length
 *
 * Retrieves file name template for @templ. If @nlen isn't %NULL the it
 * will receive length of file name template without suffix. Returned
 * data should be freed with g_free() after usage.
 *
 * Returns: (transfer full): file name template.
 *
 * Since: 1.2.0
 */
char *fm_template_dup_name(FmTemplate *templ, gint *nlen)
{
    char *name;

    G_LOCK(templates);
    name = templ->template_file ? g_strdup(fm_path_get_basename(templ->template_file)) : NULL;
    G_UNLOCK(templates);
    if(nlen && name)
    {
        char *point;
        if(!name)
            *nlen = 0;
        else if((point = strrchr(name, '.')))
            *nlen = (point - name);
        else
            *nlen = strlen(name);
    }
    return name;
}

/**
 * fm_template_get_mime_type
 * @templ: a template descriptor
 *
 * Retrieves MIME type descriptor for @templ. Returned data are owned by
 * @templ and should be not freed by caller.
 *
 * Returns: (transfer none): mime type descriptor.
 *
 * Since: 1.2.0
 */
FmMimeType *fm_template_get_mime_type(FmTemplate *templ)
{
    return templ->mime_type;
}

/**
 * fm_template_icon
 * @templ: a template descriptor
 *
 * Retrieves icon defined for @templ. Returned data should be freed with
 * fm_icon_unref() after usage.
 *
 * Returns: (transfer full): icon for template.
 *
 * Since: 1.2.0
 */
FmIcon *fm_template_icon(FmTemplate *templ)
{
    FmIcon *icon = NULL;

    G_LOCK(templates);
    if(templ->icon)
        icon = fm_icon_ref(templ->icon);
    G_UNLOCK(templates);
    if(!icon && (icon = fm_mime_type_get_icon(templ->mime_type)))
        fm_icon_ref(icon);
    return icon;
}

/**
 * fm_template_dup_prompt
 * @templ: a template descriptor
 *
 * Retrieves prompt for @templ. It can be used as label in entry for the
 * desired name. If no prompt is defined then returns %NULL. Returned
 * data should be freed with g_free() after usage.
 *
 * Returns: (transfer full): file prompt.
 *
 * Since: 1.2.0
 */
char *fm_template_dup_prompt(FmTemplate *templ)
{
    char *name;

    G_LOCK(templates);
    name = templ->prompt ? g_strdup(templ->prompt) : NULL;
    G_UNLOCK(templates);
    return name;
}

/**
 * fm_template_dup_label
 * @templ: a template descriptor
 *
 * Retrieves label for @templ. It can be used as label in menu. Returned
 * data should be freed with g_free() after usage.
 *
 * Returns: (transfer full): template label.
 *
 * Since: 1.2.0
 */
char *fm_template_dup_label(FmTemplate *templ)
{
    char *name;

    G_LOCK(templates);
    name = templ->label ? g_strdup(templ->label) : NULL;
    G_UNLOCK(templates);
    return name;
}

/**
 * fm_template_is_directory
 * @templ: a template descriptor
 *
 * Checks if @templ is directory template.
 *
 * Returns: %TRUE if @templ is directory template.
 *
 * Since: 1.2.0
 */
gboolean fm_template_is_directory(FmTemplate *templ)
{
    return (templ->mime_type == _fm_mime_type_get_inode_directory());
}

/**
 * fm_template_create_file
 * @templ: a template descriptor
 * @path: path to file to create
 * @error: (allow-none): location to retrieve error
 *
 * Tries to create file at @path using rules of creating from @templ.
 *
 * Returns: %TRUE if file creation started successfully.
 *
 * Since: 1.2.0
 */
gboolean fm_template_create_file(FmTemplate *templ, GFile *path, GError **error)
{
    char *command;
    GAppInfo *app;
    GFile *tfile;
    GList *list;
    gboolean ret;

    if(!FM_IS_TEMPLATE(templ) || !G_IS_FILE(path))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "fm_template_create_file: invalid argument");
        return FALSE;
    }
    G_LOCK(templates);
    command = templ->command ? g_strdup(templ->command) : NULL;
    G_UNLOCK(templates);
    if(command)
    {
        app = g_app_info_create_from_commandline(command, NULL, G_APP_INFO_CREATE_NONE, error);
        g_free(command);
    }
    else
    {
        app = g_app_info_get_default_for_type(fm_mime_type_get_type(templ->mime_type), FALSE);
        if(!app && error)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("No default application is set for MIME type %s"),
                        fm_mime_type_get_type(templ->mime_type));
    }
    if(!app)
        return FALSE;
    G_LOCK(templates);
    tfile = NULL;
    if(templ->template_file)
    {
        command = fm_path_to_str(templ->template_file);
        tfile = g_file_new_for_path(command);
        g_free(command);
    }
    G_UNLOCK(templates);
    /* FIXME: it may block */
    if(!g_file_copy(tfile, path, G_FILE_COPY_TARGET_DEFAULT_PERMS, NULL, NULL,
                    NULL, error))
    {
        if((*error)->domain != G_IO_ERROR || (*error)->code != G_IO_ERROR_NOT_FOUND)
        {
            /* we ran into problems, application will run into them too
               the most probably, so don't try to launch it then */
            g_object_unref(app);
            g_object_unref(tfile);
            return FALSE;
        }
        /* template file not found, it's normal */
        g_clear_error(error);
    }
    g_object_unref(tfile);
    list = g_list_prepend(NULL, path);
    ret = g_app_info_launch(app, list, NULL, error);
    g_list_free(list);
    g_object_unref(app);
    return ret;
}
