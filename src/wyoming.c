/*
 * starter-explain - wyoming.c
 *
 * Minimal Wyoming protocol client for Piper TTS.
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 */

#include "wyoming.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#define LOGW(msg...) g_log ("wyoming", G_LOG_LEVEL_WARNING, msg)
#define LOGI(msg...) g_log ("wyoming", G_LOG_LEVEL_INFO, msg)

typedef struct
{
  gchar *type;
  JsonObject *data;		/* owned ref, may be empty object */
  GBytes *payload;		/* owned, may be NULL */
} WyEvent;

static void
wy_event_free (WyEvent * ev)
{
  if (!ev)
    return;
  g_free (ev->type);
  if (ev->data)
    json_object_unref (ev->data);
  if (ev->payload)
    g_bytes_unref (ev->payload);
  g_free (ev);
}

static gboolean
read_exact (GInputStream * in, guint8 * buf, gsize len, GCancellable * cancel,
	    GError ** error)
{
  gsize got = 0;
  while (got < len)
    {
      gssize n = g_input_stream_read (in, buf + got, len - got, cancel, error);
      if (n < 0)
	return FALSE;
      if (n == 0)
	{
	  g_set_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
		       "wyoming: connection closed");
	  return FALSE;
	}
      got += (gsize) n;
    }
  return TRUE;
}

static gchar *
read_line (GInputStream * in, GCancellable * cancel, GError ** error)
{
  GString *s = g_string_new ("");
  guint8 c;

  for (;;)
    {
      gssize n = g_input_stream_read (in, &c, 1, cancel, error);
      if (n < 0)
	{
	  g_string_free (s, TRUE);
	  return NULL;
	}
      if (n == 0)
	{
	  if (s->len == 0)
	    {
	      g_string_free (s, TRUE);
	      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
			   "wyoming: EOF");
	      return NULL;
	    }
	  break;
	}
      if (c == '\n')
	break;
      g_string_append_c (s, (gchar) c);
    }
  return g_string_free (s, FALSE);
}

static WyEvent *
read_event (GInputStream * in, GCancellable * cancel, GError ** error)
{
  g_autofree gchar *line = NULL;
  g_autoptr (JsonParser) parser = NULL;
  JsonObject *hdr;
  WyEvent *ev;
  gint64 data_len = 0;
  gint64 payload_len = 0;

  line = read_line (in, cancel, error);
  if (line == NULL)
    return NULL;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, line, -1, error))
    return NULL;

  hdr = json_node_get_object (json_parser_get_root (parser));
  if (hdr == NULL || !json_object_has_member (hdr, "type"))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		   "wyoming: bad event header");
      return NULL;
    }

  ev = g_new0 (WyEvent, 1);
  ev->type = g_strdup (json_object_get_string_member (hdr, "type"));
  ev->data = json_object_new ();

  if (json_object_has_member (hdr, "data") &&
      JSON_NODE_HOLDS_OBJECT (json_object_get_member (hdr, "data")))
    {
      JsonObject *inline_data =
	json_object_get_object_member (hdr, "data");
      GList *keys = json_object_get_members (inline_data);
      GList *l;
      for (l = keys; l; l = l->next)
	{
	  const gchar *k = l->data;
	  JsonNode *n = json_object_get_member (inline_data, k);
	  json_object_set_member (ev->data, k, json_node_copy (n));
	}
      g_list_free (keys);
    }

  if (json_object_has_member (hdr, "data_length"))
    data_len = json_object_get_int_member (hdr, "data_length");
  if (json_object_has_member (hdr, "payload_length"))
    payload_len = json_object_get_int_member (hdr, "payload_length");

  if (data_len > 0)
    {
      g_autofree guint8 *dbuf = g_malloc ((gsize) data_len + 1);
      g_autoptr (JsonParser) dp = json_parser_new ();
      if (!read_exact (in, dbuf, (gsize) data_len, cancel, error))
	{
	  wy_event_free (ev);
	  return NULL;
	}
      dbuf[data_len] = '\0';
      if (!json_parser_load_from_data
	  (dp, (const gchar *) dbuf, (gssize) data_len, error))
	{
	  wy_event_free (ev);
	  return NULL;
	}
      if (JSON_NODE_HOLDS_OBJECT (json_parser_get_root (dp)))
	{
	  JsonObject *extra =
	    json_node_get_object (json_parser_get_root (dp));
	  GList *keys = json_object_get_members (extra);
	  GList *l;
	  for (l = keys; l; l = l->next)
	    {
	      const gchar *k = l->data;
	      JsonNode *n = json_object_get_member (extra, k);
	      json_object_set_member (ev->data, k, json_node_copy (n));
	    }
	  g_list_free (keys);
	}
    }

  if (payload_len > 0)
    {
      guint8 *pbuf = g_malloc ((gsize) payload_len);
      if (!read_exact (in, pbuf, (gsize) payload_len, cancel, error))
	{
	  g_free (pbuf);
	  wy_event_free (ev);
	  return NULL;
	}
      ev->payload = g_bytes_new_take (pbuf, (gsize) payload_len);
    }

  return ev;
}

