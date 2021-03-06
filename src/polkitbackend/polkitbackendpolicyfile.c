/*
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
 * Author: Ikey Doherty <ikey@solus-project.com>
 */

#include "config.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "polkitbackendpolicyfile.h"

/**
 * Permanently correct section for defining:
 * Rules=
 * AdminRules=
 */
#define POLICY_SECTION "Policy"

/**
 * Action ID to match all possible IDs
 * Useful for "SubjectUser=" matches
 */
#define POLICY_MATCH_ALL "*"

static gboolean policy_file_load_rules (GKeyFile *keyfile,
                                        const gchar *section, Policy **target);
static PolkitImplicitAuthorization policy_string_to_result (const gchar *inp);

PolicyFile *
policy_file_new_from_path (const char *path, GError **err)
{
  g_autoptr (GKeyFile) keyf = NULL;
  PolicyFile *ret = NULL;
  gboolean has_rules = FALSE;

  keyf = g_key_file_new ();
  if (!g_key_file_load_from_file (keyf, path, G_KEY_FILE_NONE, err))
    {
      return NULL;
    }

  ret = g_new0 (PolicyFile, 1);

  if (g_key_file_has_key (keyf, POLICY_SECTION, "Rules", NULL))
    {
      if (!policy_file_load_rules (keyf, "Rules", &ret->rules.normal))
        {
          policy_file_free (ret);
          return NULL;
        }
      has_rules = TRUE;
    }

  if (g_key_file_has_key (keyf, POLICY_SECTION, "AdminRules", NULL))
    {

      if (!policy_file_load_rules (keyf, "AdminRules", &ret->rules.admin))
        {
          policy_file_free (ret);
          return NULL;
        }
      has_rules = TRUE;
    }

  /* No sense in loading empty rules */
  if (!has_rules)
    {
      policy_file_free (ret);
      return NULL;
    }

  return ret;
}

static void
policy_free (Policy *policy)
{
  if (!policy)
    {
      return;
    }
  g_clear_pointer (&policy->next, policy_free);
  g_clear_pointer (&policy->id, g_free);
  g_clear_pointer (&policy->actions, g_strfreev);
  g_clear_pointer (&policy->action_contains, g_strfreev);
  g_clear_pointer (&policy->unix_groups, g_strfreev);
  g_clear_pointer (&policy->unix_names, g_strfreev);
  g_clear_pointer (&policy->net_groups, g_strfreev);
  g_free (policy);
}

void
policy_file_free (PolicyFile *file)
{
  if (!file)
    {
      return;
    }
  g_clear_pointer (&file->next, policy_file_free);
  g_clear_pointer (&file->rules.admin, policy_free);
  g_clear_pointer (&file->rules.normal, policy_free);
  g_free (file);
}

/**
 * Attempt to load a policy from the given section id and keyfile
 */
