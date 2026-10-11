/*
 * starter-debugger - mock-debug-server.c
 *
 * Minimal org.starter.Core.Debug GDBus peer for unit tests.
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

#include "mock-debug-server.h"

#include <stdio.h>
#include <string.h>

#define ST_DBG_OBJECT_PATH "/org/starter/core/debug"
#define ST_DBG_INTERFACE   "org.starter.Core.Debug"

typedef struct
{
  gchar *name;
  gchar *address;
  guint port;
  guint state;
} MockPeer;

typedef struct
{
  guint handle;
  gchar *id;
} MockPoint;

typedef struct
{
  gchar *id;
  gchar *description;
  gdouble value;
} MockIo;

struct _STDbgMockServer
{
  GMainContext *context;
  GMainLoop *loop;
  GThread *thread;
  GMutex mutex;
  GCond cond;
  gboolean ready;
  gboolean start_ok;
  GError *start_error;

  GDBusServer *server;
  GPtrArray *connections;	/* GDBusConnection* */
  gchar *host;
  guint16 port;

  GHashTable *peers;		/* name -> MockPeer */
  GHashTable *inputs;		/* id -> MockIo */
  GHashTable *outputs;		/* id -> MockIo */
  GHashTable *breakpoints;	/* handle -> MockPoint */
  GHashTable *watchpoints;
  guint next_handle;
  gboolean paused;

  gchar *last_set_input_id;
  gdouble last_set_input_value;
  gchar *last_set_output_id;
  gdouble last_set_output_value;

  gboolean stopping;
};

static const gchar introspection_xml[] =
  "<node name='/org/starter/core/debug'>"
  "  <interface name='org.starter.Core.Debug'>"
  "    <method name='GetStatus'>"
  "      <arg direction='out' type='a{sv}' name='status'/>"
  "    </method>"
  "    <method name='ListPeers'>"
  "      <arg direction='out' type='a(ssuu)' name='peers'/>"
  "    </method>"
  "    <method name='ListInputs'>"
  "      <arg direction='out' type='a{s(siddddt)}' name='inputs'/>"
  "    </method>"
  "    <method name='ListOutputs'>"
  "      <arg direction='out' type='a{s(siddddt)}' name='outputs'/>"
  "    </method>"
  "    <method name='SetInput'>"
  "      <arg direction='in' type='s' name='id'/>"
  "      <arg direction='in' type='d' name='value'/>"
  "      <arg direction='in' type='t' name='flags'/>"
  "      <arg direction='out' type='i' name='ok'/>"
  "    </method>"
  "    <method name='SetOutput'>"
  "      <arg direction='in' type='s' name='id'/>"
  "      <arg direction='in' type='d' name='value'/>"
  "      <arg direction='in' type='t' name='flags'/>"
  "      <arg direction='out' type='i' name='ok'/>"
  "    </method>"
  "    <method name='AddBreakpoint'>"
  "      <arg direction='in' type='s' name='id'/>"
  "      <arg direction='out' type='u' name='handle'/>"
  "    </method>"
  "    <method name='RemoveBreakpoint'>"
  "      <arg direction='in' type='u' name='handle'/>"
  "      <arg direction='out' type='b' name='ok'/>"
  "    </method>"
  "    <method name='ListBreakpoints'>"
  "      <arg direction='out' type='a(us)' name='breakpoints'/>"
  "    </method>"
  "    <method name='AddWatchpoint'>"
  "      <arg direction='in' type='s' name='id'/>"
  "      <arg direction='out' type='u' name='handle'/>"
  "    </method>"
  "    <method name='RemoveWatchpoint'>"
  "      <arg direction='in' type='u' name='handle'/>"
  "      <arg direction='out' type='b' name='ok'/>"
  "    </method>"
  "    <method name='ListWatchpoints'>"
  "      <arg direction='out' type='a(us)' name='watchpoints'/>"
  "    </method>"
  "    <method name='Continue'/>"
  "    <method name='GetPaused'>"
  "      <arg direction='out' type='b' name='paused'/>"
  "    </method>"
  "    <signal name='PeerChanged'>"
  "      <arg name='name' type='s'/>"
  "      <arg name='address' type='s'/>"
  "      <arg name='port' type='u'/>"
  "      <arg name='state' type='u'/>"
  "      <arg name='event' type='s'/>"
  "    </signal>"
  "    <signal name='InputChanged'>"
  "      <arg name='object' type='{s(siddddt)}'/>"
  "    </signal>"
  "    <signal name='OutputChanged'>"
  "      <arg name='object' type='{s(siddddt)}'/>"
  "    </signal>"
  "    <signal name='StatsChanged'>"
  "      <arg name='name' type='s'/>"
  "    </signal>"
  "    <signal name='VarChanged'>"
  "      <arg name='name' type='s'/>"
  "      <arg name='describe' type='s'/>"
  "    </signal>"
  "    <signal name='RuleTriggered'>"
  "      <arg name='dest' type='s'/>"
  "      <arg name='describe' type='s'/>"
  "    </signal>"
  "    <signal name='TriggerTriggered'>"
  "      <arg name='dest' type='s'/>"
  "      <arg name='describe' type='s'/>"
  "    </signal>"
  "    <signal name='Stopped'>"
  "      <arg name='reason' type='s'/>"
  "      <arg name='id' type='s'/>"
  "      <arg name='value' type='d'/>"
  "      <arg name='handle' type='u'/>"
  "    </signal>"
  "  </interface>"
  "</node>";

