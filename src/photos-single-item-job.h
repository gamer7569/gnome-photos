/*
 * Photos - access, organize and share your photos on GNOME
 * Copyright © 2012 – 2016 Red Hat, Inc.
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

#ifndef PHOTOS_SINGLE_ITEM_JOB_H
#define PHOTOS_SINGLE_ITEM_JOB_H

#include <gio/gio.h>
#include <tracker-sparql.h>

#include "photos-search-context.h"

G_BEGIN_DECLS

#define PHOTOS_TYPE_SINGLE_ITEM_JOB (photos_single_item_job_get_type ())

#define PHOTOS_SINGLE_ITEM_JOB(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
   PHOTOS_TYPE_SINGLE_ITEM_JOB, PhotosSingleItemJob))

#define PHOTOS_IS_SINGLE_ITEM_JOB(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
   PHOTOS_TYPE_SINGLE_ITEM_JOB))

typedef void (*PhotosSingleItemJobCallback) (TrackerSparqlCursor *, gpointer);

typedef struct _PhotosSingleItemJob      PhotosSingleItemJob;
typedef struct _PhotosSingleItemJobClass PhotosSingleItemJobClass;

GType                  photos_single_item_job_get_type             (void) G_GNUC_CONST;

PhotosSingleItemJob   *photos_single_item_job_new                  (const gchar *urn);

TrackerSparqlCursor   *photos_single_item_job_finish               (PhotosSingleItemJob *self,
                                                                    GAsyncResult *res,
                                                                    GError **error);

void                   photos_single_item_job_run                  (PhotosSingleItemJob *self,
                                                                    PhotosSearchContextState *state,
                                                                    gint flags,
                                                                    GCancellable *cancellable,
                                                                    GAsyncReadyCallback callback,
                                                                    gpointer user_data);

G_END_DECLS

#endif /* PHOTOS_SINGLE_ITEM_JOB_H */