static Policy *
policy_load (GKeyFile *file, const gchar *section_id)
{
  Policy *policy = NULL;
  gsize n_segments = 0;
  g_autoptr (GError) err = NULL;

  if (!g_key_file_has_group (file, section_id))
    {
      g_warning ("Missing rule: '%s'\n", section_id);
      return NULL;
    }

  policy = g_new0 (Policy, 1);
  policy->id = g_strdup (section_id);

  /* Load Action IDs */
  if (g_key_file_has_key (file, section_id, "Actions", NULL))
    {
      policy->actions = g_key_file_get_string_list (
          file, section_id, "Actions", &n_segments, &err);
      if (err)
        {
          goto handle_err;
        }
      policy->n_actions = n_segments;
      policy->constraints |= PF_CONSTRAINT_ACTIONS;
      n_segments = 0;
    }

  /* Load ActionContains IDs */
  if (g_key_file_has_key (file, section_id, "ActionContains", NULL))
    {
      policy->action_contains = g_key_file_get_string_list (
          file, section_id, "ActionContains", &n_segments, &err);
      if (err)
        {
          goto handle_err;
        }
      policy->n_action_contains = n_segments;
      policy->constraints |= PF_CONSTRAINT_ACTION_CONTAINS;
      n_segments = 0;
    }

  /* Are specific unix groups needed? */
  if (g_key_file_has_key (file, section_id, "InUnixGroups", NULL))
    {
      policy->unix_groups = g_key_file_get_string_list (
          file, section_id, "InUnixGroups", &n_segments, &err);
      if (err)
        {
          goto handle_err;
        }
      policy->n_unix_groups = n_segments;
      policy->constraints |= PF_CONSTRAINT_UNIX_GROUPS;
      n_segments = 0;
    }

  /* Are specific net groups needed? */
  if (g_key_file_has_key (file, section_id, "InNetGroups", NULL))
    {
      policy->net_groups = g_key_file_get_string_list (
          file, section_id, "InNetGroups", &n_segments, &err);
      if (err)
        {
          goto handle_err;
        }
      policy->n_net_groups = n_segments;
      policy->constraints |= PF_CONSTRAINT_NET_GROUPS;
      n_segments = 0;
    }

  /* Find out the response type */
  if (g_key_file_has_key (file, section_id, "Result", NULL))
    {
      g_autofree gchar *result
          = g_key_file_get_string (file, section_id, "Result", &err);
      if (err)
        {
          goto handle_err;
        }
      policy->response = policy_string_to_result (g_strstrip (result));
      if (policy->response == POLKIT_IMPLICIT_AUTHORIZATION_UNKNOWN)
        {
          g_warning ("Invalid 'Result': '%s'\n", result);
          goto handle_err;
        }
      policy->constraints |= PF_CONSTRAINT_RESULT;
    }

  /* Find out the inverse response type */
  if (g_key_file_has_key (file, section_id, "ResultInverse", NULL))
    {
      g_autofree gchar *result
          = g_key_file_get_string (file, section_id, "ResultInverse", &err);
      if (err)
        {
          goto handle_err;
        }
      policy->response_inverse = policy_string_to_result (g_strstrip (result));
      if (policy->response == POLKIT_IMPLICIT_AUTHORIZATION_UNKNOWN)
        {
          g_warning ("Invalid 'ResultInverse': '%s'\n", result);
          goto handle_err;
        }
      policy->constraints |= PF_CONSTRAINT_RESULT_INVERSE;
    }

  /* Match unix usernames */
  if (g_key_file_has_key (file, section_id, "InUserNames", NULL))
    {
      policy->unix_names = g_key_file_get_string_list (
          file, section_id, "InUserNames", &n_segments, &err);
      if (err)
        {
          goto handle_err;
        }
      policy->n_unix_names = n_segments;
      policy->constraints |= PF_CONSTRAINT_UNIX_NAMES;
      n_segments = 0;
    }

  /* Match active */
  if (g_key_file_has_key (file, section_id, "SubjectActive", NULL))
    {
      policy->require_active
          = g_key_file_get_boolean (file, section_id, "SubjectActive", &err);
      if (err)
        {
          goto handle_err;
        }
      policy->constraints |= PF_CONSTRAINT_SUBJECT_ACTIVE;
    }

  /* Match local */
  if (g_key_file_has_key (file, section_id, "SubjectLocal", NULL))
    {
      policy->require_local
          = g_key_file_get_boolean (file, section_id, "SubjectLocal", &err);
      if (err)
        {
          goto handle_err;
        }
      policy->constraints |= PF_CONSTRAINT_SUBJECT_LOCAL;
    }

  return policy;

handle_err:

  /* Print error.. */
  g_warning ("policy_load(): error: %s\n", err->message);

  policy_free (policy);
  return NULL;
}

/**
 * Attempt to load rules from the named section within the key file
 */
static gboolean
policy_file_load_rules (GKeyFile *keyfile, const gchar *section,
                        Policy **target)
{
  gchar **sections = NULL;
  gsize n_sections = 0;
  g_autoptr (GError) err = NULL;
  Policy *last = NULL;

  sections = g_key_file_get_string_list (keyfile, POLICY_SECTION, section,
                                         &n_sections, &err);
  if (err)
    {
      g_warning ("Failed to get sections: %s\n", err->message);
      return FALSE;
    }

  /* Attempt to load each rule now */
  for (gsize i = 0; i < n_sections; i++)
    {
      Policy *p = NULL;

      p = policy_load (keyfile, g_strstrip (sections[i]));
      if (!p)
        {
          g_strfreev (sections);
          return FALSE;
        }
      if (last)
        {
          last->next = p;
          last = p;
        }
      else
        {
          last = *target = p;
        }
    }

  g_strfreev (sections);

  return TRUE;
}

/**
 * We wrap the implicit APIs to ensure we do a case insensitive, space-stripped
 * comparison.
 */
static PolkitImplicitAuthorization
policy_string_to_result (const gchar *inp)
{
  g_autofree gchar *comparison = g_ascii_strdown (inp, -1);
  PolkitImplicitAuthorization ret = POLKIT_IMPLICIT_AUTHORIZATION_UNKNOWN;
  if (!polkit_implicit_authorization_from_string (g_strstrip (comparison),
                                                  &ret))
    {
      return POLKIT_IMPLICIT_AUTHORIZATION_UNKNOWN;
    }
  return ret;
}

/**
 * Test the given policy againt the given constraints, and find out if we have
 * some specified action to take.
 * Notice that the highest priority policies are lower in the list and reached
 * first, so they can quite happily block later policies from evaluating
 * completely.
 */
