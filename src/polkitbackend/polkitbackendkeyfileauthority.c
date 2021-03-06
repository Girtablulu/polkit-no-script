/*
 * Copyright (C) 2008-2012 Red Hat, Inc.
 * Copyright (C) 2017 Ikey Doherty
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Authors:
 *    David Zeuthen <davidz@redhat.com>
 *    Ikey Doherty <ikey@solus-project.com>
 */

#include "config.h"
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <sys/wait.h>
#ifdef HAVE_NETGROUP_H
#include <netgroup.h>
#else
#include <netdb.h>
#endif
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

#include "polkitbackendkeyfileauthority.h"
#include "polkitbackendpolicyfile.h"
#include <polkit/polkit.h>

#include <polkit/polkitprivate.h>

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-login.h>
#endif /* HAVE_LIBSYSTEMD */

/**
 * SECTION:polkitbackendkeyfileauthority
 * @title: PolkitBackendKeyfileAuthority
 * @short_description: Keyfile Authority
 * @stability: Unstable
 *
 * An implementation of #PolkitBackendAuthority that reads and
 * "compiles" keyfiles into chained structs, to support interaction
 * with authentication agents (virtue of being based on
 * #PolkitBackendInteractiveAuthority).
 */

/* ----------------------------------------------------------------------------------------------------
 */

struct _PolkitBackendKeyfileAuthorityPrivate
{
  gchar **rules_dirs;
  GFileMonitor *
      *dir_monitors; /* NULL-terminated array of GFileMonitor instances */

  PolicyFile *policy; /* Linked series of policies */
};

static void on_dir_monitor_changed (GFileMonitor *monitor, GFile *file,
                                    GFile *other_file,
                                    GFileMonitorEvent event_type,
                                    gpointer user_data);

/* ----------------------------------------------------------------------------------------------------
 */

enum
{
  PROP_0,
  PROP_RULES_DIRS,
};

/* ----------------------------------------------------------------------------------------------------
 */

static gpointer runaway_killer_thread_func (gpointer user_data);

static GList *polkit_backend_keyfile_authority_get_admin_auth_identities (
    PolkitBackendInteractiveAuthority *authority, PolkitSubject *caller,
    PolkitSubject *subject, PolkitIdentity *user_for_subject,
    gboolean subject_is_local, gboolean subject_is_active,
    const gchar *action_id, PolkitDetails *details);

static PolkitImplicitAuthorization
polkit_backend_keyfile_authority_check_authorization_sync (
    PolkitBackendInteractiveAuthority *authority, PolkitSubject *caller,
    PolkitSubject *subject, PolkitIdentity *user_for_subject,
    gboolean subject_is_local, gboolean subject_is_active,
    const gchar *action_id, PolkitDetails *details,
    PolkitImplicitAuthorization implicit);

G_DEFINE_TYPE (PolkitBackendKeyfileAuthority, polkit_backend_keyfile_authority,
               POLKIT_BACKEND_TYPE_INTERACTIVE_AUTHORITY);

/* ----------------------------------------------------------------------------------------------------
 */

static void
polkit_backend_keyfile_authority_init (
    PolkitBackendKeyfileAuthority *authority)
{
  authority->priv = G_TYPE_INSTANCE_GET_PRIVATE (
      authority, POLKIT_BACKEND_TYPE_KEYFILE_AUTHORITY,
      PolkitBackendKeyfileAuthorityPrivate);
}

static gint
rules_file_name_cmp (const gchar *a, const gchar *b)
{
  gint ret;
  const gchar *a_base;
  const gchar *b_base;

  a_base = strrchr (a, '/');
  b_base = strrchr (b, '/');

  g_assert (a_base != NULL);
  g_assert (b_base != NULL);
  a_base += 1;
  b_base += 1;

  ret = g_strcmp0 (a_base, b_base);
  if (ret == 0)
    {
      /* /etc wins over /usr */
      ret = g_strcmp0 (a, b);
      g_assert (ret != 0);
    }

  return ret;
}

