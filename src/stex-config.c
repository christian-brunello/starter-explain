/*
 * starter-explain - config.c
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gio/gio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <string.h>
#include <stdlib.h>

#include "stex-config.h"

#define LOGD(msg...) g_log ("config", G_LOG_LEVEL_DEBUG, msg)
#define LOGI(msg...) g_log ("config", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log ("config", G_LOG_LEVEL_WARNING, msg)
#define LOGE(msg...) g_log ("config", G_LOG_LEVEL_ERROR, msg)

#define ST_EXPLAIN_DEFAULT_HOST "127.0.0.1"
#define ST_EXPLAIN_DEFAULT_PORT 17373
#define ST_EXPLAIN_DEFAULT_HISTORY 8
#define ST_EXPLAIN_DEFAULT_QUEUE 4
#define ST_EXPLAIN_DEFAULT_TIMEOUT_MS 30000

static void
io_ref_free (gpointer p)
{
  STExplainIoRef *r = p;
  if (r == NULL)
    return;
  g_free (r->name);
  g_free (r->pattern);
  g_free (r);
}

static void
header_free (gpointer p)
{
  STExplainHeader *h = p;
  if (h == NULL)
    return;
  g_free (h->name);
  g_free (h->value);
  g_free (h);
}

static void
context_cfg_free (gpointer p)
{
  STExplainContextCfg *c = p;
  if (c == NULL)
    return;
  g_free (c->id);
  g_free (c->name);
  g_free (c->description);
  if (c->io_refs)
    g_ptr_array_free (c->io_refs, TRUE);
  g_free (c);
}

static gchar *
xml_prop (xmlNodePtr node, const char *name)
{
  xmlChar *v = xmlGetProp (node, (const xmlChar *) name);
  gchar *out;

  if (v == NULL)
    return NULL;
  out = g_strdup ((const char *) v);
  xmlFree (v);
  return out;
}

static gchar *
xml_content_trim (xmlNodePtr node)
{
  xmlChar *c = xmlNodeGetContent (node);
  gchar *out;
  gchar *trim;

  if (c == NULL)
    return g_strdup ("");
  out = g_strdup ((const char *) c);
  xmlFree (c);
  trim = g_strstrip (out);
  if (trim != out)
    {
      trim = g_strdup (trim);
      g_free (out);
      return trim;
    }
  return out;
}

static guint
parse_uint_attr (xmlNodePtr node, const char *name, guint def)
{
  g_autofree gchar *v = xml_prop (node, name);
  gchar *end = NULL;
  unsigned long n;

  if (v == NULL || *v == '\0')
    return def;
  n = strtoul (v, &end, 10);
  if (end == v || *end != '\0')
    return def;
  return (guint) n;
}

static gint
parse_int_attr (xmlNodePtr node, const char *name, gint def)
{
  g_autofree gchar *v = xml_prop (node, name);
  gchar *end = NULL;
  long n;

  if (v == NULL || *v == '\0')
    return def;
  n = strtol (v, &end, 10);
  if (end == v || *end != '\0')
    return def;
  return (gint) n;
}

static gdouble
parse_double_attr (xmlNodePtr node, const char *name, gdouble def)
{
  g_autofree gchar *v = xml_prop (node, name);
  gchar *end = NULL;
  gdouble n;

  if (v == NULL || *v == '\0')
    return def;
  n = g_ascii_strtod (v, &end);
  if (end == v || *end != '\0')
    return def;
  return n;
}

static STExplainIoRef *
parse_io_ref (xmlNodePtr node, GError ** error)
{
  STExplainIoRef *ref;
  g_autofree gchar *name = xml_prop (node, "name");
  g_autofree gchar *pattern = xml_prop (node, "pattern");
  g_autofree gchar *md = xml_prop (node, "min-delta");

  if ((name == NULL || *name == '\0') &&
      (pattern == NULL || *pattern == '\0'))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
		   "<io> requires name or pattern");
      return NULL;
    }

  ref = g_new0 (STExplainIoRef, 1);
  ref->name = name ? g_steal_pointer (&name) : NULL;
  ref->pattern = pattern ? g_steal_pointer (&pattern) : NULL;
  if (md != NULL && *md != '\0')
    {
      gchar *end = NULL;
      gdouble n = g_ascii_strtod (md, &end);
      if (end != md && *end == '\0')
	{
	  ref->has_min_delta = TRUE;
	  ref->min_delta = n;
	}
    }
  return ref;
}

static void
deadband_add_copy (STExplainConfig * cfg, const STExplainIoRef * src)
{
  STExplainIoRef *c;

  if (cfg == NULL || src == NULL || !src->has_min_delta)
    return;

  c = g_new0 (STExplainIoRef, 1);
  c->name = g_strdup (src->name);
  c->pattern = g_strdup (src->pattern);
  c->has_min_delta = TRUE;
  c->min_delta = src->min_delta;
  g_ptr_array_add (cfg->deadbands, c);
}

static gboolean
parse_headers (xmlNodePtr parent, GPtrArray * headers, GError ** error)
{
  xmlNodePtr child;

  for (child = parent->children; child; child = child->next)
    {
      STExplainHeader *h;

      if (child->type != XML_ELEMENT_NODE)
	continue;
      if (!xmlStrEqual (child->name, (const xmlChar *) "header"))
	continue;

      h = g_new0 (STExplainHeader, 1);
      h->name = xml_prop (child, "name");
      h->value = xml_prop (child, "value");
      if (h->name == NULL || h->value == NULL)
	{
	  header_free (h);
	  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
		       "<header> requires name and value");
	  return FALSE;
	}
      g_ptr_array_add (headers, h);
    }
  return TRUE;
}

static gboolean
parse_llm (xmlNodePtr node, STExplainConfig * cfg, GError ** error)
{
  xmlNodePtr child;

  g_free (cfg->llm_url);
  cfg->llm_url = xml_prop (node, "url");
  g_free (cfg->llm_model);
  cfg->llm_model = xml_prop (node, "model");
  cfg->llm_timeout_ms =
    parse_uint_attr (node, "timeout-ms", ST_EXPLAIN_DEFAULT_TIMEOUT_MS);
  cfg->llm_max_queue =
    parse_uint_attr (node, "max-queue", ST_EXPLAIN_DEFAULT_QUEUE);
  cfg->llm_num_predict = parse_int_attr (node, "num-predict", 40);
  cfg->llm_keep_alive = parse_int_attr (node, "keep-alive", -1);

  if (cfg->llm_url == NULL || *cfg->llm_url == '\0')
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
		   "<llm> requires url");
      return FALSE;
    }

  for (child = node->children; child; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;
      if (xmlStrEqual (child->name, (const xmlChar *) "header"))
	{
	  STExplainHeader *h = g_new0 (STExplainHeader, 1);
	  h->name = xml_prop (child, "name");
	  h->value = xml_prop (child, "value");
	  if (h->name == NULL || h->value == NULL)
	    {
	      header_free (h);
	      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
			   "<header> requires name and value");
	      return FALSE;
	    }
	  g_ptr_array_add (cfg->llm_headers, h);
	}
      else if (xmlStrEqual (child->name, (const xmlChar *) "system"))
	{
	  g_free (cfg->llm_system);
	  cfg->llm_system = xml_content_trim (child);
	}
      else if (xmlStrEqual (child->name, (const xmlChar *) "prompt"))
	{
	  xmlNodePtr p;
	  for (p = child->children; p; p = p->next)
	    {
	      if (p->type != XML_ELEMENT_NODE)
		continue;
	      if (xmlStrEqual (p->name, (const xmlChar *) "user-template"))
		{
		  g_free (cfg->llm_user_template);
		  cfg->llm_user_template = xml_content_trim (p);
		}
	    }
	}
    }

  if (cfg->llm_user_template == NULL || *cfg->llm_user_template == '\0')
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
		   "<llm> requires <prompt><user-template>");
      return FALSE;
    }

  return TRUE;
}

static gboolean
parse_tts (xmlNodePtr node, STExplainConfig * cfg, GError ** error)
{
  g_free (cfg->tts_url);
  cfg->tts_url = xml_prop (node, "url");
  g_free (cfg->tts_method);
  cfg->tts_method = xml_prop (node, "method");
  if (cfg->tts_method == NULL || *cfg->tts_method == '\0')
    {
      g_free (cfg->tts_method);
      cfg->tts_method = g_strdup ("POST");
    }
  cfg->tts_timeout_ms =
    parse_uint_attr (node, "timeout-ms", ST_EXPLAIN_DEFAULT_TIMEOUT_MS);
  cfg->tts_max_queue =
    parse_uint_attr (node, "max-queue", ST_EXPLAIN_DEFAULT_QUEUE);
  cfg->tts_max_chars = parse_uint_attr (node, "max-chars", 0);
  g_free (cfg->tts_content_type);
  cfg->tts_content_type = xml_prop (node, "content-type");
  g_free (cfg->tts_body_template);
  cfg->tts_body_template = xml_prop (node, "body-template");
  g_free (cfg->tts_response);
  cfg->tts_response = xml_prop (node, "response");

  if (cfg->tts_url == NULL || *cfg->tts_url == '\0')
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
		   "<tts> requires url");
      return FALSE;
    }
  if (g_ascii_strcasecmp (cfg->tts_method, "wyoming") == 0 ||
      (cfg->tts_url &&
       (g_str_has_prefix (cfg->tts_url, "tcp://") ||
	g_str_has_prefix (cfg->tts_url, "wyoming://"))))
    {
      /* Wyoming Piper: no HTTP body */
    }
  else if (g_ascii_strcasecmp (cfg->tts_method, "GET") != 0)
    {
      if (cfg->tts_body_template == NULL)
	{
	  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
		       "<tts> POST requires body-template");
	  return FALSE;
	}
      if (cfg->tts_content_type == NULL)
	cfg->tts_content_type = g_strdup ("application/json");
    }

  if (!parse_headers (node, cfg->tts_headers, error))
    return FALSE;

  return TRUE;
}