static GDBusNodeInfo *introspection_data = NULL;

static void
mock_peer_free (gpointer data)
{
  MockPeer *p = data;

  if (!p)
    return;
  g_free (p->name);
  g_free (p->address);
  g_free (p);
}

static void
mock_point_free (gpointer data)
{
  MockPoint *p = data;

  if (!p)
    return;
  g_free (p->id);
  g_free (p);
}

static void
mock_io_free (gpointer data)
{
  MockIo *io = data;

  if (!io)
    return;
  g_free (io->id);
  g_free (io->description);
  g_free (io);
}

static void
mock_io_upsert (GHashTable * table, const gchar * id,
		const gchar * description, gdouble value)
{
  MockIo *io;

  io = g_hash_table_lookup (table, id);
  if (io == NULL)
    {
      io = g_new0 (MockIo, 1);
      io->id = g_strdup (id);
      g_hash_table_insert (table, io->id, io);
    }
  g_free (io->description);
  io->description = g_strdup (description ? description : "");
  io->value = value;
}

static void
list_io_table (GHashTable * table, GDBusMethodInvocation * invocation)
{
  GVariantBuilder builder;
  GHashTableIter iter;
  gpointer key, value;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{s(siddddt)}"));
  g_hash_table_iter_init (&iter, table);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      MockIo *io = value;

      g_variant_builder_add (&builder, "{s(siddddt)}", io->id,
			     io->description ? io->description : "", 0,
			     0.0, 100.0, 1.0, io->value, (guint64) 0);
    }
  g_dbus_method_invocation_return_value (invocation,
					 g_variant_new ("(a{s(siddddt)})",
							&builder));
}

static void
broadcast_signal (STDbgMockServer * self, const gchar * name, GVariant * params)
{
  guint i;

  g_variant_ref_sink (params);
  for (i = 0; i < self->connections->len; i++)
    {
      GDBusConnection *conn = self->connections->pdata[i];

      g_dbus_connection_emit_signal (conn, NULL, ST_DBG_OBJECT_PATH,
				     ST_DBG_INTERFACE, name, params, NULL);
    }
  g_variant_unref (params);
}

static guint
add_point (GHashTable * table, guint * next, const gchar * id)
{
  MockPoint *p;
  guint handle;

  if (id == NULL || *id == '\0')
    return 0;

  handle = (*next)++;
  if (handle == 0)
    handle = (*next)++;

  p = g_new0 (MockPoint, 1);
  p->handle = handle;
  p->id = g_strdup (id);
  g_hash_table_insert (table, GUINT_TO_POINTER (handle), p);
  return handle;
}

static void
list_points (GHashTable * table, GDBusMethodInvocation * invocation)
{
  GVariantBuilder builder;
  GHashTableIter iter;
  gpointer key, value;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(us)"));
  g_hash_table_iter_init (&iter, table);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      MockPoint *p = value;

      g_variant_builder_add (&builder, "(us)", p->handle, p->id);
    }
  g_dbus_method_invocation_return_value (invocation,
					 g_variant_new ("(a(us))", &builder));
}