/* authority->priv->cx must be within a request */
static void
load_rules (PolkitBackendKeyfileAuthority *authority)
{
  GList *files = NULL;
  GList *l;
  guint num_files = 0;
  GError *error = NULL;
  guint n;
  PolicyFile *last = NULL;

  files = NULL;

  for (n = 0; authority->priv->rules_dirs != NULL
              && authority->priv->rules_dirs[n] != NULL;
       n++)
    {
      const gchar *dir_name = authority->priv->rules_dirs[n];
      GDir *dir = NULL;

      polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                    "Loading rules from directory %s",
                                    dir_name);

      dir = g_dir_open (dir_name, 0, &error);
      if (dir == NULL)
        {
          polkit_backend_authority_log (
              POLKIT_BACKEND_AUTHORITY (authority),
              "Error opening rules directory: %s (%s, %d)", error->message,
              g_quark_to_string (error->domain), error->code);
          g_clear_error (&error);
        }
      else
        {
          const gchar *name;
          while ((name = g_dir_read_name (dir)) != NULL)
            {
              if (g_str_has_suffix (name, ".keyrules"))
                files = g_list_prepend (
                    files, g_strdup_printf ("%s/%s", dir_name, name));
            }
          g_dir_close (dir);
        }
    }

  files = g_list_sort (files, (GCompareFunc)rules_file_name_cmp);

  for (l = files; l != NULL; l = l->next)
    {
      const gchar *filename = (gchar *)l->data;
      PolicyFile *file = NULL;
      g_autoptr (GError) err = NULL;

      file = policy_file_new_from_path (filename, &err);
      if (!file)
        {
          polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                        "Error compiling rules %s: %s",
                                        filename, err->message);
          continue;
        }

      if (last)
        {
          last->next = file;
          last = file;
        }
      else
        {
          last = authority->priv->policy = file;
        }
      num_files++;
    }

  polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                "Finished loading %d rules", num_files);
  g_list_free_full (files, g_free);
}

static void
reload_rules (PolkitBackendKeyfileAuthority *authority)
{
  /* Remove old rules */
  g_clear_pointer (&authority->priv->policy, policy_file_free);

  load_rules (authority);

  /* Let applications know we have new rules... */
  g_signal_emit_by_name (authority, "changed");
}

static void
on_dir_monitor_changed (GFileMonitor *monitor, GFile *file, GFile *other_file,
                        GFileMonitorEvent event_type, gpointer user_data)
{
  PolkitBackendKeyfileAuthority *authority
      = POLKIT_BACKEND_KEYFILE_AUTHORITY (user_data);

  /* TODO: maybe rate-limit so storms of events are collapsed into one with a
   * 500ms resolution? Because when editing a file with emacs we get 4-8
   * events..
   */

  if (file != NULL)
    {
      gchar *name;

      name = g_file_get_basename (file);

      /* g_print ("event_type=%d file=%p name=%s\n", event_type, file, name);
       */
      if (!g_str_has_prefix (name, ".") && !g_str_has_prefix (name, "#")
          && g_str_has_suffix (name, ".keyrules")
          && (event_type == G_FILE_MONITOR_EVENT_CREATED
              || event_type == G_FILE_MONITOR_EVENT_DELETED
              || event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT))
        {
          polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                        "Reloading rules");
          reload_rules (authority);
        }
      g_free (name);
    }
}

static void
setup_file_monitors (PolkitBackendKeyfileAuthority *authority)
{
  guint n;
  GPtrArray *p;

  p = g_ptr_array_new ();
  for (n = 0; authority->priv->rules_dirs != NULL
              && authority->priv->rules_dirs[n] != NULL;
       n++)
    {
      GFile *file;
      GError *error;
      GFileMonitor *monitor;

      file = g_file_new_for_path (authority->priv->rules_dirs[n]);
      error = NULL;
      monitor
          = g_file_monitor_directory (file, G_FILE_MONITOR_NONE, NULL, &error);
      g_object_unref (file);
      if (monitor == NULL)
        {
          g_warning ("Error monitoring directory %s: %s",
                     authority->priv->rules_dirs[n], error->message);
          g_clear_error (&error);
        }
      else
        {
          g_signal_connect (monitor, "changed",
                            G_CALLBACK (on_dir_monitor_changed), authority);
          g_ptr_array_add (p, monitor);
        }
    }
  g_ptr_array_add (p, NULL);
  authority->priv->dir_monitors = (GFileMonitor **)g_ptr_array_free (p, FALSE);
}