static gboolean
parse_context (xmlNodePtr node, STExplainConfig * cfg, GError ** error)
{
  STExplainContextCfg *c;
  xmlNodePtr child;

  c = g_new0 (STExplainContextCfg, 1);
  c->id = xml_prop (node, "id");
  c->name = xml_prop (node, "name");
  c->description = xml_prop (node, "description");
  c->io_refs = g_ptr_array_new_with_free_func (io_ref_free);

  if (c->id == NULL || *c->id == '\0')
    {
      context_cfg_free (c);
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
		   "<context> requires id");
      return FALSE;
    }

  for (child = node->children; child; child = child->next)
    {
      STExplainIoRef *ref;

      if (child->type != XML_ELEMENT_NODE)
	continue;
      if (!xmlStrEqual (child->name, (const xmlChar *) "io"))
	continue;
      ref = parse_io_ref (child, error);
      if (ref == NULL)
	{
	  context_cfg_free (c);
	  return FALSE;
	}
      if (ref->has_min_delta)
	deadband_add_copy (cfg, ref);
      g_ptr_array_add (c->io_refs, ref);
    }

  g_ptr_array_add (cfg->contexts, c);
  return TRUE;
}

STExplainConfig *
st_explain_config_new (void)
{
  STExplainConfig *cfg = g_new0 (STExplainConfig, 1);

  cfg->host = g_strdup (ST_EXPLAIN_DEFAULT_HOST);
  cfg->port = ST_EXPLAIN_DEFAULT_PORT;
  cfg->llm_headers = g_ptr_array_new_with_free_func (header_free);
  cfg->tts_headers = g_ptr_array_new_with_free_func (header_free);
  cfg->ignore = g_ptr_array_new_with_free_func (io_ref_free);
  cfg->contexts = g_ptr_array_new_with_free_func (context_cfg_free);
  cfg->deadbands = g_ptr_array_new_with_free_func (io_ref_free);
  cfg->llm_timeout_ms = ST_EXPLAIN_DEFAULT_TIMEOUT_MS;
  cfg->llm_max_queue = ST_EXPLAIN_DEFAULT_QUEUE;
  cfg->llm_num_predict = 40;
  cfg->llm_keep_alive = -1;
  cfg->tts_method = g_strdup ("POST");
  cfg->tts_timeout_ms = ST_EXPLAIN_DEFAULT_TIMEOUT_MS;
  cfg->tts_max_queue = ST_EXPLAIN_DEFAULT_QUEUE;
  cfg->debounce_ms = 500;
  cfg->coalesce = TRUE;
  cfg->history_length = ST_EXPLAIN_DEFAULT_HISTORY;
  cfg->min_delta = 0.0;
  cfg->audio_player = g_strdup ("mpg123 -q {{file}}");
  return cfg;
}

