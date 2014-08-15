/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "ostree-metalink.h"

#include "otutil.h"
#include "libgsystem.h"

typedef enum {
  OSTREE_METALINK_STATE_INITIAL,
  OSTREE_METALINK_STATE_METALINK,
  OSTREE_METALINK_STATE_FILES,
  OSTREE_METALINK_STATE_FILE,
  OSTREE_METALINK_STATE_SIZE,
  OSTREE_METALINK_STATE_VERIFICATION,
  OSTREE_METALINK_STATE_HASH,
  OSTREE_METALINK_STATE_RESOURCES,
  OSTREE_METALINK_STATE_URL,

  OSTREE_METALINK_STATE_PASSTHROUGH, /* Ignoring unknown elements */
  OSTREE_METALINK_STATE_ERROR
} OstreeMetalinkState;

struct OstreeMetalink
{
  GObject parent_instance;

  OstreeMetalink *fetcher;
  char *requested_file;
  guint64 max_size;
};

G_DEFINE_TYPE (OstreeMetalink, _ostree_metalink, G_TYPE_OBJECT)

struct OstreeMetalinkRequest
{
  OstreeMetalink *metalink;

  guint passthrough_depth;
  OstreeMetalinkState passthrough_previous;
  
  guint found_a_file_element : 1;
  guint found_our_file_element : 1;
  guint verification_known : 1;

  guint64 size;
  char *verification_sha256;
  char *verification_sha512;

  GPtrArray *urls;

  OstreeMetalinkState state;
}

static void
state_transition (OstreeMetalinkRequest  *self,
                  OstreeMetalinkState     new_state)
{
  g_assert (self->state != new_state);
  self->state = new_state;
}

static void
unknown_element (OstreeMetalinkRequest         *self,
                 const char                    *element_name,
                 GError                       **error)
{
  state_transition (self, OSTREE_METALINK_STATE_PASSTHROUGH);
  g_assert (self->passthrough_depth == 0);
}

static void
metalink_parser_start (GMarkupParseContext  *context,
                       const gchar          *element_name,
                       const gchar         **attribute_names,
                       const gchar         **attribute_values,
                       gpointer              user_data,
                       GError              **error)
{
  OstreeMetalinkRequest *self = user_data;

  switch (self->state)
    {
    case OSTREE_METALINK_STATE_INITIAL:
      if (strcmp (element_name, "metalink") == 0)
        state_transition (self, OSTREE_METALINK_STATE_METALINK);
      else
        unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_METALINK:
      if (strcmp (element_name, "files") == 0)
        state_transition (self, OSTREE_METALINK_STATE_FILES);
      else
        unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_FILES:
      /* If we've already processed a <file> element we're OK with, just
       * ignore the others.
       */
      if (self->urls->length > 0)
        {
          state_transition (self, OSTREE_METALINK_STATE_PASSTHROUGH);
        }
      else if (strcmp (element_name, "file") == 0)
        {
          const char *file_name;

          g_clear_pointer (&self->file_name, g_free);
          if (!g_markup_collect_attributes (element_name,
                                            attribute_names,
                                            attribute_values,
                                            error,
                                            G_MARKUP_COLLECT_STRING,
                                            "name",
                                            &file_name,
                                            G_MARKUP_COLLECT_INVALID))
            goto out;

          self->found_a_file_element = TRUE;

          if (strcmp (file_name, self->requested_file) != 0)
            {
              state_transition (self, OSTREE_METALINK_STATE_PASSTHROUGH);
              g_assert (self->passthrough_depth == 0);
            }
          else
            {
              self->found_our_file_element = TRUE;
              state_transition (self, OSTREE_METALINK_STATE_FILE);
            }
        }
      else
        unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_FILE:
      if (strcmp (element_name, "size") == 0)
        state_transition (self, OSTREE_METALINK_STATE_SIZE);
      else if (strcmp (element_name, "verification") == 0)
        state_transition (self, OSTREE_METALINK_STATE_VERIFICATION);
      else if (strcmp (element_name, "resources") == 0)
        state_transition (self, OSTREE_METALINK_STATE_RESOURCES);
      else
        unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_SIZE:
      unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_VERIFICATION:
      if (strcmp (element_name, "hash") == 0)
        {
          gs_free char *verification_type_str = NULL;

          state_transition (self, OSTREE_METALINK_STATE_HASH);
          if (!g_markup_collect_attributes (element_name,
                                            attribute_names,
                                            attribute_values,
                                            error,
                                            G_MARKUP_COLLECT_STRING,
                                            "name",
                                            &verification_type_str,
                                            G_MARKUP_COLLECT_INVALID))
            goto out;

          /* Only accept sha256/sha512 */
          self->verification_known = TRUE;
          if (strcmp (verification_type_str, "sha256") == 0)
            self->verification_type = G_CHECKSUM_SHA256;
          else if (strcmp (verification_type_str, "sha512") == 0)
            self->verification_type = G_CHECKSUM_SHA512;
          else
            self->verification_known = FALSE;
        }
      else
        unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_HASH:
      unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_RESOURCES:
      if (self->size == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No <size> element found or it is zero");
          goto out;
        }
      if (!self->verification_known)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No <verification> element with known <hash type=> found");
          goto out;
        }

      if (strcmp (element_name, "url") == 0)
        {
          const char *protocol;

          if (!g_markup_collect_attributes (element_name,
                                            attribute_names,
                                            attribute_values,
                                            error,
                                            G_MARKUP_COLLECT_STRING,
                                            "protocol",
                                            &protocol,
                                            G_MARKUP_COLLECT_INVALID))
            goto out;

          /* Ignore non-HTTP resources */
          if (!(strcmp (protocol, "http") == 0 || strcmp (protocol, "https") == 0))
            state_transition (self, OSTREE_METALINK_STATE_PASSTHROUGH);
          else
            state_transition (self, OSTREE_METALINK_STATE_URL);
        }
      else
        unknown_element (self, element_name, error);
      }
      break;
    case OSTREE_METALINK_STATE_URL:
      unknown_element (self, element_name, error);
      break;
    case OSTREE_METALINK_STATE_PASSTHROUGH:
      self->passthrough_depth++;
      break;
    }

 out:
  return;
}