static void
handle_method_call (GDBusConnection * connection,
		    const gchar * sender,
		    const gchar * object_path,
		    const gchar * interface_name,
		    const gchar * method_name,
		    GVariant * parameters,
		    GDBusMethodInvocation * invocation, gpointer user_data)
{
  STDbgMockServer *self = user_data;

  (void) connection;
  (void) sender;
  (void) object_path;
  (void) interface_name;

  if (g_strcmp0 (method_name, "GetStatus") == 0)
    {
      GVariantBuilder b;

      g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_add (&b, "{sv}", "package",
			     g_variant_new_string ("mock-debug"));
      g_variant_builder_add (&b, "{sv}", "version",
			     g_variant_new_string ("0.0.0"));
      g_variant_builder_add (&b, "{sv}", "bind-host",
			     g_variant_new_string (self->host));
      g_variant_builder_add (&b, "{sv}", "bind-port",
			     g_variant_new_uint16 (self->port));
      g_variant_builder_add (&b, "{sv}", "peers",
			     g_variant_new_uint32 (g_hash_table_size
						   (self->peers)));
      g_variant_builder_add (&b, "{sv}", "inputs",
			     g_variant_new_uint32 (g_hash_table_size
						   (self->inputs)));
      g_variant_builder_add (&b, "{sv}", "outputs",
			     g_variant_new_uint32 (g_hash_table_size
						   (self->outputs)));
      g_variant_builder_add (&b, "{sv}", "paused",
			     g_variant_new_boolean (self->paused));
      g_variant_builder_add (&b, "{sv}", "breakpoints",
			     g_variant_new_uint32 (g_hash_table_size
						   (self->breakpoints)));
      g_variant_builder_add (&b, "{sv}", "watchpoints",
			     g_variant_new_uint32 (g_hash_table_size
						   (self->watchpoints)));
      g_dbus_method_invocation_return_value (invocation,
					     g_variant_new ("(a{sv})", &b));
    }
  else if (g_strcmp0 (method_name, "ListPeers") == 0)
    {
      GVariantBuilder b;
      GHashTableIter iter;
      gpointer key, value;

      g_variant_builder_init (&b, G_VARIANT_TYPE ("a(ssuu)"));
      g_hash_table_iter_init (&iter, self->peers);
      while (g_hash_table_iter_next (&iter, &key, &value))
	{
	  MockPeer *p = value;

	  g_variant_builder_add (&b, "(ssuu)", p->name, p->address, p->port,
				 p->state);
	}
      g_dbus_method_invocation_return_value (invocation,
					     g_variant_new ("(a(ssuu))", &b));
    }
  else if (g_strcmp0 (method_name, "ListInputs") == 0)
    list_io_table (self->inputs, invocation);
  else if (g_strcmp0 (method_name, "ListOutputs") == 0)
    list_io_table (self->outputs, invocation);
  else if (g_strcmp0 (method_name, "SetInput") == 0)
    {
      const gchar *id = NULL;
      gdouble value = 0;
      guint64 flags = 0;

      g_variant_get (parameters, "(&sdt)", &id, &value, &flags);
      (void) flags;
      g_free (self->last_set_input_id);
      self->last_set_input_id = g_strdup (id);
      self->last_set_input_value = value;
      g_dbus_method_invocation_return_value (invocation,
					     g_variant_new ("(i)", 1));
    }
  else if (g_strcmp0 (method_name, "SetOutput") == 0)
    {
      const gchar *id = NULL;
      gdouble value = 0;
      guint64 flags = 0;

      g_variant_get (parameters, "(&sdt)", &id, &value, &flags);
      (void) flags;
      g_free (self->last_set_output_id);
      self->last_set_output_id = g_strdup (id);
      self->last_set_output_value = value;
      g_dbus_method_invocation_return_value (invocation,
					     g_variant_new ("(i)", 1));
    }
  else if (g_strcmp0 (method_name, "AddBreakpoint") == 0)
    {
      const gchar *id = NULL;
      guint handle;

      g_variant_get (parameters, "(&s)", &id);
      handle = add_point (self->breakpoints, &self->next_handle, id);
      g_dbus_method_invocation_return_value (invocation,
					     g_variant_new ("(u)", handle));
    }
  else if (g_strcmp0 (method_name, "RemoveBreakpoint") == 0)
    {
      guint handle = 0;
      gboolean ok;

      g_variant_get (parameters, "(u)", &handle);
      ok = g_hash_table_remove (self->breakpoints, GUINT_TO_POINTER (handle));
      g_dbus_method_invocation_return_value (invocation,
					     g_variant_new ("(b)", ok));
    }
  else if (g_strcmp0 (method_name, "ListBreakpoints") == 0)
    list_points (self->breakpoints, invocation);
  else if (g_strcmp0 (method_name, "AddWatchpoint") == 0)
    {
      const gchar *id = NULL;
      guint handle;

      g_variant_get (parameters, "(&s)", &id);
      handle = add_point (self->watchpoints, &self->next_handle, id);
      g_dbus_method_invocation_return_value (invocation,
					     g_variant_new ("(u)", handle));
    }
  else if (g_strcmp0 (method_name, "RemoveWatchpoint") == 0)
    {
      guint handle = 0;
      gboolean ok;

      g_variant_get (parameters, "(u)", &handle);
      ok = g_hash_table_remove (self->watchpoints, GUINT_TO_POINTER (handle));
      g_dbus_method_invocation_return_value (invocation,
					     g_variant_new ("(b)", ok));
    }
  else if (g_strcmp0 (method_name, "ListWatchpoints") == 0)
    list_points (self->watchpoints, invocation);
  else if (g_strcmp0 (method_name, "Continue") == 0)
    {
      self->paused = FALSE;
      g_dbus_method_invocation_return_value (invocation, NULL);
    }
  else if (g_strcmp0 (method_name, "GetPaused") == 0)
    g_dbus_method_invocation_return_value (invocation,
					   g_variant_new ("(b)",
							  self->paused));
  else
    g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
					   G_DBUS_ERROR_UNKNOWN_METHOD,
					   "Unknown method %s", method_name);
}