void
st_explain_config_free (STExplainConfig * cfg)
{
  if (cfg == NULL)
    return;

  g_free (cfg->host);
  g_free (cfg->llm_url);
  g_free (cfg->llm_model);
  g_free (cfg->llm_system);
  g_free (cfg->llm_user_template);
  g_free (cfg->tts_url);
  g_free (cfg->tts_method);
  g_free (cfg->tts_content_type);
  g_free (cfg->tts_body_template);
  g_free (cfg->tts_response);
  g_free (cfg->audio_player);

  if (cfg->llm_headers)
    g_ptr_array_free (cfg->llm_headers, TRUE);
  if (cfg->tts_headers)
    g_ptr_array_free (cfg->tts_headers, TRUE);
  if (cfg->ignore)
    g_ptr_array_free (cfg->ignore, TRUE);
  if (cfg->contexts)
    g_ptr_array_free (cfg->contexts, TRUE);
  if (cfg->deadbands)
    g_ptr_array_free (cfg->deadbands, TRUE);

  g_free (cfg);
}

gboolean
st_explain_config_set_core (STExplainConfig * cfg, const gchar * host_port,
			    GError ** error)
{
  const gchar *colon;
  gchar *end = NULL;
  unsigned long p;
  gchar *h;

  g_return_val_if_fail (cfg != NULL, FALSE);
  g_return_val_if_fail (host_port != NULL, FALSE);

  colon = strchr (host_port, ':');
  if (colon == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   "expected HOST:PORT, got '%s'", host_port);
      return FALSE;
    }

  p = strtoul (colon + 1, &end, 10);
  if (end == colon + 1 || *end != '\0' || p == 0 || p > 65535)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   "invalid port in '%s'", host_port);
      return FALSE;
    }

  h = g_strndup (host_port, (gsize) (colon - host_port));
  if (*h == '\0')
    {
      g_free (h);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   "empty host in '%s'", host_port);
      return FALSE;
    }

  g_free (cfg->host);
  cfg->host = h;
  cfg->port = (guint16) p;
  return TRUE;
}