static void
polkit_backend_keyfile_authority_constructed (GObject *object)
{
  PolkitBackendKeyfileAuthority *authority
      = POLKIT_BACKEND_KEYFILE_AUTHORITY (object);

  if (authority->priv->rules_dirs == NULL)
    {
      authority->priv->rules_dirs = g_new0 (gchar *, 3);
      authority->priv->rules_dirs[0]
          = g_strdup (PACKAGE_SYSCONF_DIR "/polkit-1/rules.d");
      authority->priv->rules_dirs[1]
          = g_strdup (PACKAGE_DATA_DIR "/polkit-1/rules.d");
    }

  setup_file_monitors (authority);
  load_rules (authority);

  G_OBJECT_CLASS (polkit_backend_keyfile_authority_parent_class)
      ->constructed (object);
}

static void
polkit_backend_keyfile_authority_finalize (GObject *object)
{
  PolkitBackendKeyfileAuthority *authority
      = POLKIT_BACKEND_KEYFILE_AUTHORITY (object);
  guint n;

  for (n = 0; authority->priv->dir_monitors != NULL
              && authority->priv->dir_monitors[n] != NULL;
       n++)
    {
      GFileMonitor *monitor = authority->priv->dir_monitors[n];
      g_signal_handlers_disconnect_by_func (
          monitor, (gpointer *)G_CALLBACK (on_dir_monitor_changed), authority);
      g_object_unref (monitor);
    }
  g_free (authority->priv->dir_monitors);
  g_strfreev (authority->priv->rules_dirs);

  /* Remove old rules */
  g_clear_pointer (&authority->priv->policy, policy_file_free);

  G_OBJECT_CLASS (polkit_backend_keyfile_authority_parent_class)
      ->finalize (object);
}

