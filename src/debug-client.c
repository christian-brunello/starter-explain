/*
 * starter-explain - debug-client.c
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 */

#include "debug-client.h"

#define ST_EXPLAIN_OBJECT_PATH "/org/starter/core/debug"
#define ST_EXPLAIN_INTERFACE   "org.starter.Core.Debug"

#define LOGI(msg...) g_log ("debug", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log ("debug", G_LOG_LEVEL_WARNING, msg)
#define LOGD(msg...) g_log ("debug", G_LOG_LEVEL_DEBUG, msg)

struct _STExplainDebugClient
{
  GDBusConnection *conn;
  gchar *host;
  guint16 port;
  guint signal_input;
  guint signal_output;
  guint signal_rule;
  guint signal_trigger;
  STExplainIoChangedFunc io_cb;
  gpointer io_cb_data;
  STExplainEngineTriggeredFunc engine_cb;
  gpointer engine_cb_data;
};

static void
emit_io (STExplainDebugClient * self, STExplainIoKind kind, GVariant * entry)
{
  const gchar *id = NULL;
  const gchar *descr = NULL;
  gint unit = 0;
  gdouble min = 0, max = 0, step = 0, value = 0;
  guint64 flags = 0;

  g_variant_get (entry, "{&s(&siddddt)}", &id, &descr, &unit, &min, &max,
		 &step, &value, &flags);

  LOGD ("%s %s = %g", kind == ST_EXPLAIN_IO_INPUT ? "input" : "output", id,
	value);

  if (self->io_cb)
    self->io_cb (self, id, kind, descr, unit, min, max, step, value, flags,
		 self->io_cb_data);
}

static void
emit_engine (STExplainDebugClient * self, STExplainEngineKind kind,
	     GVariant * parameters)
{
  const gchar *dest = NULL;
  const gchar *describe = NULL;

  g_variant_get (parameters, "(&s&s)", &dest, &describe);
  LOGD ("%s on %s => %s",
	kind == ST_EXPLAIN_ENGINE_RULE ? "RuleTriggered" : "TriggerTriggered",
	dest ? dest : "?", describe ? describe : "");

  if (self->engine_cb)
    self->engine_cb (self, kind, dest, describe ? describe : "",
		     self->engine_cb_data);
}

static void
on_input_changed (GDBusConnection * connection, const gchar * sender_name,
		  const gchar * object_path, const gchar * interface_name,
		  const gchar * signal_name, GVariant * parameters,
		  gpointer user_data)
{
  STExplainDebugClient *self = user_data;
  g_autoptr (GVariant) entry = NULL;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;

  entry = g_variant_get_child_value (parameters, 0);
  emit_io (self, ST_EXPLAIN_IO_INPUT, entry);
}

static void
on_output_changed (GDBusConnection * connection, const gchar * sender_name,
		   const gchar * object_path, const gchar * interface_name,
		   const gchar * signal_name, GVariant * parameters,
		   gpointer user_data)
{
  STExplainDebugClient *self = user_data;
  g_autoptr (GVariant) entry = NULL;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;

  entry = g_variant_get_child_value (parameters, 0);
  emit_io (self, ST_EXPLAIN_IO_OUTPUT, entry);
}

static void
on_rule_triggered (GDBusConnection * connection, const gchar * sender_name,
		   const gchar * object_path, const gchar * interface_name,
		   const gchar * signal_name, GVariant * parameters,
		   gpointer user_data)
{
  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;
  emit_engine (user_data, ST_EXPLAIN_ENGINE_RULE, parameters);
}

static void
on_trigger_triggered (GDBusConnection * connection, const gchar * sender_name,
		      const gchar * object_path, const gchar * interface_name,
		      const gchar * signal_name, GVariant * parameters,
		      gpointer user_data)
{
  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;
  emit_engine (user_data, ST_EXPLAIN_ENGINE_TRIGGER, parameters);
}

static void
unsubscribe_all (STExplainDebugClient * self)
{
  if (self->conn == NULL)
    return;
  if (self->signal_input)
    g_dbus_connection_signal_unsubscribe (self->conn, self->signal_input);
  if (self->signal_output)
    g_dbus_connection_signal_unsubscribe (self->conn, self->signal_output);
  if (self->signal_rule)
    g_dbus_connection_signal_unsubscribe (self->conn, self->signal_rule);
  if (self->signal_trigger)
    g_dbus_connection_signal_unsubscribe (self->conn, self->signal_trigger);
  self->signal_input = 0;
  self->signal_output = 0;
  self->signal_rule = 0;
  self->signal_trigger = 0;
}

static void
subscribe_all (STExplainDebugClient * self)
{
  self->signal_input =
    g_dbus_connection_signal_subscribe (self->conn, NULL, ST_EXPLAIN_INTERFACE,
					"InputChanged", ST_EXPLAIN_OBJECT_PATH,
					NULL, G_DBUS_SIGNAL_FLAGS_NONE,
					on_input_changed, self, NULL);
  self->signal_output =
    g_dbus_connection_signal_subscribe (self->conn, NULL, ST_EXPLAIN_INTERFACE,
					"OutputChanged",
					ST_EXPLAIN_OBJECT_PATH, NULL,
					G_DBUS_SIGNAL_FLAGS_NONE,
					on_output_changed, self, NULL);
  self->signal_rule =
    g_dbus_connection_signal_subscribe (self->conn, NULL, ST_EXPLAIN_INTERFACE,
					"RuleTriggered",
					ST_EXPLAIN_OBJECT_PATH, NULL,
					G_DBUS_SIGNAL_FLAGS_NONE,
					on_rule_triggered, self, NULL);
  self->signal_trigger =
    g_dbus_connection_signal_subscribe (self->conn, NULL, ST_EXPLAIN_INTERFACE,
					"TriggerTriggered",
					ST_EXPLAIN_OBJECT_PATH, NULL,
					G_DBUS_SIGNAL_FLAGS_NONE,
					on_trigger_triggered, self, NULL);
}