static void
metalink_parser_end (GMarkupParseContext  *context,
                     const gchar          *element_name,
                     gpointer              user_data,
                     GError              **error)
{
  OstreeMetalinkRequest *self = user_data;
}

static void
metalink_parser_text (GMarkupParseContext *context,
                      const gchar         *text,
                      gsize                text_len,
                      gpointer             user_data,
                      GError             **error)
{
  OstreeMetalinkRequest *self = user_data;

  switch (self->state)
    {
    case OSTREE_METALINK_STATE_INITIAL:
      break;
    case OSTREE_METALINK_STATE_METALINK:
      break;
    case OSTREE_METALINK_STATE_FILES:
      break;
    case OSTREE_METALINK_STATE_FILE:
      break;
    case OSTREE_METALINK_STATE_SIZE:
      {
        gs_free char *duped = g_strndup (text, text_len);
        self->size = g_ascii_strtoull (duped, NULL, 10);
      }
      break;
    case OSTREE_METALINK_STATE_VERIFICATION:
      if (self->verification_known)
        {
          switch (self->verification_type)
            {
            case G_CHECKSUM_SHA256:
              self->verification_sha256 = g_strndup (text, text_len);
              break;
            case G_CHECKSUM_SHA512:
              self->verification_sha512 = g_strndup (text, text_len);
              break;
            default:
              g_assert_not_reached ();
            }
        }
      break;
    case OSTREE_METALINK_STATE_HASH:
      {
        g_clear_pointer (&self->verification_value, g_free);
        self->verification_value = g_strndup (text, text_len);
      }
      break;
    case OSTREE_METALINK_STATE_RESOURCES:
      break;
    case OSTREE_METALINK_STATE_URL:
      {
        gs_free char *uri_text = g_strndup (text, text_len);
        SoupURI *uri = soup_uri_new (uri_text);
        if (uri != NULL)
          g_ptr_array_add (self->urls, uri);
      }
      break;
    case OSTREE_METALINK_STATE_PASSTHROUGH:
      break;
    }
 out:
  return;
}

static void
_ostree_metalink_finalize (GObject *object)
{
  OstreeMetalink *self;

  self = OSTREE_METALINK (object);

  G_OBJECT_CLASS (_ostree_metalink_parent_class)->finalize (object);
}

static void
_ostree_metalink_class_init (OstreeMetalinkClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = _ostree_metalink_finalize;
}

static void
_ostree_metalink_init (OstreeMetalink *self)
{
}

OstreeMetalink *
_ostree_metalink_new (OstreeFetcher  *fetcher,
                      const char     *requested_file,
                      guint64         max_size,
                      SoupURI        *uri)
{
  OstreeMetalink *self = (OstreeMetalink*)g_object_new (OSTREE_TYPE_METALINK, NULL);

  self->requested_file = g_strdup (requested_file);
  self->max_size = max_size;
 
  return self;
}

static gboolean
valid_hex_checksum (const char *s, gsize expected_len)
{
  gsize len = strspn (s, "01234567890abcdef");

  return len == expected_len && s[len] == '\0';
}