static void
polkit_backend_keyfile_authority_set_property (GObject *object,
                                               guint property_id,
                                               const GValue *value,
                                               GParamSpec *pspec)
{
  PolkitBackendKeyfileAuthority *authority
      = POLKIT_BACKEND_KEYFILE_AUTHORITY (object);

  switch (property_id)
    {
    case PROP_RULES_DIRS:
      g_assert (authority->priv->rules_dirs == NULL);
      authority->priv->rules_dirs = (gchar **)g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static const gchar *
polkit_backend_keyfile_authority_get_name (PolkitBackendAuthority *authority)
{
  return "keyfile";
}

static const gchar *
polkit_backend_keyfile_authority_get_version (
    PolkitBackendAuthority *authority)
{
  return PACKAGE_VERSION;
}

static PolkitAuthorityFeatures
polkit_backend_keyfile_authority_get_features (
    PolkitBackendAuthority *authority)
{
  return POLKIT_AUTHORITY_FEATURES_TEMPORARY_AUTHORIZATION;
}

static void
polkit_backend_keyfile_authority_class_init (
    PolkitBackendKeyfileAuthorityClass *klass)
{
  GObjectClass *gobject_class;
  PolkitBackendAuthorityClass *authority_class;
  PolkitBackendInteractiveAuthorityClass *interactive_authority_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = polkit_backend_keyfile_authority_finalize;
  gobject_class->set_property = polkit_backend_keyfile_authority_set_property;
  gobject_class->constructed = polkit_backend_keyfile_authority_constructed;

  authority_class = POLKIT_BACKEND_AUTHORITY_CLASS (klass);
  authority_class->get_name = polkit_backend_keyfile_authority_get_name;
  authority_class->get_version = polkit_backend_keyfile_authority_get_version;
  authority_class->get_features
      = polkit_backend_keyfile_authority_get_features;

  interactive_authority_class
      = POLKIT_BACKEND_INTERACTIVE_AUTHORITY_CLASS (klass);
  interactive_authority_class->get_admin_identities
      = polkit_backend_keyfile_authority_get_admin_auth_identities;
  interactive_authority_class->check_authorization_sync
      = polkit_backend_keyfile_authority_check_authorization_sync;

  g_object_class_install_property (
      gobject_class, PROP_RULES_DIRS,
      g_param_spec_boxed ("rules-dirs", NULL, NULL, G_TYPE_STRV,
                          G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));

  g_type_class_add_private (klass,
                            sizeof (PolkitBackendKeyfileAuthorityPrivate));
}

/* ----------------------------------------------------------------------------------------------------
 */

/**
 * Ensure we prepare the context with extra details and make cookie bits work.
 */
static gboolean
polkit_backend_keyfile_internal_prepare_context (
    PolkitBackendKeyfileAuthority *authority, PolicyContext *context)
{

  pid_t pid;
  uid_t uid;
  struct passwd *passwd;

  if (POLKIT_IS_UNIX_PROCESS (context->subject))
    {
      pid = polkit_unix_process_get_pid (
          POLKIT_UNIX_PROCESS (context->subject));
    }
  else if (POLKIT_IS_SYSTEM_BUS_NAME (context->subject))
    {
      g_autoptr (GError) err = NULL;
      PolkitSubject *process;
      process = polkit_system_bus_name_get_process_sync (
          POLKIT_SYSTEM_BUS_NAME (context->subject), NULL, &err);
      if (process == NULL)
        {
          polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                        "Failed to get process details: %s",
                                        err->message);
          return FALSE;
        }
      pid = polkit_unix_process_get_pid (POLKIT_UNIX_PROCESS (process));
      g_object_unref (process);
    }
  else
    {
      g_assert_not_reached ();
    }

#ifdef HAVE_LIBSYSTEMD
  if (sd_pid_get_session (pid, &context->session_id) == 0)
    {
      if (sd_session_get_seat (context->session_id, &context->seat_id) == 0)
        {
          /* do nothing */
        }
    }
#endif /* HAVE_LIBSYSTEMD */

  g_assert (POLKIT_IS_UNIX_USER (context->user_for_subject));
  uid = polkit_unix_user_get_uid (
      POLKIT_UNIX_USER (context->user_for_subject));

  context->groups = g_ptr_array_new_with_free_func (g_free);

  passwd = getpwuid (uid);
  if (passwd == NULL)
    {
      context->username = g_strdup_printf ("%d", (gint)uid);
      g_warning ("Error looking up info for uid %d: %m", (gint)uid);
    }
  else
    {
      gid_t gids[512];
      int num_gids = 512;

      context->username = g_strdup (passwd->pw_name);

      if (getgrouplist (passwd->pw_name, passwd->pw_gid, gids, &num_gids) < 0)
        {
          g_warning ("Error looking up groups for uid %d: %m", (gint)uid);
        }
      else
        {
          gint n;
          for (n = 0; n < num_gids; n++)
            {
              struct group *group;
              group = getgrgid (gids[n]);
              if (group == NULL)
                {
                  g_ptr_array_add (context->groups,
                                   g_strdup_printf ("%d", (gint)gids[n]));
                }
              else
                {
                  g_ptr_array_add (context->groups, g_strdup (group->gr_name));
                }
            }
        }
    }

  return TRUE;
}

/**
 * Clear up any allocated types on the policycontext
 */
static void
polkit_backend_keyfile_internal_clear_context (PolicyContext *context)
{
  g_clear_pointer (&context->seat_id, free);
  g_clear_pointer (&context->session_id, free);
  g_clear_pointer (&context->groups, g_ptr_array_unref);
  g_clear_pointer (&context->username, g_free);
}

static GList *
polkit_backend_keyfile_internal_build_admin (
    PolkitBackendKeyfileAuthority *authority, GList *ret, gchar **grouping,
    gsize n_items, gchar *id_prefix)
{
  g_autoptr (GError) err = NULL;

  for (gsize i = 0; i < n_items; i++)
    {
      const gchar *match_item = g_strstrip (grouping[i]);
      const gchar *identifier = NULL;

      /* Allow %wheel% substitution here */
      if (g_str_equal (match_item, POLICY_MATCH_WHEEL))
        {
          identifier = POLICY_WHEEL_GROUP;
        }
      else
        {
          identifier = match_item;
        }

      g_autofree gchar *nom = g_strdup_printf ("%s:%s", id_prefix, identifier);
      PolkitIdentity *i = polkit_identity_from_string (nom, &err);
      if (err)
        {
          polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                        "Identity `%s' is not valid, ignoring",
                                        nom);
          g_clear_error (&err);
          continue;
        }
      ret = g_list_prepend (ret, i);
    }
  return ret;
}