gboolean
st_explain_config_load (STExplainConfig * cfg, const gchar * path,
			GError ** error)
{
  xmlDocPtr doc;
  xmlNodePtr root;
  xmlNodePtr child;

  g_return_val_if_fail (cfg != NULL, FALSE);
  g_return_val_if_fail (path != NULL, FALSE);

  doc = xmlReadFile (path, NULL, XML_PARSE_NOBLANKS);
  if (doc == NULL)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
		   "failed to parse XML '%s'", path);
      return FALSE;
    }

  root = xmlDocGetRootElement (doc);
  if (root == NULL || !xmlStrEqual (root->name, (const xmlChar *) "stex"))
    {
      xmlFreeDoc (doc);
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
		   "root element must be <stex>");
      return FALSE;
    }

  for (child = root->children; child; child = child->next)
    {
      if (child->type != XML_ELEMENT_NODE)
	continue;

      if (xmlStrEqual (child->name, (const xmlChar *) "core"))
	{
	  g_autofree gchar *host = xml_prop (child, "host");
	  guint port = parse_uint_attr (child, "port", cfg->port);
	  if (host && *host)
	    {
	      g_free (cfg->host);
	      cfg->host = g_steal_pointer (&host);
	    }
	  if (port > 0 && port <= 65535)
	    cfg->port = (guint16) port;
	}
      else if (xmlStrEqual (child->name, (const xmlChar *) "llm"))
	{
	  if (!parse_llm (child, cfg, error))
	    {
	      xmlFreeDoc (doc);
	      return FALSE;
	    }
	}
      else if (xmlStrEqual (child->name, (const xmlChar *) "tts"))
	{
	  if (!parse_tts (child, cfg, error))
	    {
	      xmlFreeDoc (doc);
	      return FALSE;
	    }
	}
      else if (xmlStrEqual (child->name, (const xmlChar *) "audio"))
	{
	  g_autofree gchar *player = xml_prop (child, "player");
	  if (player && *player)
	    {
	      g_free (cfg->audio_player);
	      cfg->audio_player = g_steal_pointer (&player);
	    }
	}
      else if (xmlStrEqual (child->name, (const xmlChar *) "ignore"))
	{
	  xmlNodePtr io;
	  for (io = child->children; io; io = io->next)
	    {
	      STExplainIoRef *ref;
	      if (io->type != XML_ELEMENT_NODE)
		continue;
	      if (!xmlStrEqual (io->name, (const xmlChar *) "io"))
		continue;
	      ref = parse_io_ref (io, error);
	      if (ref == NULL)
		{
		  xmlFreeDoc (doc);
		  return FALSE;
		}
	      g_ptr_array_add (cfg->ignore, ref);
	    }
	}
      else if (xmlStrEqual (child->name, (const xmlChar *) "contexts"))
	{
	  xmlNodePtr ctx;
	  for (ctx = child->children; ctx; ctx = ctx->next)
	    {
	      if (ctx->type != XML_ELEMENT_NODE)
		continue;
	      if (!xmlStrEqual (ctx->name, (const xmlChar *) "context"))
		continue;
	      if (!parse_context (ctx, cfg, error))
		{
		  xmlFreeDoc (doc);
		  return FALSE;
		}
	    }
	}
      else if (xmlStrEqual (child->name, (const xmlChar *) "deadband"))
	{
	  xmlNodePtr io;
	  for (io = child->children; io; io = io->next)
	    {
	      STExplainIoRef *ref;
	      if (io->type != XML_ELEMENT_NODE)
		continue;
	      if (!xmlStrEqual (io->name, (const xmlChar *) "io"))
		continue;
	      ref = parse_io_ref (io, error);
	      if (ref == NULL)
		{
		  xmlFreeDoc (doc);
		  return FALSE;
		}
	      if (!ref->has_min_delta)
		{
		  io_ref_free (ref);
		  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
			       "<deadband><io> requires min-delta");
		  xmlFreeDoc (doc);
		  return FALSE;
		}
	      g_ptr_array_add (cfg->deadbands, ref);
	    }
	}
      else if (xmlStrEqual (child->name, (const xmlChar *) "policy"))
	{
	  cfg->debounce_ms = parse_uint_attr (child, "debounce-ms",
					      cfg->debounce_ms);
	  {
	    g_autofree gchar *c = xml_prop (child, "coalesce");
	    if (c)
	      cfg->coalesce = (g_ascii_strcasecmp (c, "true") == 0 ||
			       g_strcmp0 (c, "1") == 0);
	  }
	  cfg->history_length =
	    parse_uint_attr (child, "history-length", cfg->history_length);
	  if (cfg->history_length == 0)
	    cfg->history_length = 1;
	  cfg->min_delta = parse_double_attr (child, "min-delta",
					      cfg->min_delta);
	}
    }

  xmlFreeDoc (doc);

  if (cfg->llm_url == NULL)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
		   "missing <llm> in config");
      return FALSE;
    }

  LOGI ("loaded config %s (core=%s:%u contexts=%u ignore=%u deadband=%u "
	"min-delta=%g history=%u)",
	path, cfg->host, cfg->port, cfg->contexts->len, cfg->ignore->len,
	cfg->deadbands->len, cfg->min_delta, cfg->history_length);
  return TRUE;
}