static gboolean
write_event (GOutputStream * out, const gchar * type, JsonObject * data,
	     GCancellable * cancel, GError ** error)
{
  g_autoptr (JsonBuilder) hb = json_builder_new ();
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autofree gchar *data_json = NULL;
  g_autofree gchar *header = NULL;
  gsize data_len = 0;
  gsize header_len = 0;
  JsonNode *root;

  if (data != NULL)
    {
      g_autoptr (JsonGenerator) dgen = json_generator_new ();
      JsonNode *dnode = json_node_new (JSON_NODE_OBJECT);
      json_node_set_object (dnode, data);
      json_generator_set_root (dgen, dnode);
      data_json = json_generator_to_data (dgen, &data_len);
      json_node_unref (dnode);
    }

  json_builder_begin_object (hb);
  json_builder_set_member_name (hb, "type");
  json_builder_add_string_value (hb, type);
  if (data_json && data_len > 0)
    {
      json_builder_set_member_name (hb, "data_length");
      json_builder_add_int_value (hb, (gint64) data_len);
    }
  json_builder_end_object (hb);

  root = json_builder_get_root (hb);
  json_generator_set_root (gen, root);
  header = json_generator_to_data (gen, &header_len);
  json_node_unref (root);

  {
    g_autofree gchar *line = g_strdup_printf ("%s\n", header);
    if (!g_output_stream_write_all
	(out, line, strlen (line), NULL, cancel, error))
      return FALSE;
  }
  if (data_json && data_len > 0)
    {
      if (!g_output_stream_write_all
	  (out, data_json, data_len, NULL, cancel, error))
	return FALSE;
    }
  return g_output_stream_flush (out, cancel, error);
}

static gboolean
write_wav_file (const gchar * path, guint rate, guint width, guint channels,
		const guint8 * pcm, gsize pcm_len, GError ** error)
{
  FILE *fp;
  guint32 byte_rate;
  guint16 block_align;
  guint32 data_size = (guint32) pcm_len;
  guint32 riff_size;

  if (rate == 0 || width == 0 || channels == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   "wyoming: invalid audio format");
      return FALSE;
    }

  block_align = (guint16) (width * channels);
  byte_rate = rate * block_align;
  riff_size = 36 + data_size;

  fp = g_fopen (path, "wb");
  if (fp == NULL)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
		   "cannot write %s", path);
      return FALSE;
    }

  fwrite ("RIFF", 1, 4, fp);
  fwrite (&riff_size, 4, 1, fp);
  fwrite ("WAVEfmt ", 1, 8, fp);
  {
    guint32 fmt_size = 16;
    guint16 audio_format = 1;	/* PCM */
    guint16 ch = (guint16) channels;
    guint32 sr = rate;
    guint16 bps = (guint16) (width * 8);
    fwrite (&fmt_size, 4, 1, fp);
    fwrite (&audio_format, 2, 1, fp);
    fwrite (&ch, 2, 1, fp);
    fwrite (&sr, 4, 1, fp);
    fwrite (&byte_rate, 4, 1, fp);
    fwrite (&block_align, 2, 1, fp);
    fwrite (&bps, 2, 1, fp);
  }
  fwrite ("data", 1, 4, fp);
  fwrite (&data_size, 4, 1, fp);
  fwrite (pcm, 1, pcm_len, fp);
  fclose (fp);
  return TRUE;
}

gboolean
st_explain_wyoming_parse_uri (const gchar * uri, gchar ** host_out,
			      guint16 * port_out, GError ** error)
{
  const gchar *p;
  g_autofree gchar *host = NULL;
  gchar *colon;
  long port;

  g_return_val_if_fail (uri != NULL && host_out && port_out, FALSE);

  p = uri;
  if (g_str_has_prefix (p, "tcp://"))
    p += 6;
  else if (g_str_has_prefix (p, "wyoming://"))
    p += 10;

  host = g_strdup (p);
  colon = strrchr (host, ':');
  if (colon == NULL || colon == host)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   "wyoming uri needs host:port (got %s)", uri);
      return FALSE;
    }
  *colon = '\0';
  port = strtol (colon + 1, NULL, 10);
  if (port <= 0 || port > 65535)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   "wyoming bad port in %s", uri);
      return FALSE;
    }
  *host_out = g_steal_pointer (&host);
  *port_out = (guint16) port;
  return TRUE;
}

