/*
 * Photos - access, organize and share your photos on GNOME
 * Copyright © 2016 Red Hat, Inc.
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
#include <tracker-sparql.h>

#include "photos-filterable.h"
#include "photos-share-point-online.h"
#include "photos-tracker-queue.h"
#include "photos-query.h"
#include "photos-query-builder.h"
#include "photos-search-context.h"
#include "photos-base-item.h"


struct _PhotosSharePointOnlinePrivate
{
  GIcon *icon;
  PhotosTrackerQueue *queue;
  PhotosSource *source;
  gchar *id;
  gchar *name;
  GError *queue_error;
};

enum
{
  PROP_0,
  PROP_SOURCE,
};

static void photos_filterable_interface_init (PhotosFilterableInterface *iface);


G_DEFINE_ABSTRACT_TYPE_WITH_CODE (PhotosSharePointOnline, photos_share_point_online, PHOTOS_TYPE_SHARE_POINT,
                                  G_ADD_PRIVATE (PhotosSharePointOnline)
                                  G_IMPLEMENT_INTERFACE (PHOTOS_TYPE_FILTERABLE, photos_filterable_interface_init));

typedef struct _PhotosSharePointOnlineData PhotosSharePointOnlineData;

struct _PhotosSharePointOnlineData
{
  const gchar *remote_title;
  const gchar *remote_id;
  PhotosBaseItem *item;
};

static PhotosSharePointOnlineData *
photos_share_point_online_data_new (PhotosBaseItem *item, const gchar *title, const gchar *id)
{
  PhotosSharePointOnlineData *data;

  data = g_slice_new0 (PhotosSharePointOnlineData);
  data->item = g_object_ref (item);
  data->remote_id = id;
  data->remote_title = title;

  return data;
}


static void
photos_share_point_online_data_free (PhotosSharePointOnlineData *data)
{
  g_clear_object (&data->item);
  g_slice_free (PhotosSharePointOnlineData, data);
}


static GIcon *
photos_share_point_online_get_icon (PhotosSharePoint *share_point)
{
  PhotosSharePointOnline *self = PHOTOS_SHARE_POINT_ONLINE (share_point);
  PhotosSharePointOnlinePrivate *priv;

  priv = photos_share_point_online_get_instance_private (self);
  return priv->icon;
}


static const gchar *
photos_share_point_online_get_id (PhotosFilterable *filterable)
{
  PhotosSharePointOnline *self = PHOTOS_SHARE_POINT_ONLINE (filterable);
  PhotosSharePointOnlinePrivate *priv;

  priv = photos_share_point_online_get_instance_private (self);
  return priv->id;
}


static const gchar *
photos_share_point_online_get_name (PhotosSharePoint *share_point)
{
  PhotosSharePointOnline *self = PHOTOS_SHARE_POINT_ONLINE (share_point);
  PhotosSharePointOnlinePrivate *priv;

  priv = photos_share_point_online_get_instance_private (self);
  return priv->name;
}


static void
photos_share_point_online_dispose (GObject *object)
{
  PhotosSharePointOnline *self = PHOTOS_SHARE_POINT_ONLINE (object);
  PhotosSharePointOnlinePrivate *priv;

  priv = photos_share_point_online_get_instance_private (self);

  g_clear_object (&priv->icon);
  g_clear_object (&priv->source);

  G_OBJECT_CLASS (photos_share_point_online_parent_class)->dispose (object);
}


static void
photos_share_point_online_finalize (GObject *object)
{
  PhotosSharePointOnline *self = PHOTOS_SHARE_POINT_ONLINE (object);
  PhotosSharePointOnlinePrivate *priv;

  priv = photos_share_point_online_get_instance_private (self);

  g_free (priv->id);
  g_free (priv->name);

  G_OBJECT_CLASS (photos_share_point_online_parent_class)->finalize (object);
}


static void
photos_share_point_online_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  PhotosSharePointOnline *self = PHOTOS_SHARE_POINT_ONLINE (object);
  PhotosSharePointOnlinePrivate *priv;

  priv = photos_share_point_online_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_SOURCE:
      g_value_set_object (value, priv->source);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}


static void
photos_share_point_online_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  PhotosSharePointOnline *self = PHOTOS_SHARE_POINT_ONLINE (object);
  PhotosSharePointOnlinePrivate *priv;

  priv = photos_share_point_online_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_SOURCE:
      {
        GIcon *icon;
        const gchar *id;
        const gchar *name;

        priv->source = PHOTOS_SOURCE (g_value_dup_object (value));
        if (priv->source == NULL)
          break;

        id = photos_filterable_get_id (PHOTOS_FILTERABLE (priv->source));
        priv->id = g_strdup (id);

        icon = photos_source_get_icon (priv->source);
        priv->icon = g_object_ref (icon);

        name = photos_source_get_name (priv->source);
        priv->name = g_strdup (name);
        break;
      }

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}


static void
photos_share_point_online_init (PhotosSharePointOnline *self)
{
  PhotosSharePointOnlinePrivate *priv = photos_share_point_online_get_instance_private (self);
  priv->queue = photos_tracker_queue_dup_singleton (NULL, &priv->queue_error);
}


static void
photos_share_point_online_class_init (PhotosSharePointOnlineClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  PhotosSharePointClass *share_point_class = PHOTOS_SHARE_POINT_CLASS (class);

  object_class->dispose = photos_share_point_online_dispose;
  object_class->finalize = photos_share_point_online_finalize;
  object_class->get_property = photos_share_point_online_get_property;
  object_class->set_property = photos_share_point_online_set_property;
  share_point_class->get_icon = photos_share_point_online_get_icon;
  share_point_class->get_name = photos_share_point_online_get_name;

  g_object_class_install_property (object_class,
                                   PROP_SOURCE,
                                   g_param_spec_object ("source",
                                                        "PhotosSource instance",
                                                        "The online source corresponding to this share point",
                                                        PHOTOS_TYPE_SOURCE,
                                                        G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE));
}


static void
photos_filterable_interface_init (PhotosFilterableInterface *iface)
{
  iface->get_id = photos_share_point_online_get_id;
}


PhotosSource *
photos_share_point_online_get_source (PhotosSharePointOnline *self)
{
  PhotosSharePointOnlinePrivate *priv;

  priv = photos_share_point_online_get_instance_private (self);
  return priv->source;
}


static void
photos_share_point_online_relate_objects (PhotosSharePointOnline *self, GCancellable *cancellable, const gchar *obj1, const gchar *obj2)
{
  PhotosSharePointOnlinePrivate *priv = photos_share_point_online_get_instance_private (self);
  PhotosSearchContextState *state;
  PhotosQuery *query;
  GApplication *app;

  app = g_application_get_default ();
  state = photos_search_context_get_state (PHOTOS_SEARCH_CONTEXT (app));

  query = photos_query_builder_relate_objects (state, obj1, obj2);
  printf("Related query 1 :%s \n", query->sparql);
  photos_tracker_queue_update (priv->queue,
                               query->sparql,
                               cancellable,
                               NULL,
                               NULL,
                               NULL);

  query = photos_query_builder_relate_objects (state, obj2, obj1);
  printf("Related query 2 :%s \n", query->sparql);
  photos_tracker_queue_update (priv->queue,
                               query->sparql,
                               cancellable,
                               NULL,
                               NULL,
                               NULL);
  photos_query_free (query);
}


static void
photos_share_point_online_tracker_entry_created (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhotosSharePointOnline *self;
  TrackerSparqlConnection *connection = TRACKER_SPARQL_CONNECTION (source_object);
  PhotosSharePointOnlineData *data;
  GCancellable *cancellable;
  GTask *task = G_TASK (user_data);
  GError *error;
  GVariant *variant;
  GVariant *child;
  const gchar *item_urn;
  const gchar *remote_urn;

  self = PHOTOS_SHARE_POINT_ONLINE (g_task_get_source_object (task));
  data = (PhotosSharePointOnlineData *) (g_task_get_task_data (task));
  cancellable = g_task_get_cancellable (task);

  error = NULL;
  variant = tracker_sparql_connection_update_blank_finish (connection, res, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  child = g_variant_get_child_value (variant, 0); /* variant is now aa{ss} */
  g_variant_unref (variant);
  variant = child;

  child = g_variant_get_child_value (variant, 0); /* variant is now s{ss} */
  g_variant_unref (variant);
  variant = child;

  child = g_variant_get_child_value (variant, 0); /* variant is now {ss} */
  g_variant_unref (variant);
  variant = child;

  child = g_variant_get_child_value (variant, 1);
  remote_urn = g_variant_dup_string (child, NULL);/*urn of inserted object*/
  g_variant_unref (child);
  printf("remote image urn: %s\n", remote_urn);

  item_urn = photos_filterable_get_id (PHOTOS_FILTERABLE (data->item));
  printf("local image urn: %s\n", item_urn);

  photos_share_point_online_relate_objects (self, cancellable, remote_urn, item_urn);
}

