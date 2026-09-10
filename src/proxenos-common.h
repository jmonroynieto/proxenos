/* Helpers shared by the command line tool, the window, the panel plugin and
   the router.  Each of these used to exist in three slightly different copies. */
#ifndef PROXENOS_COMMON_H
#define PROXENOS_COMMON_H

#include <glib.h>
#include <sys/types.h>

/* The router runs as root, so its pid file lives outside the user's state. */
#define PROXENOS_ROUTER_PID_FILE "/run/proxenos-router.pid"

/* PROXENOS_CONFIG overrides the location of services.conf. */
gchar *proxenos_config_path(void);

/* $XDG_STATE_HOME/proxenos/<service>.<suffix>, creating the directory. */
gchar *proxenos_state_path(const gchar *service, const gchar *suffix);

/* Reads the recorded pid of a service and confirms the process is still the
   one Proxenos started.  Returns FALSE when the service is not running. */
gboolean proxenos_service_pid(const gchar *service, pid_t *pid);
gboolean proxenos_service_is_running(const gchar *service);

/* Records or discards the pid Proxenos started for a service. */
gboolean proxenos_record_service_pid(const gchar *service, pid_t pid, GError **error);
void proxenos_forget_service_pid(const gchar *service);

/* Reads a pid file written by Proxenos and confirms the process is still the
   one it names.  Files hold "<pid> <start time>": the pid alone would match
   whatever later inherits that number. */
gboolean proxenos_pid_from_file(const gchar *path, pid_t *pid);
gboolean proxenos_write_pid_file(const gchar *path, pid_t pid, GError **error);

/* Consults the router's pid file alone: two file reads, cheap enough to call
   from a UI poll. */
gboolean proxenos_router_pid_from_file(pid_t *pid);

/* Falls back to scanning /proc when the pid file is missing, which recovers a
   router whose pid file was lost.  The scan opens every process entry, so keep
   it off short polling intervals. */
gboolean proxenos_router_pid(pid_t *pid);
gboolean proxenos_router_is_running(void);

/* ---- readiness ---------------------------------------------------------

   Whether a service is running is a question about a process; whether it can
   answer is a question about its port.  The port question is settled without
   sending the service anything: the kernel already lists every listening
   socket, so Proxenos reads that list rather than knocking on the door. */

typedef enum {
  PROXENOS_PORT_CLOSED,      /* nothing is listening on the port */
  PROXENOS_PORT_CONFIRMED,   /* the process Proxenos started holds the socket */
  PROXENOS_PORT_ASSUMED,     /* one of your processes holds it, most likely a
                                child of the service, such as a wrapper's */
  PROXENOS_PORT_FOREIGN,     /* another user's process holds it: a collision */
} ProxenosPortState;

typedef enum {
  PROXENOS_SERVICE_STOPPED,
  PROXENOS_SERVICE_RUNNING,     /* alive, and no port is declared to check */
  PROXENOS_SERVICE_STARTING,    /* alive, but its port is not open yet */
  PROXENOS_SERVICE_READY,       /* alive and holding its port */
  PROXENOS_SERVICE_PORT_TAKEN,  /* alive, but its port belongs to someone else */
} ProxenosServiceState;

/* Reads /proc/net/tcp and /proc/net/tcp6.  A service may bind IPv4, IPv6 or
   both, so both tables are always consulted.  These one-shot calls take their
   own snapshot and suit a program asking about a single service. */
ProxenosPortState proxenos_port_state(guint16 port, pid_t owner);
ProxenosServiceState proxenos_service_state(const gchar *service, gint port);
const gchar *proxenos_service_state_text(ProxenosServiceState state);

/* The kernel builds each table by walking every socket on the machine, which
   costs about a millisecond however few services you have.  A caller asking
   about several services at once takes one snapshot and asks it repeatedly,
   so the cost stops depending on how many services are registered. */
typedef struct ProxenosListeners ProxenosListeners;

ProxenosListeners *proxenos_listeners_snapshot(void);
void proxenos_listeners_free(ProxenosListeners *listeners);
ProxenosPortState proxenos_listeners_port_state(const ProxenosListeners *listeners, guint16 port, pid_t owner);
ProxenosServiceState proxenos_listeners_service_state(const ProxenosListeners *listeners,
                                                      const gchar *service, gint port);

/* Exposed so the tests can drive the parser with captured table contents.
   `contents` is one /proc/net/tcp-style table. */
gboolean proxenos_find_listener(const gchar *contents, guint16 port, guint64 *inode, uid_t *owner);

/* The result of a configured `ready_command`, remembered next to the pid file
   so that every part of Proxenos can see what the last check found without
   running the command again. */
typedef enum {
  PROXENOS_CHECK_UNKNOWN,
  PROXENOS_CHECK_PASSED,
  PROXENOS_CHECK_FAILED,
} ProxenosCheckResult;

ProxenosCheckResult proxenos_read_check_result(const gchar *service, gchar **detail);
void proxenos_write_check_result(const gchar *service, ProxenosCheckResult result, const gchar *detail);
void proxenos_forget_check_result(const gchar *service);

/* Appends a timestamped line to the diagnostic log.  Call configure() once at
   startup; a NULL path selects PROXENOS_DEBUG_LOG or the default location. */
void proxenos_debug_log_configure(const gchar *component, const gchar *path);
gchar *proxenos_debug_log_path(void);
void proxenos_debug_log(const gchar *format, ...) G_GNUC_PRINTF(1, 2);

#endif
