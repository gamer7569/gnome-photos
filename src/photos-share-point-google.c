/*
 * Photos - access, organize and share your photos on GNOME
 * Copyright © 2016 Umang Jain
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


#include "config.h"

#include <gio/gio.h>
#include <gdata/gdata.h>
#include <glib/gi18n.h>
#include <gexiv2/gexiv2.h>

#include "photos-base-item.h"
#include "photos-filterable.h"
#include "photos-query.h"
#include "photos-query-builder.h"
#include "photos-search-context.h"
#include "photos-share-point-google.h"
#include "photos-source.h"
#include "photos-tracker-queue.h"
#include "photos-utils.h"


struct _PhotosSharePointGoogle
{
  PhotosSharePointOnline parent_instance;
  PhotosTrackerQueue *queue;
  GDataGoaAuthorizer *authorizer;
  GDataPicasaWebService *service;
  GError *queue_error;
};


G_DEFINE_TYPE_WITH_CODE (PhotosSharePointGoogle, photos_share_point_google, PHOTOS_TYPE_SHARE_POINT_ONLINE,
                         photos_utils_ensure_extension_points ();
                         g_io_extension_point_implement (PHOTOS_SHARE_POINT_ONLINE_EXTENSION_POINT_NAME,
                                                         g_define_type_id,
                                                         "google",
                                                         0));


typedef struct _PhotosSharePointGoogleShareData PhotosSharePointGoogleShareData;

struct _PhotosSharePointGoogleShareData
{
  GDataUploadStream *stream;
  PhotosBaseItem *item;
};


static PhotosSharePointGoogleShareData *
photos_share_point_google_share_data_new (PhotosBaseItem *item)
{
  PhotosSharePointGoogleShareData *data;

  data = g_slice_new0 (PhotosSharePointGoogleShareData);
  data->item = g_object_ref (item);

  return data;
}


static void
photos_share_point_google_share_data_free (PhotosSharePointGoogleShareData *data)
{
  g_clear_object (&data->stream);
  g_clear_object (&data->item);
  g_slice_free (PhotosSharePointGoogleShareData, data);
}


static gchar *
photos_share_point_google_parse_error (PhotosSharePoint *self, GError *error)
{
  gchar *msg;

  if (g_error_matches (error, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_AUTHENTICATION_REQUIRED))
    msg = g_strdup (_("Failed to upload photo: Service not authorized"));
  else
    msg = g_strdup (_("Failed to upload photo"));

  return msg;
}


static void
photos_share_point_google_set_metadata (PhotosBaseItem *item, const gchar *title, const gchar *id, gpointer user_data)
{
  GExiv2Metadata *meta;
  GFile *file;
  GTask *task;
  const gchar *uri;
  gchar *identifier;
  gchar *path;
  GError *error = NULL;

  meta = gexiv2_metadata_new ();
  task = G_TASK (user_data);

  /*Can we do something better to get path ?*/
  uri = photos_base_item_get_uri (item);
  file = g_file_new_for_uri (uri);
  path = g_file_get_path (file);
  gexiv2_metadata_open_path (meta, path, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
    }
  else
    {
      if (gexiv2_metadata_has_tag (meta, "Xmp.xmp.gnome-photos.google.title"))
	gexiv2_metadata_clear_tag (meta, "Xmp.xmp.gnome-photos.google.title");

      if (gexiv2_metadata_has_tag (meta, "Xmp.xmp.gnome-photos.google.id"))
	gexiv2_metadata_clear_tag (meta, "Xmp.xmp.gnome-photos.google.id");

      gexiv2_metadata_set_tag_string (meta, "Xmp.xmp.gnome-photos.google.title", title);

      identifier = g_strconcat ("google:picasaweb:", id, NULL);
      gexiv2_metadata_set_tag_string (meta, "Xmp.xmp.gnome-photos.google.id", identifier);
      gexiv2_metadata_save_file (meta, path, &error);

      if (error)
	g_task_return_error (task, error);
      }

  gexiv2_metadata_free (meta);
  g_free (identifier);
  g_object_unref (file);
  g_object_unref (task);
  g_free (path);
}


static void
photos_share_point_google_relate_objects (PhotosSharePointGoogle *self, GCancellable *cancellable, const gchar *obj1, const gchar *obj2)
{
  PhotosSearchContextState *state;
  PhotosQuery *query;
  GApplication *app;

  app = g_application_get_default ();
  state = photos_search_context_get_state (PHOTOS_SEARCH_CONTEXT (app));

  query = photos_query_builder_relate_objects (state, obj1, obj2);
  printf("Related query 1 :%s \n", query->sparql);
  photos_tracker_queue_update (self->queue,
                               query->sparql,
                               cancellable,
                               NULL,
                               NULL,
                               NULL);

  query = photos_query_builder_relate_objects (state, obj2, obj1);
  printf("Related query 2 :%s \n", query->sparql);
  photos_tracker_queue_update (self->queue,
                               query->sparql,
                               cancellable,
                               NULL,
                               NULL,
                               NULL);
  photos_query_free (query);
}