static void
photos_share_point_online_create_entry_in_thread_func (GTask *task,
						       gpointer source_object,
						       gpointer task_data,
						       GCancellable *cancellable)
{
  PhotosSharePointOnline *self = PHOTOS_SHARE_POINT_ONLINE (source_object);
  PhotosSharePointOnlinePrivate *priv;
  PhotosSearchContextState *state;
  PhotosSharePointOnlineData *data;
  GApplication *app;
  PhotosQuery *query;
	printf("In thread func \n");
  app = g_application_get_default ();
  state = photos_search_context_get_state (PHOTOS_SEARCH_CONTEXT (app));
  data = (PhotosSharePointOnlineData *) (task_data);

  priv = photos_share_point_online_get_instance_private (self);
  query = photos_query_builder_insert_remote_object (state, data->remote_title, data->remote_id);
  photos_tracker_queue_update_blank (priv->queue,
                                     query->sparql,
                                     cancellable,
                                     photos_share_point_online_tracker_entry_created,
                                     g_object_ref (task),
                                     g_object_unref);
  photos_query_free (query);


  /*Metadata Embed logic flows from here*/
 // data->item = PHOTOS_BASE_ITEM (g_object_ref (data->item));
  //photos_share_point_google_set_metadata (data->item, title, id, g_object_ref (task));
}


gboolean
photos_share_point_online_tracker_entry_finish (PhotosSharePointOnline *self, GAsyncResult *res, GError **error)
{
  GTask *task = G_TASK (res);

  g_return_val_if_fail (g_task_is_valid (res, self), FALSE);
  g_return_val_if_fail (g_task_get_source_tag (task) == photos_share_point_online_tracker_entry_async, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return g_task_propagate_boolean (task, error);
}


void
photos_share_point_online_tracker_entry_async (PhotosSharePointOnline *self,
					       GCancellable *cancellable,
					       const gchar *title,
					       const gchar *id,
					       GAsyncReadyCallback callback,
					       gpointer user_data)
{
  GTask *task;
  PhotosBaseItem *item;
  PhotosSharePointOnlineData *data;

  item = PHOTOS_BASE_ITEM (user_data);

  data = photos_share_point_online_data_new (item, title, id);

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, photos_share_point_online_tracker_entry_async);
  g_task_set_task_data (task, data, (GDestroyNotify) photos_share_point_online_data_free);

  g_task_run_in_thread (task, photos_share_point_online_create_entry_in_thread_func);
  g_object_unref (task);
}