static const GDBusInterfaceVTable interface_vtable = {
  handle_method_call,
  NULL,
  NULL,
};

static void
on_connection_closed (GDBusConnection * connection, gboolean remote_peer_vanished,
		      GError * error, gpointer user_data)
{
  STDbgMockServer *self = user_data;

  (void) remote_peer_vanished;
  (void) error;
  if (self == NULL || self->stopping || self->connections == NULL)
    return;
  g_ptr_array_remove (self->connections, connection);
}

static gboolean
on_new_connection (GDBusServer * server, GDBusConnection * connection,
		   gpointer user_data)
{
  STDbgMockServer *self = user_data;
  guint id;

  (void) server;
  g_object_ref (connection);
  g_ptr_array_add (self->connections, connection);
  g_signal_connect (connection, "closed", G_CALLBACK (on_connection_closed),
		    self);

  id =
    g_dbus_connection_register_object (connection, ST_DBG_OBJECT_PATH,
				       introspection_data->interfaces[0],
				       &interface_vtable, self, NULL, NULL);
  return id != 0;
}

static gpointer
server_thread (gpointer data)
{
  STDbgMockServer *self = data;
  g_autofree gchar *guid = NULL;
  g_autofree gchar *addr = NULL;
  GError *error = NULL;

  g_main_context_push_thread_default (self->context);

  if (introspection_data == NULL)
    introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);

  guid = g_dbus_generate_guid ();
  addr = g_strdup_printf ("tcp:host=%s,port=0", self->host);
  self->server =
    g_dbus_server_new_sync (addr,
			    G_DBUS_SERVER_FLAGS_AUTHENTICATION_ALLOW_ANONYMOUS,
			    guid, NULL, NULL, &error);
  if (self->server == NULL)
    {
      self->start_error = error;
      self->start_ok = FALSE;
    }
  else
    {
      const gchar *client_addr =
	g_dbus_server_get_client_address (self->server);
      const gchar *port_key;

      if (client_addr && (port_key = strstr (client_addr, "port=")) != NULL)
	{
	  unsigned int parsed = 0;

	  if (sscanf (port_key + 5, "%u", &parsed) == 1 && parsed <= 65535)
	    self->port = (guint16) parsed;
	}

      g_signal_connect (self->server, "new-connection",
			G_CALLBACK (on_new_connection), self);
      g_dbus_server_start (self->server);
      self->start_ok = TRUE;
    }

  g_mutex_lock (&self->mutex);
  self->ready = TRUE;
  g_cond_signal (&self->cond);
  g_mutex_unlock (&self->mutex);

  if (self->start_ok)
    g_main_loop_run (self->loop);

  if (self->server)
    {
      g_dbus_server_stop (self->server);
      g_clear_object (&self->server);
    }

  g_main_context_pop_thread_default (self->context);
  return NULL;
}