static void
photos_google_share_point_tracker_entry_created (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhotosSharePointGoogle *self;
  PhotosSharePointGoogleShareData *data;
  TrackerSparqlConnection *connection = TRACKER_SPARQL_CONNECTION (source_object);
  GCancellable *cancellable;
  GError *error;
  GTask *task = G_TASK (user_data);
  GVariant *variant;
  GVariant *child;
  const gchar *item_urn;
  const gchar *remote_urn;

  self = PHOTOS_SHARE_POINT_GOOGLE (g_task_get_source_object (task));
  cancellable = g_task_get_cancellable (task);
  data = (PhotosSharePointGoogleShareData *) g_task_get_task_data (task);

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

  /* We do, remote urn relatedTo base urn mapping here.
   * In the model, we see if any urn has set its related to property then,
   * it is definitely a remote urn and it does not show up in overview.
   */

  photos_share_point_google_relate_objects (self, cancellable, remote_urn, item_urn);
}


static void
photos_share_point_google_create_tracker_entry (PhotosSharePointGoogle *self,
                                                GTask *task,
                                                GDataPicasaWebFile *file_entry,
                                                GError **error)
{
  PhotosSearchContextState *state;
  PhotosSharePointGoogleShareData *data;
  PhotosQuery *query;
  GApplication *app;
  GCancellable *cancellable;
  const gchar *id;
  const gchar *title;

  app = g_application_get_default ();
  state = photos_search_context_get_state (PHOTOS_SEARCH_CONTEXT (app));

  cancellable = g_task_get_cancellable (task);
  data = (PhotosSharePointGoogleShareData *) g_task_get_task_data (task);

  id = gdata_entry_get_id (GDATA_ENTRY (file_entry));
  title = gdata_entry_get_title (GDATA_ENTRY (file_entry));

  query = photos_query_builder_insert_remote_object (state, title, id);
  photos_tracker_queue_update_blank (self->queue,
                                     query->sparql,
                                     cancellable,
                                     photos_google_share_point_tracker_entry_created,
                                     g_object_ref (task),
                                     g_object_unref);
  photos_query_free (query);

  /*Metadata Embed logic flows from here*/
  data->item = PHOTOS_BASE_ITEM (g_object_ref (data->item));
  photos_share_point_google_set_metadata (data->item, title, id, g_object_ref (task));
}


static void
photos_share_point_google_share_save_to_stream (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhotosSharePointGoogle *self;
  GError *error;
  GTask *task = G_TASK (user_data);
  GDataPicasaWebFile *file_entry = NULL;
  PhotosBaseItem *item = PHOTOS_BASE_ITEM (source_object);
  PhotosSharePointGoogleShareData *data;

  self = PHOTOS_SHARE_POINT_GOOGLE (g_task_get_source_object (task));
  data = (PhotosSharePointGoogleShareData *) g_task_get_task_data (task);

  error = NULL;
  if (!photos_base_item_save_to_stream_finish (item, res, &error))
    {
      g_task_return_error (task, error);
      goto out;
    }

  error = NULL;
  file_entry = gdata_picasaweb_service_finish_file_upload (GDATA_PICASAWEB_SERVICE (self->service),
                                                           data->stream,
                                                           &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      goto out;
    }

    /* adding remote object logic flows from here*/
  error = NULL;
  photos_share_point_google_create_tracker_entry (self, task, file_entry, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      goto out;
    }

  g_task_return_boolean (task, TRUE);

 out:
  g_clear_object (&file_entry);
  g_object_unref (task);
}


