/*
 * Photos - access, organize and share your photos on GNOME
 * Copyright © 2013 – 2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/* Based on code from:
 *   + Documents
 */


#include "config.h"

#include <gio/gio.h>

#include "photos-item-manager.h"
#include "photos-query-builder.h"
#include "photos-offset-collections-controller.h"
#include "photos-search-context.h"


struct _PhotosOffsetCollectionsController
{
  PhotosOffsetController parent_instance;
  PhotosBaseManager *item_mngr;
};

struct _PhotosOffsetCollectionsControllerClass
{
  PhotosOffsetControllerClass parent_class;
};


G_DEFINE_TYPE (PhotosOffsetCollectionsController,
               photos_offset_collections_controller,
               PHOTOS_TYPE_OFFSET_CONTROLLER);


static PhotosQuery *
photos_offset_collections_controller_get_query (PhotosOffsetController *offset_cntrlr)
{
  PhotosOffsetCollectionsController *self = PHOTOS_OFFSET_COLLECTIONS_CONTROLLER (offset_cntrlr);
  GApplication *app;
  PhotosBaseItem *collection;
  PhotosSearchContextState *state;
  gint flags;

  g_return_val_if_fail (self->item_mngr != NULL, NULL);

  collection = photos_item_manager_get_active_collection (PHOTOS_ITEM_MANAGER (self->item_mngr));
  if (collection != NULL)
    flags = PHOTOS_QUERY_FLAGS_NONE;
  else
    flags = PHOTOS_QUERY_FLAGS_COLLECTIONS;

  app = g_application_get_default ();
  state = photos_search_context_get_state (PHOTOS_SEARCH_CONTEXT (app));

  return photos_query_builder_count_query (state, flags);
}


static GObject *
photos_offset_collections_controller_constructor (GType type,
                                                  guint n_construct_params,
                                                  GObjectConstructParam *construct_params)
{
  static GObject *self = NULL;

  if (self == NULL)
    {
      self = G_OBJECT_CLASS (photos_offset_collections_controller_parent_class)->constructor (type,
                                                                                              n_construct_params,
                                                                                              construct_params);
      g_object_add_weak_pointer (self, (gpointer) &self);
      return self;
    }

  return g_object_ref (self);
}


static void
photos_offset_collections_controller_finalize (GObject *object)
{
  PhotosOffsetCollectionsController *self = PHOTOS_OFFSET_COLLECTIONS_CONTROLLER (object);

  if (self->item_mngr != NULL)
    g_object_remove_weak_pointer (G_OBJECT (self->item_mngr), (gpointer *) &self->item_mngr);

  G_OBJECT_CLASS (photos_offset_collections_controller_parent_class)->finalize (object);
}


static void
photos_offset_collections_controller_init (PhotosOffsetCollectionsController *self)
{
  GApplication *app;
  PhotosSearchContextState *state;

  app = g_application_get_default ();
  state = photos_search_context_get_state (PHOTOS_SEARCH_CONTEXT (app));

  self->item_mngr = state->item_mngr;
  g_object_add_weak_pointer (G_OBJECT (self->item_mngr), (gpointer *) &self->item_mngr);
}


static void
photos_offset_collections_controller_class_init (PhotosOffsetCollectionsControllerClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  PhotosOffsetControllerClass *offset_controller_class = PHOTOS_OFFSET_CONTROLLER_CLASS (class);

  object_class->constructor = photos_offset_collections_controller_constructor;
  object_class->finalize = photos_offset_collections_controller_finalize;
  offset_controller_class->get_query = photos_offset_collections_controller_get_query;
}


PhotosOffsetController *
photos_offset_collections_controller_dup_singleton (void)
{
  return g_object_new (PHOTOS_TYPE_OFFSET_COLLECTIONS_CONTROLLER, NULL);
}