static PolkitImplicitAuthorization
policy_test (Policy *policy, const gchar *action_id, PolicyContext *context)
{
  PolkitImplicitAuthorization response = POLKIT_IMPLICIT_AUTHORIZATION_UNKNOWN;
  gboolean conditions = FALSE;
  gboolean id_matched = FALSE;

  /* Check actions to see if we've been matched */
  if ((policy->constraints & PF_CONSTRAINT_ACTIONS) == PF_CONSTRAINT_ACTIONS)
    {
      for (gsize i = 0; i < policy->n_actions; i++)
        {
          const gchar *action = g_strstrip (policy->actions[i]);
          /* Actions can match either directly or via special '*' character */
          if (g_str_equal (action, action_id)
              || g_str_equal (action, POLICY_MATCH_ALL))
            {
              conditions = TRUE;
              break;
            }
        }
    }

  /* See if the string ID contains some substring patterns */
  if ((policy->constraints & PF_CONSTRAINT_ACTION_CONTAINS)
      == PF_CONSTRAINT_ACTION_CONTAINS)
    {
      for (gsize i = 0; i < policy->n_action_contains; i++)
        {
          const gchar *action = g_strstrip (policy->action_contains[i]);
          if (strstr (action_id, action))
            {
              conditions = TRUE;
              break;
            }
        }
    }

  /* At this point, policy test must've passed as we're explicitly testing
   * action IDs */
  if (!conditions)
    {
      goto unmatched;
    }

  id_matched = TRUE;

  /* Check for SubjectActive */
  if ((policy->constraints & PF_CONSTRAINT_SUBJECT_ACTIVE)
      == PF_CONSTRAINT_SUBJECT_ACTIVE)
    {
      if (context->subject_is_active != policy->require_active)
        {
          goto unmatched;
        }
      conditions = TRUE;
    }

  /* Check for SubjectLocal */
  if ((policy->constraints & PF_CONSTRAINT_SUBJECT_LOCAL)
      == PF_CONSTRAINT_SUBJECT_LOCAL)
    {
      if (context->subject_is_local != policy->require_local)
        {
          conditions = FALSE;
          goto unmatched;
        }
      conditions = TRUE;
    }

  /* Check for Unix Groups */
  if ((policy->constraints & PF_CONSTRAINT_UNIX_GROUPS)
      == PF_CONSTRAINT_UNIX_GROUPS)
    {
      /* Must explicitly re-match here for unix groups now */
      gboolean local_test = FALSE;

      for (gsize i = 0; i < policy->n_unix_groups; i++)
        {
          for (gsize j = 0; j < context->groups->len; j++)
            {
              const gchar *group = NULL;
              const gchar *spec_group = g_strstrip (policy->unix_groups[i]);
              const gchar *test_group = g_ptr_array_index (context->groups, j);

              /* Perform %wheel% substitution here */
              if (g_str_equal (spec_group, POLICY_MATCH_WHEEL))
                {
                  group = POLICY_WHEEL_GROUP;
                }
              else
                {
                  group = spec_group;
                }

              if (g_str_equal (group, test_group))
                {
                  local_test = conditions = TRUE;
                  break;
                }
            }
        }

      if (!local_test)
        {
          conditions = FALSE;
          goto unmatched;
        }
    }

  /* Check for Unix usernames */
  if ((policy->constraints & PF_CONSTRAINT_UNIX_NAMES)
      == PF_CONSTRAINT_UNIX_NAMES)
    {
      /* Must explicitly re-match here for unix names now */
      gboolean local_test = FALSE;

      for (gsize i = 0; i < policy->n_unix_names; i++)
        {
          const gchar *username = g_strstrip (policy->unix_names[i]);
          if (g_str_equal (username, context->username))
            {
              local_test = conditions = TRUE;
              break;
            }
        }
      if (!local_test)
        {
          conditions = FALSE;
          goto unmatched;
        }
    }

  /* We hit our conditions */
  if (conditions
      && (policy->constraints & PF_CONSTRAINT_RESULT) == PF_CONSTRAINT_RESULT)
    {
      response = policy->response;
    }

unmatched:
  /* Must have an actual ID match */
  if (!id_matched)
    {
      if (response == POLKIT_IMPLICIT_AUTHORIZATION_UNKNOWN && policy->next)
        {
          return policy_test (policy->next, action_id, context);
        }
      return response;
    }

  /* Conditions for the ID match were unmet and an inverse response is set */
  if (!conditions
      && (policy->constraints & PF_CONSTRAINT_RESULT_INVERSE)
             == PF_CONSTRAINT_RESULT_INVERSE)
    {
      return policy->response_inverse;
    }

  /* Return or pass along */
  if (response == POLKIT_IMPLICIT_AUTHORIZATION_UNKNOWN && policy->next)
    {
      return policy_test (policy->next, action_id, context);
    }
  return response;
}

PolkitImplicitAuthorization
policy_file_test (PolicyFile *file, const gchar *action_id,
                  PolicyContext *context)
{

  PolkitImplicitAuthorization response = POLKIT_IMPLICIT_AUTHORIZATION_UNKNOWN;

  /* Traverse our policies and see if we find a match of some description */
  if (file->rules.normal)
    {
      response = policy_test (file->rules.normal, action_id, context);
    }

  /* If we're still unhandled, pass it down the chain */
  if (response == POLKIT_IMPLICIT_AUTHORIZATION_UNKNOWN && file->next)
    {
      return policy_file_test (file->next, action_id, context);
    }

  return response;
}