static void
photos_share_point_google_share_refresh_authorization (GObject *source_object,
                                                       GAsyncResult *res,
                                                       gpointer user_data)
{
  PhotosSharePointGoogle *self;
  GCancellable *cancellable;
  GError *error;
  GTask *task = G_TASK (user_data);
  GDataAuthorizer *authorizer = GDATA_AUTHORIZER (source_object);
  GDataPicasaWebFile *file_entry = NULL;
  GDataUploadStream *stream = NULL;
  PhotosSharePointGoogleShareData *data;
  const gchar *filename;
  const gchar *mime_type;
  const gchar *name;

  self = PHOTOS_SHARE_POINT_GOOGLE (g_task_get_source_object (task));
  cancellable = g_task_get_cancellable (task);
  data = (PhotosSharePointGoogleShareData *) g_task_get_task_data (task);

  error = NULL;
  if (!gdata_authorizer_refresh_authorization_finish (authorizer, res, &error))
    {
      g_task_return_error (task, error);
      goto out;
    }

  file_entry = gdata_picasaweb_file_new (NULL);
  name = photos_base_item_get_name_with_fallback (data->item);
  gdata_entry_set_title (GDATA_ENTRY (file_entry), name);

  filename = photos_base_item_get_filename (data->item);
  mime_type = photos_base_item_get_mime_type (data->item);

  error = NULL;
  stream = gdata_picasaweb_service_upload_file (self->service,
                                                NULL,
                                                file_entry,
                                                filename,
                                                mime_type,
                                                cancellable,
                                                &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      goto out;
    }

  g_assert_null (data->stream);
  data->stream = g_object_ref (stream);

  photos_base_item_save_to_stream_async (data->item,
                                         G_OUTPUT_STREAM (stream),
                                         1.0,
                                         cancellable,
                                         photos_share_point_google_share_save_to_stream,
                                         g_object_ref (task));

 out:
  g_clear_object (&file_entry);
  g_clear_object (&stream);
  g_object_unref (task);
}


static void
photos_share_point_google_share_async (PhotosSharePoint *share_point,
                                       PhotosBaseItem *item,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
  PhotosSharePointGoogle *self = PHOTOS_SHARE_POINT_GOOGLE (share_point);
  GTask *task;
  PhotosSharePointGoogleShareData *data;

  data = photos_share_point_google_share_data_new (item);

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, photos_share_point_google_share_async);
  g_task_set_task_data (task, data, (GDestroyNotify) photos_share_point_google_share_data_free);

  gdata_authorizer_refresh_authorization_async (GDATA_AUTHORIZER (self->authorizer),
                                                cancellable,
                                                photos_share_point_google_share_refresh_authorization,
                                                g_object_ref (task));

  g_object_unref (task);
}


static gboolean
photos_share_point_google_share_finish (PhotosSharePoint *share_point, GAsyncResult *res, GError **error)
{
  PhotosSharePointGoogle *self = PHOTOS_SHARE_POINT_GOOGLE (share_point);
  GTask *task;

  g_return_val_if_fail (g_task_is_valid (res, self), FALSE);
  task = G_TASK (res);

  g_return_val_if_fail (g_task_get_source_tag (task) == photos_share_point_google_share_async, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return g_task_propagate_boolean (task, error);
}


static void
photos_share_point_google_constructed (GObject *object)
{
  PhotosSharePointGoogle *self = PHOTOS_SHARE_POINT_GOOGLE (object);
  PhotosSource *source;
  GoaObject *goa_object;

  G_OBJECT_CLASS (photos_share_point_google_parent_class)->constructed (object);

  source = photos_share_point_online_get_source (PHOTOS_SHARE_POINT_ONLINE (self));
  goa_object = photos_source_get_goa_object (source);
  self->authorizer = gdata_goa_authorizer_new (goa_object);
  self->service = gdata_picasaweb_service_new (GDATA_AUTHORIZER (self->authorizer));
}


static void
photos_share_point_google_dispose (GObject *object)
{
  PhotosSharePointGoogle *self = PHOTOS_SHARE_POINT_GOOGLE (object);

  g_clear_object (&self->authorizer);
  g_clear_object (&self->queue);
  g_clear_object (&self->service);

  G_OBJECT_CLASS (photos_share_point_google_parent_class)->dispose (object);
}


static void
photos_share_point_google_finalize (GObject *object)
{
  PhotosSharePointGoogle *self = PHOTOS_SHARE_POINT_GOOGLE (object);

  g_clear_error (&self->queue_error);

  G_OBJECT_CLASS (photos_share_point_google_parent_class)->dispose (object);
}

static void
photos_share_point_google_init (PhotosSharePointGoogle *self)
{
  self->queue = photos_tracker_queue_dup_singleton (NULL, &self->queue_error);
}


static void
photos_share_point_google_class_init (PhotosSharePointGoogleClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  PhotosSharePointClass *share_point_class = PHOTOS_SHARE_POINT_CLASS (class);

  object_class->constructed = photos_share_point_google_constructed;
  object_class->dispose = photos_share_point_google_dispose;
  share_point_class->parse_error = photos_share_point_google_parse_error;
  object_class->finalize = photos_share_point_google_finalize;
  share_point_class->share_async = photos_share_point_google_share_async;
  share_point_class->share_finish = photos_share_point_google_share_finish;
}