typedef struct
{
  STDbgMockServer *self;
  gchar *name;
  gchar *address;
  guint port;
  guint state;
} PeerIdle;

static gboolean
idle_add_peer (gpointer data)
{
  PeerIdle *p = data;
  MockPeer *peer;

  peer = g_hash_table_lookup (p->self->peers, p->name);
  if (peer == NULL)
    {
      peer = g_new0 (MockPeer, 1);
      peer->name = g_strdup (p->name);
      g_hash_table_insert (p->self->peers, peer->name, peer);
    }
  g_free (peer->address);
  peer->address = g_strdup (p->address ? p->address : "");
  peer->port = p->port;
  peer->state = p->state;

  broadcast_signal (p->self, "PeerChanged",
		    g_variant_new ("(ssuus)", peer->name, peer->address,
				   peer->port, peer->state, "ready"));

  g_free (p->name);
  g_free (p->address);
  g_free (p);
  return G_SOURCE_REMOVE;
}

typedef struct
{
  STDbgMockServer *self;
  gchar *id;
  gdouble value;
} InputIdle;

static gboolean
idle_emit_input (gpointer data)
{
  InputIdle *p = data;
  GVariant *item;

  mock_io_upsert (p->self->inputs, p->id, "mock", p->value);
  item = g_variant_new ("{s(siddddt)}", p->id, "mock", 0, 0.0, 100.0, 1.0,
			p->value, (guint64) 0);
  broadcast_signal (p->self, "InputChanged", g_variant_new_tuple (&item, 1));

  g_free (p->id);
  g_free (p);
  return G_SOURCE_REMOVE;
}

typedef struct
{
  STDbgMockServer *self;
  gchar *reason;
  gchar *id;
  gdouble value;
  guint handle;
} StoppedIdle;

static gboolean
idle_emit_stopped (gpointer data)
{
  StoppedIdle *p = data;

  p->self->paused = TRUE;
  broadcast_signal (p->self, "Stopped",
		    g_variant_new ("(ssdu)", p->reason, p->id, p->value,
				   p->handle));
  g_free (p->reason);
  g_free (p->id);
  g_free (p);
  return G_SOURCE_REMOVE;
}

STDbgMockServer *
st_dbg_mock_server_start (GError ** error)
{
  STDbgMockServer *self = g_new0 (STDbgMockServer, 1);

  g_mutex_init (&self->mutex);
  g_cond_init (&self->cond);
  self->host = g_strdup ("127.0.0.1");
  self->port = 0;
  self->next_handle = 1;
  self->peers =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, mock_peer_free);
  self->inputs =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, mock_io_free);
  self->outputs =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, mock_io_free);
  self->breakpoints =
    g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, mock_point_free);
  self->watchpoints =
    g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, mock_point_free);
  self->connections = g_ptr_array_new_with_free_func (g_object_unref);

  self->context = g_main_context_new ();
  self->loop = g_main_loop_new (self->context, FALSE);
  self->thread = g_thread_new ("mock-debug-server", server_thread, self);

  g_mutex_lock (&self->mutex);
  while (!self->ready)
    g_cond_wait (&self->cond, &self->mutex);
  g_mutex_unlock (&self->mutex);

  if (!self->start_ok)
    {
      if (error)
	*error = self->start_error ? g_error_copy (self->start_error) : NULL;
      st_dbg_mock_server_stop (self);
      return NULL;
    }

  return self;
}

static gboolean
idle_detach_connections (gpointer data)
{
  STDbgMockServer *self = data;
  guint i;

  if (self->connections)
    {
      for (i = 0; i < self->connections->len; i++)
	{
	  GDBusConnection *conn = self->connections->pdata[i];

	  g_signal_handlers_disconnect_by_data (conn, self);
	}
      g_ptr_array_set_size (self->connections, 0);
    }
  return G_SOURCE_REMOVE;
}

