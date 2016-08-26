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
  GDataPicasaWebFile *file_entry;
  PhotosBaseItem *item;
};


static PhotosSharePointGoogleShareData *
photos_share_point_google_share_data_new (PhotosBaseItem *item)
{
  PhotosSharePointGoogleShareData *data;

  data = g_slice_new0 (PhotosSharePointGoogleShareData);
  data->item = g_object_ref (item);
  data->file_entry = NULL;

  return data;
}


static void
photos_share_point_google_share_data_free (PhotosSharePointGoogleShareData *data)
{
  g_clear_object (&data->stream);
  g_clear_object (&data->item);
  g_clear_object (&data->file_entry);
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
photos_share_point_google_get_equipment_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhotosSharePointGoogle *self;
  PhotosSharePointGoogleShareData *data;
  PhotosSearchContextState *state;
  PhotosQuery *query;
  TrackerSparqlCursor *cursor = TRACKER_SPARQL_CURSOR (source_object);
  GApplication *app;
  GCancellable *cancellable;
  GError *error;
  GTask *task = G_TASK (user_data);
  const gchar *eqp_urn;
  const gchar *remote_urn;

  self = PHOTOS_SHARE_POINT_GOOGLE (g_task_get_source_object (task));
  data = (PhotosSharePointGoogleShareData *) g_task_get_task_data (task);
  cancellable = g_task_get_cancellable (task);

  error = NULL;
  tracker_sparql_cursor_next_finish (cursor, res, &error);
  if (error != NULL)
    {
      g_warning ("Unable to retrieve equipment from cursor: %s", error->message);
      g_error_free (error);
      goto out;
    }

  eqp_urn = tracker_sparql_cursor_get_string (cursor, 0, NULL);
  remote_urn = g_object_get_data (G_OBJECT (data->file_entry), "remote_urn");

  app = g_application_get_default ();
  state = photos_search_context_get_state (PHOTOS_SEARCH_CONTEXT (app));
  query = photos_query_builder_insert_or_replace (state, remote_urn, "nfo:equipment", eqp_urn);
  photos_tracker_queue_update (self->queue, query->sparql, cancellable, NULL, NULL, NULL);

out:
  g_object_unref (task);
  tracker_sparql_cursor_close (cursor);
}


static void
photos_google_share_point_get_equipment (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  TrackerSparqlConnection *connection = TRACKER_SPARQL_CONNECTION (source_object);
  TrackerSparqlCursor *cursor;
  GTask *task = G_TASK (user_data);
  GError *error;

  error = NULL;
  cursor = tracker_sparql_connection_query_finish (connection, res, &error);
  if (error != NULL)
    {
      g_warning ("Unable to retrieve remote image equipment: %s", error->message);
      g_error_free (error);
      goto out;
    }

  tracker_sparql_cursor_next_async (cursor, NULL, photos_share_point_google_get_equipment_cb, g_object_ref (task));
out:
  g_object_unref (cursor);
}


