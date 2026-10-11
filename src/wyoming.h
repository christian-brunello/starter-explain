/*
 * starter-explain - wyoming.h
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 */

#ifndef ST_EXPLAIN_WYOMING_H_INCLUDED
#define ST_EXPLAIN_WYOMING_H_INCLUDED

#include <glib.h>

G_BEGIN_DECLS

/*
 * Speak @text via Wyoming TTS (e.g. wyoming-piper) at tcp://host:port.
 * On success returns a newly allocated path to a temporary WAV file
 * (caller deletes). Returns NULL on failure.
 */
gchar *st_explain_wyoming_synthesize (const gchar * host, guint16 port,
				      const gchar * text, guint timeout_ms,
				      GError ** error);

/* Parse tcp://HOST:PORT or HOST:PORT into host/port. */
gboolean st_explain_wyoming_parse_uri (const gchar * uri, gchar ** host_out,
				       guint16 * port_out, GError ** error);

G_END_DECLS
#endif