gchar *
st_explain_wyoming_synthesize (const gchar * host, guint16 port,
			       const gchar * text, guint timeout_ms,
			       GError ** error)
{
  g_autoptr (GSocketClient) client = NULL;
  g_autoptr (GSocketConnection) conn = NULL;
  g_autoptr (GCancellable) cancel = NULL;
  GInputStream *in;
  GOutputStream *out;
  g_autoptr (JsonObject) data = NULL;
  GByteArray *pcm;
  guint rate = 22050;
  guint width = 2;
  guint channels = 1;
  gboolean got_audio = FALSE;
  g_autofree gchar *path = NULL;
  gint fd;
  guint i;

  g_return_val_if_fail (host != NULL && text != NULL, NULL);

  cancel = g_cancellable_new ();

  client = g_socket_client_new ();
  g_socket_client_set_timeout (client,
			       timeout_ms >
			       0 ? (timeout_ms + 999) / 1000 : 0);

  conn =
    g_socket_client_connect_to_host (client, host, port, cancel, error);
  if (conn == NULL)
    return NULL;

  in = g_io_stream_get_input_stream (G_IO_STREAM (conn));
  out = g_io_stream_get_output_stream (G_IO_STREAM (conn));

  data = json_object_new ();
  json_object_set_string_member (data, "text", text);
  if (!write_event (out, "synthesize", data, cancel, error))
    return NULL;

  pcm = g_byte_array_new ();

  for (i = 0; i < 100000; i++)
    {
      WyEvent *ev = read_event (in, cancel, error);
      if (ev == NULL)
	{
	  g_byte_array_unref (pcm);
	  return NULL;
	}

      if (g_strcmp0 (ev->type, "error") == 0)
	{
	  const gchar *msg = "wyoming error";
	  if (ev->data && json_object_has_member (ev->data, "text"))
	    msg = json_object_get_string_member (ev->data, "text");
	  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", msg);
	  wy_event_free (ev);
	  g_byte_array_unref (pcm);
	  return NULL;
	}

      if (g_strcmp0 (ev->type, "audio-start") == 0 && ev->data)
	{
	  if (json_object_has_member (ev->data, "rate"))
	    rate = (guint) json_object_get_int_member (ev->data, "rate");
	  if (json_object_has_member (ev->data, "width"))
	    width = (guint) json_object_get_int_member (ev->data, "width");
	  if (json_object_has_member (ev->data, "channels"))
	    channels =
	      (guint) json_object_get_int_member (ev->data, "channels");
	}
      else if (g_strcmp0 (ev->type, "audio-chunk") == 0)
	{
	  if (ev->data)
	    {
	      if (json_object_has_member (ev->data, "rate"))
		rate = (guint) json_object_get_int_member (ev->data, "rate");
	      if (json_object_has_member (ev->data, "width"))
		width =
		  (guint) json_object_get_int_member (ev->data, "width");
	      if (json_object_has_member (ev->data, "channels"))
		channels =
		  (guint) json_object_get_int_member (ev->data, "channels");
	    }
	  if (ev->payload)
	    {
	      gsize n;
	      const guint8 *p = g_bytes_get_data (ev->payload, &n);
	      g_byte_array_append (pcm, p, (guint) n);
	      got_audio = TRUE;
	    }
	}
      else if (g_strcmp0 (ev->type, "audio-stop") == 0)
	{
	  wy_event_free (ev);
	  break;
	}

      wy_event_free (ev);
    }

  if (!got_audio || pcm->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "wyoming: no audio received");
      g_byte_array_unref (pcm);
      return NULL;
    }

  fd = g_file_open_tmp ("starter-explain-XXXXXX.wav", &path, error);
  if (fd < 0)
    {
      g_byte_array_unref (pcm);
      return NULL;
    }
  close (fd);

  if (!write_wav_file
      (path, rate, width, channels, pcm->data, pcm->len, error))
    {
      g_unlink (path);
      g_byte_array_unref (pcm);
      return NULL;
    }

  LOGI ("wyoming WAV %s (%u bytes, %u Hz)", path, pcm->len, rate);
  g_byte_array_unref (pcm);
  return g_steal_pointer (&path);
}
