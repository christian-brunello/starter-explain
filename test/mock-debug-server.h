/*
 * starter-debugger - mock-debug-server.h
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

#ifndef ST_DBG_MOCK_DEBUG_SERVER_H_INCLUDED
#define ST_DBG_MOCK_DEBUG_SERVER_H_INCLUDED

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _STDbgMockServer STDbgMockServer;

STDbgMockServer *st_dbg_mock_server_start (GError ** error);
void st_dbg_mock_server_stop (STDbgMockServer * self);

guint16 st_dbg_mock_server_get_port (STDbgMockServer * self);
const gchar *st_dbg_mock_server_get_host (STDbgMockServer * self);

/* Test helpers: mutate server-side state / emit signals. */
void st_dbg_mock_server_add_peer (STDbgMockServer * self, const gchar * name,
				  const gchar * address, guint port,
				  guint state);
void st_dbg_mock_server_set_input (STDbgMockServer * self, const gchar * id,
				   const gchar * description, gdouble value);
void st_dbg_mock_server_set_output (STDbgMockServer * self, const gchar * id,
				    const gchar * description, gdouble value);
void st_dbg_mock_server_emit_input (STDbgMockServer * self, const gchar * id,
				    gdouble value);
void st_dbg_mock_server_emit_output (STDbgMockServer * self, const gchar * id,
				     gdouble value);
void st_dbg_mock_server_emit_rule_triggered (STDbgMockServer * self,
					     const gchar * dest,
					     const gchar * describe);
void st_dbg_mock_server_emit_trigger_triggered (STDbgMockServer * self,
						const gchar * dest,
						const gchar * describe);
void st_dbg_mock_server_emit_stopped (STDbgMockServer * self,
				      const gchar * reason, const gchar * id,
				      gdouble value, guint handle);

gboolean st_dbg_mock_server_is_paused (STDbgMockServer * self);
guint st_dbg_mock_server_n_breakpoints (STDbgMockServer * self);
guint st_dbg_mock_server_n_watchpoints (STDbgMockServer * self);
gboolean st_dbg_mock_server_last_set_input (STDbgMockServer * self,
					    gchar ** id, gdouble * value);
gboolean st_dbg_mock_server_last_set_output (STDbgMockServer * self,
					     gchar ** id, gdouble * value);

G_END_DECLS
#endif /* ST_DBG_MOCK_DEBUG_SERVER_H_INCLUDED */
