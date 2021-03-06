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

#if !defined(_POLKIT_BACKEND_COMPILATION)                                     \
    && !defined(_POLKIT_BACKEND_INSIDE_POLKIT_BACKEND_H)
#error                                                                        \
    "Only <polkitbackend/polkitbackend.h> can be included directly, this file may disappear or change contents."
#endif

#ifndef __POLKIT_BACKEND_KEYFILE_AUTHORITY_H
#define __POLKIT_BACKEND_KEYFILE_AUTHORITY_H

#include <glib-object.h>
#include <polkitbackend/polkitbackendinteractiveauthority.h>
#include <polkitbackend/polkitbackendtypes.h>

G_BEGIN_DECLS

#define POLKIT_BACKEND_TYPE_KEYFILE_AUTHORITY                                 \
  (polkit_backend_keyfile_authority_get_type ())
#define POLKIT_BACKEND_KEYFILE_AUTHORITY(o)                                   \
  (G_TYPE_CHECK_INSTANCE_CAST ((o), POLKIT_BACKEND_TYPE_KEYFILE_AUTHORITY,    \
                               PolkitBackendKeyfileAuthority))
#define POLKIT_BACKEND_KEYFILE_AUTHORITY_CLASS(k)                             \
  (G_TYPE_CHECK_CLASS_CAST ((k), POLKIT_BACKEND_TYPE_KEYFILE_AUTHORITY,       \
                            PolkitBackendKeyfileAuthorityClass))
#define POLKIT_BACKEND_KEYFILE_AUTHORITY_GET_CLASS(o)                         \
  (G_TYPE_INSTANCE_GET_CLASS ((o), POLKIT_BACKEND_TYPE_KEYFILE_AUTHORITY,     \
                              PolkitBackendKeyfileAuthorityClass))
#define POLKIT_BACKEND_IS_KEYFILE_AUTHORITY(o)                                \
  (G_TYPE_CHECK_INSTANCE_TYPE ((o), POLKIT_BACKEND_TYPE_KEYFILE_AUTHORITY))
#define POLKIT_BACKEND_IS_KEYFILE_AUTHORITY_CLASS(k)                          \
  (G_TYPE_CHECK_CLASS_TYPE ((k), POLKIT_BACKEND_TYPE_KEYFILE_AUTHORITY))

typedef struct _PolkitBackendKeyfileAuthorityClass
    PolkitBackendKeyfileAuthorityClass;
typedef struct _PolkitBackendKeyfileAuthorityPrivate
    PolkitBackendKeyfileAuthorityPrivate;

/**
 * PolkitBackendKeyfileAuthority:
 *
 * The #PolkitBackendKeyfileAuthority struct should not be accessed directly.
 */
struct _PolkitBackendKeyfileAuthority
{
  /*< private >*/
  PolkitBackendInteractiveAuthority parent_instance;
  PolkitBackendKeyfileAuthorityPrivate *priv;
};

/**
 * PolkitBackendKeyfileAuthorityClass:
 * @parent_class: The parent class.
 *
 * Class structure for #PolkitBackendKeyfileAuthority.
 */
struct _PolkitBackendKeyfileAuthorityClass
{
  /*< public >*/
  PolkitBackendInteractiveAuthorityClass parent_class;
};

GType polkit_backend_keyfile_authority_get_type (void) G_GNUC_CONST;

G_END_DECLS

#endif /* __POLKIT_BACKEND_KEYFILE_AUTHORITY_H */
