/*
 * starter-explain - test-explain.c
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 */

#include <glib.h>
#include <string.h>

#include "stex-config.h"
#include "state.h"
#include "prompt.h"
#include "explain.h"
#include "debug-client.h"
#include "mock-debug-server.h"
#include "log.h"

static STExplainState *g_test_state = NULL;
static guint g_test_hits = 0;
static guint g_engine_hits = 0;
static gchar *g_engine_dest = NULL;
static gchar *g_engine_describe = NULL;
static STExplainEngineKind g_engine_kind = ST_EXPLAIN_ENGINE_RULE;

static void
test_io_cb (STExplainDebugClient * c, const gchar * name,
	    STExplainIoKind kind, const gchar * description, gint unit,
	    gdouble min, gdouble max, gdouble step, gdouble value,
	    guint64 flags, gpointer user_data)
{
  gboolean explain = FALSE;
  gdouble oldv = 0;
  guint64 oldf = 0;

  (void) c;
  (void) user_data;
  if (st_explain_state_upsert
      (g_test_state, name, kind, description, unit, min, max, step, value,
       flags, &explain, &oldv, &oldf))
    g_test_hits++;
}

static void
test_config_load (void)
{
  g_autofree gchar *path =
    g_build_filename (g_test_get_dir (G_TEST_DIST), "..", "examples",
		      "demo.stex.xml", NULL);
  STExplainConfig *cfg = st_explain_config_new ();
  GError *error = NULL;

  g_assert_true (st_explain_config_load (cfg, path, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (cfg->host, ==, "127.0.0.1");
  g_assert_cmpuint (cfg->port, ==, 17373);
  g_assert_nonnull (cfg->llm_url);
  g_assert_nonnull (cfg->llm_user_template);
  g_assert_cmpuint (cfg->contexts->len, >=, 2);
  g_assert_cmpuint (cfg->ignore->len, >=, 2);
  g_assert_cmpuint (cfg->history_length, ==, 8);
  g_assert_cmpfloat (cfg->min_delta, ==, 0.05);
  g_assert_cmpuint (cfg->deadbands->len, >=, 1);

  g_assert_true (st_explain_config_set_core (cfg, "10.0.0.1:9999", &error));
  g_assert_cmpstr (cfg->host, ==, "10.0.0.1");
  g_assert_cmpuint (cfg->port, ==, 9999);

  st_explain_config_free (cfg);
}

static void
test_ignore_and_membership (void)
{
  g_autofree gchar *path =
    g_build_filename (g_test_get_dir (G_TEST_DIST), "..", "examples",
		      "demo.stex.xml", NULL);
  STExplainConfig *cfg = st_explain_config_new ();
  STExplainState *state;
  GPtrArray *ctx;
  gboolean explain = FALSE;
  gdouble oldv = 0;
  guint64 oldf = 0;

  g_assert_true (st_explain_config_load (cfg, path, NULL));
  state = st_explain_state_new (cfg);

  g_assert_true (st_explain_state_is_ignored (state, "TIME:hour"));
  g_assert_false (st_explain_state_is_ignored (state, "SUN:azimuth"));
  g_assert_true (st_explain_state_is_ignored (state, "DemoEM:meter0power"));
  g_assert_false (st_explain_state_is_ignored
		  (state, "DemoThermostat:temperature"));

  g_assert_false (st_explain_state_upsert
		  (state, "TIME:minute", ST_EXPLAIN_IO_INPUT, "x", 0, 0, 60,
		   1, 1.0, 0, &explain, &oldv, &oldf));
  g_assert_null (st_explain_state_get_io (state, "TIME:minute"));

  g_assert_true (st_explain_state_upsert
		 (state, "DemoBoiler:mode", ST_EXPLAIN_IO_OUTPUT, "Hybrid", 5,
		  0, 2, 1, 0.0, 0, &explain, &oldv, &oldf));
  g_assert_false (explain);
  g_assert_nonnull (st_explain_state_get_io (state, "DemoBoiler:mode"));

  g_assert_true (st_explain_state_upsert
		 (state, "DemoBoiler:mode", ST_EXPLAIN_IO_OUTPUT, "Hybrid", 5,
		  0, 2, 1, 1.0, 0, &explain, &oldv, &oldf));
  g_assert_true (explain);
  g_assert_cmpfloat (oldv, ==, 0.0);

  /* below global deadband 0.05 */
  g_assert_true (st_explain_state_upsert
		 (state, "DemoBoiler:mode", ST_EXPLAIN_IO_OUTPUT, "Hybrid", 5,
		  0, 2, 1, 1.02, 0, &explain, &oldv, &oldf));
  g_assert_false (explain);

  /* per-I/O deadband (DemoThermostat:temperature = 0.2) */
  st_explain_state_upsert (state, "DemoThermostat:temperature",
			   ST_EXPLAIN_IO_INPUT, "T", 1, 0, 50, 0.1, 20.0, 0,
			   &explain, &oldv, &oldf);
  st_explain_state_upsert (state, "DemoThermostat:temperature",
			   ST_EXPLAIN_IO_INPUT, "T", 1, 0, 50, 0.1, 20.1, 0,
			   &explain, &oldv, &oldf);
  g_assert_false (explain);
  st_explain_state_upsert (state, "DemoThermostat:temperature",
			   ST_EXPLAIN_IO_INPUT, "T", 1, 0, 50, 0.1, 20.5, 0,
			   &explain, &oldv, &oldf);
  g_assert_true (explain);
  g_assert_cmpfloat (st_explain_state_min_delta_for
		     (state, "DemoThermostat:temperature"), ==, 0.2);
  g_assert_cmpfloat (st_explain_state_min_delta_for (state, "unknown.io"), ==,
		     0.05);

  st_explain_state_rebuild_membership (state);
  ctx = st_explain_state_contexts_of (state, "DemoThermostat:temperature");
  g_assert_cmpuint (ctx->len, ==, 1);
  g_assert_cmpstr (((STExplainContext *) ctx->pdata[0])->id, ==, "room");
  g_ptr_array_free (ctx, TRUE);

  /* METEO:rain is in covers + weather → N:M */
  st_explain_state_upsert (state, "METEO:rain", ST_EXPLAIN_IO_INPUT, "rain",
			   0, 0, 100, 0.1, 0.0, 0, &explain, &oldv, &oldf);
  st_explain_state_upsert (state, "METEO:rain", ST_EXPLAIN_IO_INPUT, "rain",
			   0, 0, 100, 0.1, 1.0, 0, &explain, &oldv, &oldf);
  st_explain_state_rebuild_membership (state);
  ctx = st_explain_state_contexts_of (state, "METEO:rain");
  g_assert_cmpuint (ctx->len, ==, 2);
  g_ptr_array_free (ctx, TRUE);

  {
    GPtrArray *merged;
    ctx = st_explain_state_contexts_of (state, "METEO:rain");
    merged = st_explain_state_merge_io_names (state, ctx);
    g_assert_cmpuint (merged->len, >=, 1);
    g_ptr_array_free (merged, TRUE);
    g_ptr_array_free (ctx, TRUE);
  }

  st_explain_state_free (state);
  st_explain_config_free (cfg);
}

static void
test_prompt_and_units (void)
{
  g_autofree gchar *flags = st_explain_flags_to_text (0x1);
  g_autofree gchar *unit = st_explain_unit_to_text (1);
  g_autofree gchar *path =
    g_build_filename (g_test_get_dir (G_TEST_DIST), "..", "examples",
		      "demo.stex.xml", NULL);
  STExplainConfig *cfg = st_explain_config_new ();
  STExplainState *state;
  GPtrArray *ctx;
  g_autofree gchar *prompt = NULL;
  g_autofree gchar *prompt_action = NULL;
  gboolean explain = FALSE;
  gdouble oldv = 0;
  guint64 oldf = 0;

  g_assert_cmpstr (flags, ==, "manual_override");
  g_assert_cmpstr (unit, ==, "celsius");

  g_assert_true (st_explain_config_load (cfg, path, NULL));
  state = st_explain_state_new (cfg);
  st_explain_state_upsert (state, "DemoThermostat:temperature",
			   ST_EXPLAIN_IO_INPUT, "Demo room thermostat temperature",
			   1, 0, 50, 0.1, 40.0, 0, &explain, &oldv, &oldf);
  st_explain_state_upsert (state, "DemoThermostat:temperature",
			   ST_EXPLAIN_IO_INPUT, "Demo room thermostat temperature",
			   1, 0, 50, 0.1, 50.0, 0, &explain, &oldv, &oldf);
  st_explain_state_rebuild_membership (state);
  ctx = st_explain_state_contexts_of (state, "DemoThermostat:temperature");
  prompt =
    st_explain_prompt_build (cfg, state, ctx, "DemoThermostat:temperature",
			     ST_EXPLAIN_IO_INPUT, 40.0, 50.0, 0,
			     ST_EXPLAIN_ENGINE_RULE, NULL);
  g_assert_nonnull (prompt);
  g_assert_true (strstr (prompt, "TYPE: event") != NULL);
  g_assert_true (strstr (prompt, "EVENT:") != NULL);
  g_assert_true (strstr (prompt, "RULE: none") != NULL);
  g_assert_true (strstr (prompt, "CONTEXT:") != NULL);
  g_assert_true (strstr (prompt, "STATE:") != NULL);
  g_assert_true (strstr (prompt, "DemoThermostat:temperature") != NULL);
  g_assert_true (strstr (prompt, "Demo room") != NULL);
  /* description comes from debug I/O */
  g_assert_true (strstr (prompt, "Demo room thermostat temperature") != NULL);
  /* Compact event STATE: no unrelated PARAM dump */
  g_assert_true (strstr (prompt, "PARAM:BoilerThr") == NULL);

  st_explain_state_upsert (state, "DemoThermostat:relay",
			   ST_EXPLAIN_IO_OUTPUT, "relay", 4, 0, 1, 1, 0.0, 0,
			   &explain, &oldv, &oldf);
  st_explain_state_upsert (state, "DemoThermostat:relay",
			   ST_EXPLAIN_IO_OUTPUT, "relay", 4, 0, 1, 1, 1.0, 0,
			   &explain, &oldv, &oldf);
  st_explain_state_rebuild_membership (state);
  g_ptr_array_free (ctx, TRUE);
  ctx = st_explain_state_contexts_of (state, "DemoThermostat:relay");
  prompt_action =
    st_explain_prompt_build (cfg, state, ctx, "DemoThermostat:relay",
			     ST_EXPLAIN_IO_OUTPUT, 0.0, 1.0, 0,
			     ST_EXPLAIN_ENGINE_RULE,
			     "SET 1.000000 WHEN (DemoThermostat:temperature) < (18.000000)");
  g_assert_nonnull (prompt_action);
  g_assert_true (strstr (prompt_action, "TYPE: action") != NULL);
  g_assert_true (strstr (prompt_action, "SET 1.000000 WHEN") != NULL);
  g_assert_true (strstr (prompt_action, "STATE:") != NULL);
  g_assert_true (strstr (prompt_action, "DemoThermostat:temperature") != NULL);

  g_ptr_array_free (ctx, TRUE);
  st_explain_state_free (state);
  st_explain_config_free (cfg);
}

static void
test_engine_cb (STExplainDebugClient * c, STExplainEngineKind kind,
		const gchar * dest, const gchar * describe,
		gpointer user_data)
{
  (void) c;
  (void) user_data;
  g_engine_hits++;
  g_engine_kind = kind;
  g_free (g_engine_dest);
  g_free (g_engine_describe);
  g_engine_dest = g_strdup (dest);
  g_engine_describe = g_strdup (describe);
}

static void
pump_main (guint iterations)
{
  guint i;
  for (i = 0; i < iterations; i++)
    {
      while (g_main_context_iteration (NULL, FALSE))
	;
      g_usleep (5000);
    }
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static void
pump_until_engine_hits (guint min_hits, guint timeout_ms)
{
  gint64 deadline = g_get_monotonic_time () + (gint64) timeout_ms * 1000;
  while (g_engine_hits < min_hits && g_get_monotonic_time () < deadline)
    {
      while (g_main_context_iteration (NULL, FALSE))
	;
      g_usleep (10000);
    }
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static void
test_rule_signal_and_cache (void)
{
  g_autofree gchar *path =
    g_build_filename (g_test_get_dir (G_TEST_DIST), "..", "examples",
		      "demo.stex.xml", NULL);
  STExplainConfig *cfg = st_explain_config_new ();
  STExplainService *svc;
  GError *error = NULL;
  STDbgMockServer *mock;
  const gchar *rule =
    "SET 1.000000 WHEN (DemoThermostat:temperature) < (18.000000)";
  g_autofree gchar *prompt = NULL;
  GPtrArray *ctx;
  gboolean explain = FALSE;
  gdouble oldv = 0;
  guint64 oldf = 0;

  g_assert_true (st_explain_config_load (cfg, path, &error));
  g_assert_no_error (error);
  cfg->debounce_ms = 0;

  mock = st_dbg_mock_server_start (&error);
  g_assert_no_error (error);

  g_engine_hits = 0;
  g_clear_pointer (&g_engine_dest, g_free);
  g_clear_pointer (&g_engine_describe, g_free);

  svc = st_explain_service_new (cfg, TRUE, NULL);
  /* Also listen on client for signal delivery check */
  st_explain_debug_client_set_engine_callback
    (st_explain_service_get_client (svc), test_engine_cb, NULL);

  g_assert_true (st_explain_debug_client_connect
		 (st_explain_service_get_client (svc),
		  st_dbg_mock_server_get_host (mock),
		  st_dbg_mock_server_get_port (mock), &error));
  g_assert_no_error (error);

  st_dbg_mock_server_emit_rule_triggered (mock, "DemoThermostat:relay", rule);
  pump_until_engine_hits (1, 3000);

  g_assert_cmpuint (g_engine_hits, >=, 1);
  g_assert_cmpstr (g_engine_dest, ==, "DemoThermostat:relay");
  g_assert_cmpstr (g_engine_describe, ==, rule);
  g_assert_cmpint (g_engine_kind, ==, ST_EXPLAIN_ENGINE_RULE);

  /* Restore service engine callback overwritten above */
  st_explain_debug_client_set_engine_callback
    (st_explain_service_get_client (svc), st_explain_service_on_engine, svc);
  g_engine_hits = 0;
  st_dbg_mock_server_emit_rule_triggered (mock, "DemoThermostat:relay", rule);
  pump_until_engine_hits (1, 3000);

  /* Consume via on_io path: seed state then change output */
  st_explain_state_upsert (st_explain_service_get_state (svc),
			   "DemoThermostat:relay", ST_EXPLAIN_IO_OUTPUT,
			   "relay", 4, 0, 1, 1, 0.0, 0, &explain, &oldv,
			   &oldf);
  st_explain_service_on_io (st_explain_service_get_client (svc),
			    "DemoThermostat:relay", ST_EXPLAIN_IO_OUTPUT,
			    "relay", 4, 0, 1, 1, 1.0, 0, svc);

  /* Prompt builder still works with explicit RULE (cache already consumed) */
  st_explain_state_rebuild_membership (st_explain_service_get_state (svc));
  ctx =
    st_explain_state_contexts_of (st_explain_service_get_state (svc),
				  "DemoThermostat:relay");
  prompt =
    st_explain_prompt_build (cfg, st_explain_service_get_state (svc), ctx,
			     "DemoThermostat:relay", ST_EXPLAIN_IO_OUTPUT, 0.0,
			     1.0, 0, ST_EXPLAIN_ENGINE_RULE, rule);
  g_assert_true (strstr (prompt, "RULE:") != NULL);
  g_ptr_array_free (ctx, TRUE);

  st_explain_service_free (svc);
  st_dbg_mock_server_stop (mock);
  st_explain_config_free (cfg);
  g_clear_pointer (&g_engine_dest, g_free);
  g_clear_pointer (&g_engine_describe, g_free);
}

static void
test_debug_client_mock (void)
{
  GError *error = NULL;
  STDbgMockServer *mock;
  STExplainDebugClient *client;
  STExplainConfig *cfg = st_explain_config_new ();

  cfg->history_length = 4;
  g_test_state = st_explain_state_new (cfg);
  g_test_hits = 0;

  mock = st_dbg_mock_server_start (&error);
  g_assert_no_error (error);

  client = st_explain_debug_client_new ();
  st_explain_debug_client_set_io_callback (client, test_io_cb, NULL);
  g_assert_true (st_explain_debug_client_connect
		 (client, st_dbg_mock_server_get_host (mock),
		  st_dbg_mock_server_get_port (mock), &error));
  g_assert_no_error (error);

  st_dbg_mock_server_set_input (mock, "boiler.temp", "Boiler", 42.0);
  g_assert_true (st_explain_debug_client_refresh_io (client, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (g_test_hits, >=, 1);
  g_assert_nonnull (st_explain_state_get_io (g_test_state, "boiler.temp"));

  g_assert_true (st_explain_debug_client_log_status (client, &error));
  g_assert_no_error (error);

  st_explain_debug_client_free (client);
  st_dbg_mock_server_stop (mock);
  st_explain_state_free (g_test_state);
  g_test_state = NULL;
  st_explain_config_free (cfg);
}

int
main (int argc, char **argv)
{
  st_explain_log_init ("test");
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/explain/config", test_config_load);
  g_test_add_func ("/explain/state", test_ignore_and_membership);
  g_test_add_func ("/explain/prompt", test_prompt_and_units);
  g_test_add_func ("/explain/debug-client", test_debug_client_mock);
  g_test_add_func ("/explain/rule-cache", test_rule_signal_and_cache);

  return g_test_run ();
}