static GList *
polkit_backend_keyfile_authority_get_admin_auth_identities (
    PolkitBackendInteractiveAuthority *_authority, PolkitSubject *caller,
    PolkitSubject *subject, PolkitIdentity *user_for_subject,
    gboolean subject_is_local, gboolean subject_is_active,
    const gchar *action_id, PolkitDetails *details)
{
  GList *ret = NULL;
  PolkitBackendKeyfileAuthority *authority
      = POLKIT_BACKEND_KEYFILE_AUTHORITY (_authority);

  /* Organise the context to pass to the policy file for testing */
  PolicyContext context = {
    .subject = subject,
    .user_for_subject = user_for_subject,
    .subject_is_local = subject_is_local,
    .subject_is_active = subject_is_active,
    .details = details,
  };

  if (!polkit_backend_keyfile_internal_prepare_context (authority, &context))
    {
      goto context_fail;
    }

  for (PolicyFile *file = authority->priv->policy; file; file = file->next)
    {
      for (Policy *policy = file->rules.admin; policy; policy = policy->next)
        {
          if ((policy->constraints & PF_CONSTRAINT_UNIX_GROUPS)
              == PF_CONSTRAINT_UNIX_GROUPS)
            {
              ret = polkit_backend_keyfile_internal_build_admin (
                  authority, ret, policy->unix_groups, policy->n_unix_groups,
                  "unix-group");
            }
          if ((policy->constraints & PF_CONSTRAINT_UNIX_NAMES)
              == PF_CONSTRAINT_UNIX_NAMES)
            {
              ret = polkit_backend_keyfile_internal_build_admin (
                  authority, ret, policy->unix_names, policy->n_unix_names,
                  "unix-user");
            }
          if ((policy->constraints & PF_CONSTRAINT_NET_GROUPS)
              == PF_CONSTRAINT_NET_GROUPS)
            {
              ret = polkit_backend_keyfile_internal_build_admin (
                  authority, ret, policy->net_groups, policy->n_net_groups,
                  "unix-netgroup");
            }
        }
    }

  ret = g_list_reverse (ret);

context_fail:
  polkit_backend_keyfile_internal_clear_context (&context);

  if (ret == NULL)
    ret = g_list_prepend (ret, polkit_unix_user_new (0));

  return ret;
}

/* ----------------------------------------------------------------------------------------------------
 */

static PolkitImplicitAuthorization
polkit_backend_keyfile_authority_check_authorization_sync (
    PolkitBackendInteractiveAuthority *_authority, PolkitSubject *caller,
    PolkitSubject *subject, PolkitIdentity *user_for_subject,
    gboolean subject_is_local, gboolean subject_is_active,
    const gchar *action_id, PolkitDetails *details,
    PolkitImplicitAuthorization implicit)
{
  PolkitImplicitAuthorization ret = implicit;
  PolkitBackendKeyfileAuthority *authority
      = POLKIT_BACKEND_KEYFILE_AUTHORITY (_authority);

  /* Organise the context to pass to the policy file for testing */
  PolicyContext context = {
    .subject = subject,
    .user_for_subject = user_for_subject,
    .subject_is_local = subject_is_local,
    .subject_is_active = subject_is_active,
    .details = details,
  };

  if (!polkit_backend_keyfile_internal_prepare_context (authority, &context))
    {
      polkit_backend_keyfile_internal_clear_context (&context);
      return POLKIT_IMPLICIT_AUTHORIZATION_NOT_AUTHORIZED;
    }

  /* Check if our policy files know about this */
  ret = policy_file_test (authority->priv->policy, action_id, &context);

  polkit_backend_keyfile_internal_clear_context (&context);

  /* No rules answered, so we'll just return the implicit auth */
  if (ret == POLKIT_IMPLICIT_AUTHORIZATION_UNKNOWN)
    {
      return implicit;
    }

  /* Return auth per the policies */
  return ret;
}