void
st_dbg_mock_server_stop (STDbgMockServer * self)
{
  if (self == NULL)
    return;

  self->stopping = TRUE;

  if (self->context)
    {
      g_main_context_invoke (self->context, idle_detach_connections, self);
      /* Let the idle run before quitting. */
      g_usleep (20 * 1000);
    }

  if (self->loop)
    g_main_loop_quit (self->loop);
  if (self->thread)
    g_thread_join (self->thread);

  g_clear_pointer (&self->loop, g_main_loop_unref);
  g_clear_pointer (&self->context, g_main_context_unref);
  g_clear_pointer (&self->connections, g_ptr_array_unref);
  g_clear_pointer (&self->peers, g_hash_table_unref);
  g_clear_pointer (&self->inputs, g_hash_table_unref);
  g_clear_pointer (&self->outputs, g_hash_table_unref);
  g_clear_pointer (&self->breakpoints, g_hash_table_unref);
  g_clear_pointer (&self->watchpoints, g_hash_table_unref);
  g_clear_error (&self->start_error);
  g_free (self->host);
  g_free (self->last_set_input_id);
  g_free (self->last_set_output_id);
  g_mutex_clear (&self->mutex);
  g_cond_clear (&self->cond);
  g_free (self);
}

guint16
st_dbg_mock_server_get_port (STDbgMockServer * self)
{
  g_return_val_if_fail (self != NULL, 0);
  return self->port;
}

const gchar *
st_dbg_mock_server_get_host (STDbgMockServer * self)
{
  g_return_val_if_fail (self != NULL, NULL);
  return self->host;
}

void
st_dbg_mock_server_add_peer (STDbgMockServer * self, const gchar * name,
			     const gchar * address, guint port, guint state)
{
  PeerIdle *p;

  g_return_if_fail (self != NULL);
  p = g_new0 (PeerIdle, 1);
  p->self = self;
  p->name = g_strdup (name);
  p->address = g_strdup (address ? address : "");
  p->port = port;
  p->state = state;
  g_main_context_invoke (self->context, idle_add_peer, p);
}

typedef struct
{
  STDbgMockServer *self;
  gchar *id;
  gchar *description;
  gdouble value;
  gboolean is_output;
} IoSetIdle;

static gboolean
idle_set_io (gpointer data)
{
  IoSetIdle *p = data;

  mock_io_upsert (p->is_output ? p->self->outputs : p->self->inputs, p->id,
		  p->description, p->value);
  g_free (p->id);
  g_free (p->description);
  g_free (p);
  return G_SOURCE_REMOVE;
}

void
st_dbg_mock_server_set_input (STDbgMockServer * self, const gchar * id,
			      const gchar * description, gdouble value)
{
  IoSetIdle *p;

  g_return_if_fail (self != NULL);
  g_return_if_fail (id != NULL);
  p = g_new0 (IoSetIdle, 1);
  p->self = self;
  p->id = g_strdup (id);
  p->description = g_strdup (description ? description : "");
  p->value = value;
  p->is_output = FALSE;
  g_main_context_invoke (self->context, idle_set_io, p);
}

void
st_dbg_mock_server_set_output (STDbgMockServer * self, const gchar * id,
			       const gchar * description, gdouble value)
{
  IoSetIdle *p;

  g_return_if_fail (self != NULL);
  g_return_if_fail (id != NULL);
  p = g_new0 (IoSetIdle, 1);
  p->self = self;
  p->id = g_strdup (id);
  p->description = g_strdup (description ? description : "");
  p->value = value;
  p->is_output = TRUE;
  g_main_context_invoke (self->context, idle_set_io, p);
}

void
st_dbg_mock_server_emit_input (STDbgMockServer * self, const gchar * id,
			       gdouble value)
{
  InputIdle *p;

  g_return_if_fail (self != NULL);
  p = g_new0 (InputIdle, 1);
  p->self = self;
  p->id = g_strdup (id);
  p->value = value;
  g_main_context_invoke (self->context, idle_emit_input, p);
}

void
st_dbg_mock_server_emit_stopped (STDbgMockServer * self, const gchar * reason,
				 const gchar * id, gdouble value, guint handle)
{
  StoppedIdle *p;

  g_return_if_fail (self != NULL);
  p = g_new0 (StoppedIdle, 1);
  p->self = self;
  p->reason = g_strdup (reason);
  p->id = g_strdup (id);
  p->value = value;
  p->handle = handle;
  g_main_context_invoke (self->context, idle_emit_stopped, p);
}