static void
photos_share_point_google_relate_objects (PhotosSharePointGoogle *self, GCancellable *cancellable, const gchar *domain_urn, const gchar *range_urn)
{
  PhotosSearchContextState *state;
  PhotosQuery *query;
  GApplication *app;

  app = g_application_get_default ();
  state = photos_search_context_get_state (PHOTOS_SEARCH_CONTEXT (app));

  query = photos_query_builder_relate_objects (state, domain_urn, range_urn);
  photos_tracker_queue_update (self->queue,
                               query->sparql,
                               cancellable,
                               NULL,
                               NULL,
                               NULL);

  query = photos_query_builder_relate_objects (state, range_urn, domain_urn);
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
  PhotosSearchContextState *state;
  PhotosSharePointGoogleShareData *data;
  PhotosQuery *query;
  TrackerSparqlConnection *connection = TRACKER_SPARQL_CONNECTION (source_object);
  GApplication *app;
  GCancellable *cancellable;
  GDataLink *alternate;
  GError *error;
  GList *media_contents;
  GTask *task = G_TASK (user_data);
  GTimeVal tv;
  GVariant *variant;
  GVariant *child;
  gint64 timestamp;
  gboolean flash;

  const gchar *credit;
  const gchar *make;
  const gchar *model;
  const gchar *title;
  const gchar *summary;
  const gchar *mime;
  const gchar *item_urn;
  const gchar *remote_urn;
  const gchar *flash_off;
  const gchar *flash_on;
  const gchar *alternate_uri;

  gchar *exposure;
  gchar *focal_length;
  gchar *fstop;
  gchar *iso;
  gchar *width;
  gchar *height;
  gchar *time;

  flash_off = "http://www.tracker-project.org/temp/nmm#flash-off";
  flash_on = "http://www.tracker-project.org/temp/nmm#flash-on";

  self = PHOTOS_SHARE_POINT_GOOGLE (g_task_get_source_object (task));
  cancellable = g_task_get_cancellable (task);
  data = (PhotosSharePointGoogleShareData *) g_task_get_task_data (task);
  item_urn = photos_filterable_get_id (PHOTOS_FILTERABLE (data->item));

  error = NULL;
  variant = tracker_sparql_connection_update_blank_finish (connection, res, &error);
  if (error != NULL)
    {
      g_warning ("Could not insert remote object: %s", error->message);
      g_error_free (error);
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
  remote_urn = g_variant_dup_string (child, NULL);
  g_variant_unref (child);

  g_object_set_data (G_OBJECT (data->file_entry), "remote_urn", (gpointer *) remote_urn);

  app = g_application_get_default ();
  state = photos_search_context_get_state (PHOTOS_SEARCH_CONTEXT (app));

  alternate = gdata_entry_look_up_link (GDATA_ENTRY (data->file_entry), GDATA_LINK_ALTERNATE);
  alternate_uri = gdata_link_get_uri (alternate);
  query = photos_query_builder_insert_or_replace (state, remote_urn, "nie:url", alternate_uri);
  photos_tracker_queue_update (self->queue, query->sparql, cancellable, NULL, NULL, NULL);

  summary = gdata_entry_get_summary (GDATA_ENTRY (data->file_entry));
  query = photos_query_builder_insert_or_replace (state, remote_urn, "nie:description", summary);
  photos_tracker_queue_update (self->queue, query->sparql, cancellable, NULL, NULL, NULL);

  media_contents = gdata_picasaweb_file_get_contents (data->file_entry);
  mime = gdata_media_content_get_content_type (GDATA_MEDIA_CONTENT (media_contents->data));
  query = photos_query_builder_insert_or_replace (state, remote_urn, "nie:mimeType", mime);
  photos_tracker_queue_update (self->queue, query->sparql, cancellable, NULL, NULL, NULL);

  title = gdata_entry_get_title (GDATA_ENTRY (data->file_entry));
  query = photos_query_builder_insert_or_replace (state, remote_urn, "nie:title", title);
  photos_tracker_queue_update (self->queue, query->sparql, cancellable, NULL, NULL, NULL);

  credit = gdata_picasaweb_file_get_credit (data->file_entry);
  query = photos_query_builder_insert_or_replace (state, remote_urn, "nco:creator", credit);
  photos_tracker_queue_update (self->queue, query->sparql, cancellable, NULL, NULL, NULL);

  exposure = g_strdup_printf ("%f", gdata_picasaweb_file_get_exposure (data->file_entry));
  query = photos_query_builder_insert_or_replace (state, remote_urn, "nmm:exposureTime", exposure);
  photos_tracker_queue_update (self->queue, query->sparql, cancellable, NULL, NULL, NULL);
  g_free (exposure);

  focal_length = g_strdup_printf ("%f", gdata_picasaweb_file_get_focal_length (data->file_entry));
  query = photos_query_builder_insert_or_replace (state, remote_urn, "nmm:focalLength", focal_length);
  photos_tracker_queue_update (self->queue, query->sparql, cancellable, NULL, NULL, NULL);
  g_free (focal_length);

  fstop = g_strdup_printf ("%f", gdata_picasaweb_file_get_fstop (data->file_entry));
  query = photos_query_builder_insert_or_replace (state, remote_urn, "nmm:fnumber", fstop);
  photos_tracker_queue_update (self->queue, query->sparql, cancellable, NULL, NULL, NULL);
  g_free (fstop);

  iso = g_strdup_printf ("%d", gdata_picasaweb_file_get_iso (data->file_entry));
  query = photos_query_builder_insert_or_replace (state, remote_urn, "nmm:isoSpeed", iso);
  photos_tracker_queue_update (self->queue, query->sparql, cancellable, NULL, NULL, NULL);
  g_free (iso);

  flash = gdata_picasaweb_file_get_flash (data->file_entry);
  query = photos_query_builder_insert_or_replace (state, remote_urn, "nmm:flash", flash ? flash_on : flash_off);
  photos_tracker_queue_update (self->queue, query->sparql, cancellable, NULL, NULL, NULL);

  make = gdata_picasaweb_file_get_make (data->file_entry);
  model = gdata_picasaweb_file_get_model (data->file_entry);
  if (model != NULL || make != NULL)
    {
      query = photos_query_builder_get_equipment_query (state, item_urn);
      photos_tracker_queue_select (self->queue,
                                   query->sparql,
                                   cancellable,
                                   photos_google_share_point_get_equipment,
                                   g_object_ref (task),
                                   g_object_unref);
    }

  width = g_strdup_printf ("%u", gdata_picasaweb_file_get_width (data->file_entry));
  query = photos_query_builder_insert_or_replace (state, remote_urn, "nfo:width", width);
  photos_tracker_queue_update (self->queue, query->sparql, cancellable, NULL, NULL, NULL);
  g_free (width);

  height = g_strdup_printf ("%u", gdata_picasaweb_file_get_height (data->file_entry));
  query = photos_query_builder_insert_or_replace (state, remote_urn, "nfo:height", height);
  photos_tracker_queue_update (self->queue, query->sparql, cancellable, NULL, NULL, NULL);
  g_free (height);

  /* borrowed from query_builder_mtime_update_query */
  timestamp = gdata_picasaweb_file_get_timestamp (data->file_entry) / G_USEC_PER_SEC;
  tv.tv_sec = timestamp;
  tv.tv_usec = 0;
  time = g_time_val_to_iso8601 (&tv);
  query = photos_query_builder_insert_or_replace (state, remote_urn, "nie:contentCreated", time);
  photos_tracker_queue_update (self->queue, query->sparql, cancellable, NULL, NULL, NULL);
  g_free (time);
  photos_query_free (query);

  photos_share_point_google_relate_objects (self, cancellable, remote_urn, item_urn);
}


static void
photos_share_point_google_metadata_added (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhotosBaseItem *item = PHOTOS_BASE_ITEM (source_object);
  GError *error = NULL;

  photos_base_item_add_metadata_finish (item, res, &error);
  if (error != NULL)
    {
      g_warning ("Could not remote link as metadata: %s", error->message);
      g_error_free (error);
    }

 g_object_unref (item);
}


static void
photos_share_point_google_create_tracker_entry (PhotosSharePointGoogle *self, GTask *task)
{
  PhotosSearchContextState *state;
  PhotosSharePointGoogleShareData *data;
  PhotosQuery *query;
  GApplication *app;
  GCancellable *cancellable;
  const gchar *id, *tag;
  gchar *identifier;

  app = g_application_get_default ();
  state = photos_search_context_get_state (PHOTOS_SEARCH_CONTEXT (app));

  cancellable = g_task_get_cancellable (task);
  data = (PhotosSharePointGoogleShareData *) g_task_get_task_data (task);

  id = gdata_entry_get_id (GDATA_ENTRY (data->file_entry));
  query = photos_query_builder_insert_remote_object (state, id);
  photos_tracker_queue_update_blank (self->queue,
                                     query->sparql,
                                     cancellable,
                                     photos_google_share_point_tracker_entry_created,
                                     g_object_ref (task),
                                     g_object_unref);
  photos_query_free (query);

  identifier = g_strdup_printf ("google:picasaweb:%s", id);
  tag = "Xmp.xmp.gnome-photos.google.identifier";
  g_object_set_data (G_OBJECT (data->item), "tag", (gpointer *) tag);
  photos_base_item_add_metadata_async (data->item,
                                       identifier,
                                       cancellable,
                                       photos_share_point_google_metadata_added,
                                       NULL);
  g_object_unref (task);
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

  g_task_return_boolean (task, TRUE);

  data->file_entry = GDATA_PICASAWEB_FILE (g_object_ref (file_entry));
  photos_share_point_google_create_tracker_entry (self, G_TASK (g_object_ref (task)));

 out:
  g_object_unref (file_entry);
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

  G_OBJECT_CLASS (photos_share_point_google_parent_class)->finalize (object);
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
