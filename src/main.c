/*
 * starter-explain - main.c
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

#include <glib.h>
#include <glib-unix.h>
#include <libxml/parser.h>

#include "log.h"
#include "stex-config.h"
#include "explain.h"

#define LOGI(msg...) g_log ("main", G_LOG_LEVEL_INFO, msg)
#define LOGE(msg...) g_log ("main", G_LOG_LEVEL_ERROR, msg)
#define LOGW(msg...) g_log ("main", G_LOG_LEVEL_WARNING, msg)

typedef struct
{
  gchar *config_file;
  gchar *core;
  gchar *log_text;
  gboolean dry_run;
  gboolean version;
} Options;

static GMainLoop *loop = NULL;
static STExplainService *service = NULL;
static STExplainConfig *config = NULL;
static guint reconnect_source = 0;

static gboolean
on_signal (gpointer user_data)
{
  (void) user_data;
  LOGI ("signal received, quitting");
  if (loop)
    g_main_loop_quit (loop);
  return G_SOURCE_REMOVE;
}

static gboolean
try_reconnect (gpointer user_data)
{
  GError *error = NULL;

  (void) user_data;
  reconnect_source = 0;

  if (service == NULL)
    return G_SOURCE_REMOVE;

  if (st_explain_debug_client_is_connected
      (st_explain_service_get_client (service)))
    return G_SOURCE_REMOVE;

  LOGI ("reconnecting to %s:%u...", config->host, config->port);
  if (!st_explain_service_start (service, &error))
    {
      LOGW ("reconnect failed: %s", error ? error->message : "?");
      g_clear_error (&error);
      reconnect_source = g_timeout_add_seconds (5, try_reconnect, NULL);
    }
  return G_SOURCE_REMOVE;
}

static gboolean
watch_connection (gpointer user_data)
{
  (void) user_data;
  if (service == NULL || config == NULL)
    return G_SOURCE_CONTINUE;

  if (!st_explain_debug_client_is_connected
      (st_explain_service_get_client (service)))
    {
      if (reconnect_source == 0)
	{
	  LOGW ("debug connection lost");
	  reconnect_source = g_timeout_add_seconds (2, try_reconnect, NULL);
	}
    }
  return G_SOURCE_CONTINUE;
}

int
main (int argc, char **argv)
{
  Options opt = { 0 };
  GError *error = NULL;
  GOptionContext *oc;
  GOptionEntry entries[] = {
    { "config", 'c', 0, G_OPTION_ARG_FILENAME, &opt.config_file,
      "Path to stex XML configuration", "FILE" },
    { "core", 0, 0, G_OPTION_ARG_STRING, &opt.core,
      "Override debug server HOST:PORT", "HOST:PORT" },
    { "dry-run", 'n', 0, G_OPTION_ARG_NONE, &opt.dry_run,
      "Log LLM replies without TTS/audio", NULL },
    { "log-text", 'l', 0, G_OPTION_ARG_FILENAME, &opt.log_text,
      "Append LLM text and trigger reason to FILE", "FILE" },
    { "version", 'V', 0, G_OPTION_ARG_NONE, &opt.version,
      "Show version and exit", NULL },
    { NULL }
  };

  st_explain_log_init ("explain");
  xmlInitParser ();

  oc = g_option_context_new ("- STARTER explain daemon");
  g_option_context_add_main_entries (oc, entries, NULL);
  if (!g_option_context_parse (oc, &argc, &argv, &error))
    {
      g_printerr ("%s\n", error->message);
      g_clear_error (&error);
      g_option_context_free (oc);
      return 1;
    }
  g_option_context_free (oc);

  if (opt.version)
    {
      g_print ("%s %s\n", PACKAGE_NAME, PACKAGE_VERSION);
      return 0;
    }

  if (opt.config_file == NULL)
    {
      g_printerr ("--config=FILE is required\n");
      return 1;
    }

  config = st_explain_config_new ();
  if (!st_explain_config_load (config, opt.config_file, &error))
    {
      LOGE ("config: %s", error ? error->message : "?");
      g_clear_error (&error);
      st_explain_config_free (config);
      return 1;
    }

  if (opt.core)
    {
      if (!st_explain_config_set_core (config, opt.core, &error))
	{
	  LOGE ("--core: %s", error ? error->message : "?");
	  g_clear_error (&error);
	  st_explain_config_free (config);
	  return 1;
	}
    }

  service = st_explain_service_new (config, opt.dry_run, opt.log_text);
  if (!st_explain_service_start (service, &error))
    {
      LOGW ("initial connect failed: %s (will retry)",
	    error ? error->message : "?");
      g_clear_error (&error);
      reconnect_source = g_timeout_add_seconds (2, try_reconnect, NULL);
    }

  g_unix_signal_add (SIGINT, on_signal, NULL);
  g_unix_signal_add (SIGTERM, on_signal, NULL);
  g_timeout_add_seconds (3, watch_connection, NULL);

  loop = g_main_loop_new (NULL, FALSE);
  LOGI ("running");
  g_main_loop_run (loop);

  if (reconnect_source)
    g_source_remove (reconnect_source);
  st_explain_service_free (service);
  st_explain_config_free (config);
  g_main_loop_unref (loop);
  g_free (opt.config_file);
  g_free (opt.core);
  g_free (opt.log_text);
  xmlCleanupParser ();
  return 0;
}