static gboolean
refresh_table (STExplainDebugClient * self, const gchar * method,
	       STExplainIoKind kind, GError ** error)
{
  g_autoptr (GVariant) result = NULL;
  g_autoptr (GVariant) dict = NULL;
  GVariantIter iter;
  GVariant *entry;

  result =
    g_dbus_connection_call_sync (self->conn, NULL, ST_EXPLAIN_OBJECT_PATH,
				 ST_EXPLAIN_INTERFACE, method, NULL,
				 G_VARIANT_TYPE ("(a{s(siddddt)})"),
				 G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  if (result == NULL)
    return FALSE;

  dict = g_variant_get_child_value (result, 0);
  g_variant_iter_init (&iter, dict);
  while ((entry = g_variant_iter_next_value (&iter)) != NULL)
    {
      emit_io (self, kind, entry);
      g_variant_unref (entry);
    }
  return TRUE;
}

STExplainDebugClient *
st_explain_debug_client_new (void)
{
  return g_new0 (STExplainDebugClient, 1);
}

void
st_explain_debug_client_free (STExplainDebugClient * client)
{
  if (client == NULL)
    return;
  st_explain_debug_client_disconnect (client);
  g_free (client->host);
  g_free (client);
}

void
st_explain_debug_client_set_io_callback (STExplainDebugClient * client,
					STExplainIoChangedFunc cb,
					gpointer user_data)
{
  g_return_if_fail (client != NULL);
  client->io_cb = cb;
  client->io_cb_data = user_data;
}

void
st_explain_debug_client_set_engine_callback (STExplainDebugClient * client,
					    STExplainEngineTriggeredFunc cb,
					    gpointer user_data)
{
  g_return_if_fail (client != NULL);
  client->engine_cb = cb;
  client->engine_cb_data = user_data;
}

gboolean
st_explain_debug_client_connect (STExplainDebugClient * client,
				 const gchar * host, guint16 port,
				 GError ** error)
{
  g_autofree gchar *addr = NULL;
  GDBusConnection *conn;

  g_return_val_if_fail (client != NULL, FALSE);

  st_explain_debug_client_disconnect (client);

  addr = g_strdup_printf ("tcp:host=%s,port=%hu", host, port);
  conn =
    g_dbus_connection_new_for_address_sync (addr,
					    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
					    NULL, NULL, error);
  if (conn == NULL)
    return FALSE;

  client->conn = conn;
  g_free (client->host);
  client->host = g_strdup (host);
  client->port = port;
  subscribe_all (client);
  LOGI ("connected to %s:%u", host, port);
  return TRUE;
}

void
st_explain_debug_client_disconnect (STExplainDebugClient * client)
{
  if (client == NULL || client->conn == NULL)
    return;
  unsubscribe_all (client);
  g_dbus_connection_close_sync (client->conn, NULL, NULL);
  g_clear_object (&client->conn);
}

gboolean
st_explain_debug_client_is_connected (STExplainDebugClient * client)
{
  return client && client->conn
    && !g_dbus_connection_is_closed (client->conn);
}

gboolean
st_explain_debug_client_refresh_io (STExplainDebugClient * client,
				    GError ** error)
{
  g_return_val_if_fail (client != NULL, FALSE);
  if (!st_explain_debug_client_is_connected (client))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
		   "not connected");
      return FALSE;
    }
  if (!refresh_table (client, "ListInputs", ST_EXPLAIN_IO_INPUT, error))
    return FALSE;
  if (!refresh_table (client, "ListOutputs", ST_EXPLAIN_IO_OUTPUT, error))
    return FALSE;
  return TRUE;
}

gboolean
st_explain_debug_client_log_status (STExplainDebugClient * client,
				    GError ** error)
{
  g_autoptr (GVariant) result = NULL;
  g_autoptr (GVariant) status = NULL;
  GVariantIter iter;
  const gchar *key;
  GVariant *value;

  g_return_val_if_fail (client != NULL, FALSE);
  if (!st_explain_debug_client_is_connected (client))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
		   "not connected");
      return FALSE;
    }

  result =
    g_dbus_connection_call_sync (client->conn, NULL, ST_EXPLAIN_OBJECT_PATH,
				 ST_EXPLAIN_INTERFACE, "GetStatus", NULL,
				 G_VARIANT_TYPE ("(a{sv})"),
				 G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  if (result == NULL)
    return FALSE;

  status = g_variant_get_child_value (result, 0);
  g_variant_iter_init (&iter, status);
  while (g_variant_iter_next (&iter, "{&sv}", &key, &value))
    {
      g_autofree gchar *printed = g_variant_print (value, FALSE);
      LOGI ("status %s=%s", key, printed);
      g_variant_unref (value);
    }
  return TRUE;
}