static gboolean
start_target_request_phase (OstreeMetalinkRequest      *self,
                            GError                    **error)
{
  gboolean ret = FALSE;

  if (!self->found_a_file_element)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No <file> element found");
      goto out;
    }

  if (!self->found_our_file_element)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No <file name='%s'> found", self->metalink->requested_file);
      goto out;
    }

  if (!(self->verification_sha256 || self->verification_sha512))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No <verification> hash for sha256 or sha512 found");
      goto out;
    }

  if (self->verification_sha256 && !valid_hex_checksum (self->verification_sha256, 64))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid hash digest for sha256");
      goto out;
    }

  if (self->verification_sha512 && !valid_hex_checksum (self->verification_sha512, 128))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid hash digest for sha512");
      goto out;
    }

  if (self->urls->length == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No <url method='http'> elements found");
      goto out;
    }

  
  ret = TRUE;
 out:
  return ret;
}

static void
on_metalink_bytes_read (GObject           *src,
                        GAsyncResult      *result,
                        gpointer           user_data)
{
  GError *local_error = NULL;
  GTask *task = user_data;
  OstreeMetalinkRequest *self = g_task_get_task_data (task);
  gs_unref_bytes GBytes *bytes = NULL;
  gsize len;
  const guint8 *data;

  bytes = g_input_stream_read_bytes_finish ((GInputStream*)src,
                                            result, error);
  if (!bytes)
    goto out;
  
  data = g_bytes_get_data (bytes, 

  if (g_bytes_get_size (bytes) == 0)
    {
      if (!start_target_request_phase (self, &local_error))
        goto out;
    }
  else
    {
      g_markup_parse_context_parse (self->parser, g_
      g_input_stream_read_bytes_async ((GInputStream*)src, 8192, G_PRIORITY_DEFAULT,
                                       g_task_get_cancellable (task),
                                       on_metalink_bytes_read, task);
    }

 out:
  if (local_error)
    g_task_return_error (task, local_error);
}

static void
on_retrieved_metalink (GObject           *src,
                       GAsyncResult      *result,
                       gpointer           user_data)
{
  GError *local_error = NULL;
  GTask *task = user_data;
  gs_unref_object GInputStream *metalink_stream = NULL;

  metalink_stream = ostree_fetcher_stream_uri_finish ((OstreeFetcher*)src, result, &local_error);
  if (!metalink_stream)
    goto out;

  g_input_stream_read_bytes_async (metalink_stream, 8192, G_PRIORITY_DEFAULT,
                                   g_task_get_cancellable (task),
                                   on_metalink_bytes_read, task);

 out:
  if (local_error)
    g_task_return_error (task, local_error);
}

static void
ostree_metalink_request_unref (gpointer data)
{
  OstreeMetalinkRequest  *request = data;
  g_object_unref (request->metalink);
  g_ptr_array_unref (request->urls);
  g_free (request);
}
                               

void
_ostree_metalink_request_async (OstreeMetalink         *self,
                                GCancellable           *cancellable,
                                GAsyncReadyCallback     callback,
                                gpointer                user_data)
{
  GTask *task = g_task_new (self, cancellable, callback, user_data);
  OstreeMetalinkRequest *request = g_new0 (OstreeMetalinkRequest, 1);
  request->metalink = g_object_ref (self);
  request->urls = g_ptr_array_new_with_free_func (g_free);
  g_task_set_task_data (task, request, ostree_metalink_request_unref);
  _ostree_fetcher_stream_uri_async (self->fetcher, self->uri,
                                    self->max_size, cancellable,
                                    on_retrieved_metalink, task);
}

gboolean
ostree_metalink_request_finish (OstreeMetalink         *self,
                                GAsyncResult           *result,
                                SoupURI               **out_target_uri,
                                GFile                 **out_data,
                                GError                **error)
{
}

struct MetalinkSyncCallState
{
  gboolean                running;
  gboolean                success;
  SoupURI               **out_target_uri;
  GFile                 **out_data;
  GError                **error;
}

static void
on_async_result (GObject          *src,
                 GAsyncResult     *result,
                 gpointer          user_data)
{
  MetalinkSyncCallState *state = user_data;

  state->success = ostree_metalink_request_finish ((OstreeMetalink*)src, result,
                                                   state->out_target_uri, state->out_data,
                                                   state->error);
  state->running = FALSE;
}

gboolean
_ostree_metalink_request_sync (OstreeMetalink         *self,
                               GCancellable           *cancellable,
                               SoupURI               **out_target_uri,
                               GFile                 **out_data,
                               GError                **error)
{
  gboolean ret = FALSE;
  GMainContext *sync_context = g_main_context_new ();
  MetalinkSyncCallState state = { 0, };
  
  _ostree_metalink_request_async (self, cancellable, on_async_result, &state);
  
  while (state.running)
    g_main_context_iteration (sync_context, TRUE);

  ret = state.success;
 out:
  if (sync_context)
    g_main_context_unref (sync_context);
  return ret;
}