typedef struct
{
  STDbgMockServer *self;
  gchar *signal_name;
  gchar *dest;
  gchar *describe;
} EngineIdle;

static gboolean
idle_emit_engine (gpointer data)
{
  EngineIdle *p = data;

  broadcast_signal (p->self, p->signal_name,
		    g_variant_new ("(ss)", p->dest,
				   p->describe ? p->describe : ""));
  g_free (p->signal_name);
  g_free (p->dest);
  g_free (p->describe);
  g_free (p);
  return G_SOURCE_REMOVE;
}

void
st_dbg_mock_server_emit_rule_triggered (STDbgMockServer * self,
					const gchar * dest,
					const gchar * describe)
{
  EngineIdle *p;

  g_return_if_fail (self != NULL);
  g_return_if_fail (dest != NULL);
  p = g_new0 (EngineIdle, 1);
  p->self = self;
  p->signal_name = g_strdup ("RuleTriggered");
  p->dest = g_strdup (dest);
  p->describe = g_strdup (describe ? describe : "");
  g_main_context_invoke (self->context, idle_emit_engine, p);
}

void
st_dbg_mock_server_emit_trigger_triggered (STDbgMockServer * self,
					   const gchar * dest,
					   const gchar * describe)
{
  EngineIdle *p;

  g_return_if_fail (self != NULL);
  g_return_if_fail (dest != NULL);
  p = g_new0 (EngineIdle, 1);
  p->self = self;
  p->signal_name = g_strdup ("TriggerTriggered");
  p->dest = g_strdup (dest);
  p->describe = g_strdup (describe ? describe : "");
  g_main_context_invoke (self->context, idle_emit_engine, p);
}

typedef struct
{
  STDbgMockServer *self;
  gchar *id;
  gdouble value;
} OutputIdle;

static gboolean
idle_emit_output (gpointer data)
{
  OutputIdle *p = data;
  GVariant *item;

  mock_io_upsert (p->self->outputs, p->id, "mock", p->value);
  item = g_variant_new ("{s(siddddt)}", p->id, "mock", 0, 0.0, 100.0, 1.0,
			p->value, (guint64) 0);
  broadcast_signal (p->self, "OutputChanged", g_variant_new_tuple (&item, 1));

  g_free (p->id);
  g_free (p);
  return G_SOURCE_REMOVE;
}

void
st_dbg_mock_server_emit_output (STDbgMockServer * self, const gchar * id,
				gdouble value)
{
  OutputIdle *p;

  g_return_if_fail (self != NULL);
  p = g_new0 (OutputIdle, 1);
  p->self = self;
  p->id = g_strdup (id);
  p->value = value;
  g_main_context_invoke (self->context, idle_emit_output, p);
}

gboolean
st_dbg_mock_server_is_paused (STDbgMockServer * self)
{
  g_return_val_if_fail (self != NULL, FALSE);
  return self->paused;
}

guint
st_dbg_mock_server_n_breakpoints (STDbgMockServer * self)
{
  g_return_val_if_fail (self != NULL, 0);
  return g_hash_table_size (self->breakpoints);
}

guint
st_dbg_mock_server_n_watchpoints (STDbgMockServer * self)
{
  g_return_val_if_fail (self != NULL, 0);
  return g_hash_table_size (self->watchpoints);
}

gboolean
st_dbg_mock_server_last_set_input (STDbgMockServer * self, gchar ** id,
				   gdouble * value)
{
  g_return_val_if_fail (self != NULL, FALSE);
  if (self->last_set_input_id == NULL)
    return FALSE;
  if (id)
    *id = g_strdup (self->last_set_input_id);
  if (value)
    *value = self->last_set_input_value;
  return TRUE;
}

gboolean
st_dbg_mock_server_last_set_output (STDbgMockServer * self, gchar ** id,
				    gdouble * value)
{
  g_return_val_if_fail (self != NULL, FALSE);
  if (self->last_set_output_id == NULL)
    return FALSE;
  if (id)
    *id = g_strdup (self->last_set_output_id);
  if (value)
    *value = self->last_set_output_value;
  return TRUE;
}
