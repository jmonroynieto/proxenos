#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "proxenos-common.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static gchar *debug_path = NULL;
static const gchar *debug_component = "proxenos";

gchar *proxenos_config_path(void) {
  const gchar *override = g_getenv("PROXENOS_CONFIG");
  if (override && *override)
    return g_strdup(override);
  return g_build_filename(g_get_user_config_dir(), "proxenos", "services.conf", NULL);
}

gchar *proxenos_state_path(const gchar *service, const gchar *suffix) {
  gchar *directory = g_build_filename(g_get_user_state_dir(), "proxenos", NULL);
  g_mkdir_with_parents(directory, 0700);

  gchar *leaf = g_strdup_printf("%s.%s", service, suffix);
  gchar *path = g_build_filename(directory, leaf, NULL);

  g_free(leaf);
  g_free(directory);
  return path;
}

/* Fields 3 and 22 of /proc/<pid>/stat: the process state, and the moment the
   kernel started it.  A pid on its own is not an identity, because the kernel
   reuses pids; the pair of pid and start time is one.  The command name in
   field 2 may itself contain spaces and brackets, so the fields are counted
   from the last ')'. */
static gboolean process_details(pid_t pid, guint64 *start_time, gchar *state) {
  gchar *path = g_strdup_printf("/proc/%d/stat", (int)pid);
  gchar *contents = NULL;
  gboolean read = g_file_get_contents(path, &contents, NULL, NULL);
  g_free(path);
  if (!read)
    return FALSE;

  gboolean found = FALSE;
  gchar *cursor = strrchr(contents, ')');
  if (cursor) {
    /* The token after ')' is field 3, so field 22 is nineteen tokens later. */
    for (int field = 3; *cursor && field <= 22; field++) {
      while (*cursor && *cursor != ' ') cursor++;
      while (*cursor == ' ') cursor++;
      if (field == 3 && *cursor && state)
        *state = *cursor;
      if (field == 22 && *cursor) {
        if (start_time)
          *start_time = g_ascii_strtoull(cursor, NULL, 10);
        found = TRUE;
        break;
      }
    }
  }

  g_free(contents);
  return found;
}

static gboolean process_is_alive(pid_t pid, guint64 expected_start, gboolean have_start) {
  if (pid < 2 || kill(pid, 0) != 0)
    return FALSE;

  guint64 actual = 0;
  gchar state = 0;
  if (!process_details(pid, &actual, &state))
    return !have_start;  /* Recorded by an older Proxenos: the pid is all we have. */

  /* A process that has exited but whose parent has not collected it is a
     zombie.  It still answers kill(pid, 0), and it is not a running service. */
  if (state == 'Z')
    return FALSE;

  return !have_start || actual == expected_start;
}

/* The pid file holds "<pid> <start time>".  Readers that predate the start
   time simply stop at the space, so the two formats interoperate. */
static gboolean read_pid_file(const gchar *path, pid_t *pid, guint64 *start_time, gboolean *have_start) {
  gchar *contents = NULL;
  if (!g_file_get_contents(path, &contents, NULL, NULL))
    return FALSE;

  gchar *end = NULL;
  gint64 parsed = g_ascii_strtoll(contents, &end, 10);
  gboolean valid = end != contents && parsed >= 2 && parsed <= G_MAXINT;
  if (valid) {
    *pid = (pid_t)parsed;
    *have_start = FALSE;
    while (*end == ' ' || *end == '\t') end++;
    if (g_ascii_isdigit(*end)) {
      *start_time = g_ascii_strtoull(end, NULL, 10);
      *have_start = TRUE;
    }
  }

  g_free(contents);
  return valid;
}

gboolean proxenos_service_pid(const gchar *service, pid_t *pid) {
  gchar *path = proxenos_state_path(service, "pid");
  pid_t recorded = 0;
  guint64 start_time = 0;
  gboolean have_start = FALSE;
  gboolean found = read_pid_file(path, &recorded, &start_time, &have_start);
  g_free(path);

  if (!found || !process_is_alive(recorded, start_time, have_start))
    return FALSE;
  if (pid)
    *pid = recorded;
  return TRUE;
}

gboolean proxenos_service_is_running(const gchar *service) {
  return proxenos_service_pid(service, NULL);
}

gboolean proxenos_write_pid_file(const gchar *path, pid_t pid, GError **error) {
  guint64 start_time = 0;
  gchar *contents = process_details(pid, &start_time, NULL)
      ? g_strdup_printf("%d %" G_GUINT64_FORMAT "\n", (int)pid, start_time)
      : g_strdup_printf("%d\n", (int)pid);

  gboolean saved = g_file_set_contents(path, contents, -1, error);
  g_free(contents);
  return saved;
}

gboolean proxenos_record_service_pid(const gchar *service, pid_t pid, GError **error) {
  gchar *path = proxenos_state_path(service, "pid");
  gboolean saved = proxenos_write_pid_file(path, pid, error);
  g_free(path);
  return saved;
}

void proxenos_forget_service_pid(const gchar *service) {
  gchar *path = proxenos_state_path(service, "pid");
  g_unlink(path);
  g_free(path);
}

static gboolean router_pid_from_proc(pid_t *pid) {
  GDir *proc = g_dir_open("/proc", 0, NULL);
  if (!proc)
    return FALSE;

  const gchar *entry;
  gboolean found = FALSE;
  while (!found && (entry = g_dir_read_name(proc))) {
    if (!g_ascii_isdigit(entry[0]))
      continue;

    gchar *path = g_build_filename("/proc", entry, "comm", NULL);
    gchar *name = NULL;
    if (g_file_get_contents(path, &name, NULL, NULL) && g_strcmp0(g_strstrip(name), "proxenos-router") == 0) {
      if (pid)
        *pid = (pid_t)g_ascii_strtoll(entry, NULL, 10);
      found = TRUE;
    }
    g_free(name);
    g_free(path);
  }

  g_dir_close(proc);
  return found;
}

gboolean proxenos_pid_from_file(const gchar *path, pid_t *pid) {
  pid_t recorded = 0;
  guint64 start_time = 0;
  gboolean have_start = FALSE;
  if (!read_pid_file(path, &recorded, &start_time, &have_start)
      || !process_is_alive(recorded, start_time, have_start))
    return FALSE;
  if (pid)
    *pid = recorded;
  return TRUE;
}

gboolean proxenos_router_pid_from_file(pid_t *pid) {
  return proxenos_pid_from_file(PROXENOS_ROUTER_PID_FILE, pid);
}

gboolean proxenos_router_pid(pid_t *pid) {
  return proxenos_router_pid_from_file(pid) || router_pid_from_proc(pid);
}

gboolean proxenos_router_is_running(void) {
  return proxenos_router_pid(NULL);
}

/* ---- readiness --------------------------------------------------------- */

#define TCP_STATE_LISTEN "0A"

/* One row of /proc/net/tcp looks like

     sl  local_address rem_address st ... uid ... inode
      0: 0100007F:2CAA 00000000:0000 0A ...  1000 ...  1120475

   The local address is a hexadecimal address and port; the port is all this
   needs, because both tables are searched and a wildcard bind answers on the
   loopback address anyway. */
gboolean proxenos_find_listener(const gchar *contents, guint16 port, guint64 *inode, uid_t *owner) {
  /* Read in place.  These tables are proportional to every socket on the
     machine, and this runs on the window's refresh, so the parse allocates
     nothing and touches each line once. */
  for (const gchar *line = contents; *line; ) {
    const gchar *end = strchr(line, '\n');
    if (!end)
      end = line + strlen(line);

    const gchar *token[10] = {0};
    gsize count = 0;
    for (const gchar *cursor = line; cursor < end && count < G_N_ELEMENTS(token); ) {
      while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
        cursor++;
      if (cursor >= end)
        break;
      token[count++] = cursor;
      while (cursor < end && *cursor != ' ' && *cursor != '\t')
        cursor++;
    }

    if (count == G_N_ELEMENTS(token)
        && token[3][0] == '0' && token[3][1] == 'A') {
      const gchar *colon = strchr(token[1], ':');
      if (colon && (guint16)g_ascii_strtoull(colon + 1, NULL, 16) == port) {
        if (inode)
          *inode = g_ascii_strtoull(token[9], NULL, 10);
        if (owner)
          *owner = (uid_t)g_ascii_strtoull(token[7], NULL, 10);
        return TRUE;
      }
    }

    line = *end ? end + 1 : end;
  }
  return FALSE;
}

/* A listening socket appears in its process's file descriptor table as
   "socket:[<inode>]", which is how a port is tied back to the process
   Proxenos started rather than merely to the machine. */
static gboolean pid_owns_inode(pid_t pid, guint64 inode) {
  gchar *directory = g_strdup_printf("/proc/%d/fd", (int)pid);
  GDir *fds = g_dir_open(directory, 0, NULL);
  if (!fds) {
    g_free(directory);
    return FALSE;
  }

  gchar *wanted = g_strdup_printf("socket:[%" G_GUINT64_FORMAT "]", inode);
  const gchar *entry;
  gboolean found = FALSE;

  while (!found && (entry = g_dir_read_name(fds))) {
    gchar *path = g_build_filename(directory, entry, NULL);
    gchar *target = g_file_read_link(path, NULL);
    found = g_strcmp0(target, wanted) == 0;
    g_free(target);
    g_free(path);
  }

  g_dir_close(fds);
  g_free(wanted);
  g_free(directory);
  return found;
}

struct ProxenosListeners {
  gchar *table[2];
};

/* A service may bind IPv4, IPv6 or both.  Forgejo binds the IPv6 wildcard and
   never appears in the IPv4 table, so reading one table is not enough. */
ProxenosListeners *proxenos_listeners_snapshot(void) {
  static const gchar *paths[] = { "/proc/net/tcp", "/proc/net/tcp6" };
  ProxenosListeners *listeners = g_new0(ProxenosListeners, 1);

  for (gsize i = 0; i < G_N_ELEMENTS(paths); i++)
    if (!g_file_get_contents(paths[i], &listeners->table[i], NULL, NULL))
      listeners->table[i] = NULL;

  return listeners;
}

void proxenos_listeners_free(ProxenosListeners *listeners) {
  if (!listeners)
    return;
  for (gsize i = 0; i < G_N_ELEMENTS(listeners->table); i++)
    g_free(listeners->table[i]);
  g_free(listeners);
}

ProxenosPortState proxenos_listeners_port_state(const ProxenosListeners *listeners,
                                                guint16 port, pid_t owner) {
  for (gsize i = 0; i < G_N_ELEMENTS(listeners->table); i++) {
    guint64 inode = 0;
    uid_t socket_owner = 0;
    if (!listeners->table[i] || !proxenos_find_listener(listeners->table[i], port, &inode, &socket_owner))
      continue;

    if (owner > 1 && pid_owns_inode(owner, inode))
      return PROXENOS_PORT_CONFIRMED;
    return socket_owner == getuid() ? PROXENOS_PORT_ASSUMED : PROXENOS_PORT_FOREIGN;
  }
  return PROXENOS_PORT_CLOSED;
}

ProxenosServiceState proxenos_listeners_service_state(const ProxenosListeners *listeners,
                                                      const gchar *service, gint port) {
  pid_t pid = 0;
  if (!proxenos_service_pid(service, &pid))
    return PROXENOS_SERVICE_STOPPED;
  if (port <= 0 || port > 65535)
    return PROXENOS_SERVICE_RUNNING;

  switch (proxenos_listeners_port_state(listeners, (guint16)port, pid)) {
    case PROXENOS_PORT_CONFIRMED:
    case PROXENOS_PORT_ASSUMED:
      return PROXENOS_SERVICE_READY;
    case PROXENOS_PORT_FOREIGN:
      return PROXENOS_SERVICE_PORT_TAKEN;
    default:
      return PROXENOS_SERVICE_STARTING;
  }
}

ProxenosPortState proxenos_port_state(guint16 port, pid_t owner) {
  ProxenosListeners *listeners = proxenos_listeners_snapshot();
  ProxenosPortState state = proxenos_listeners_port_state(listeners, port, owner);
  proxenos_listeners_free(listeners);
  return state;
}

ProxenosServiceState proxenos_service_state(const gchar *service, gint port) {
  ProxenosListeners *listeners = proxenos_listeners_snapshot();
  ProxenosServiceState state = proxenos_listeners_service_state(listeners, service, port);
  proxenos_listeners_free(listeners);
  return state;
}

const gchar *proxenos_service_state_text(ProxenosServiceState state) {
  switch (state) {
    case PROXENOS_SERVICE_READY:      return "ready";
    case PROXENOS_SERVICE_STARTING:   return "starting";
    case PROXENOS_SERVICE_RUNNING:    return "running";
    case PROXENOS_SERVICE_PORT_TAKEN: return "port taken";
    default:                          return "stopped";
  }
}

ProxenosCheckResult proxenos_read_check_result(const gchar *service, gchar **detail) {
  gchar *path = proxenos_state_path(service, "ready");
  GKeyFile *file = g_key_file_new();
  ProxenosCheckResult result = PROXENOS_CHECK_UNKNOWN;

  if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL)) {
    gchar *state = g_key_file_get_string(file, "readiness", "state", NULL);
    if (g_strcmp0(state, "passed") == 0)
      result = PROXENOS_CHECK_PASSED;
    else if (g_strcmp0(state, "failed") == 0)
      result = PROXENOS_CHECK_FAILED;
    if (detail)
      *detail = g_key_file_get_string(file, "readiness", "detail", NULL);
    g_free(state);
  }

  g_key_file_unref(file);
  g_free(path);
  return result;
}

void proxenos_write_check_result(const gchar *service, ProxenosCheckResult result, const gchar *detail) {
  GKeyFile *file = g_key_file_new();
  g_key_file_set_string(file, "readiness", "state",
                        result == PROXENOS_CHECK_PASSED ? "passed"
                        : result == PROXENOS_CHECK_FAILED ? "failed" : "unknown");
  g_key_file_set_int64(file, "readiness", "checked", g_get_real_time() / G_USEC_PER_SEC);
  if (detail && *detail)
    g_key_file_set_string(file, "readiness", "detail", detail);

  gchar *path = proxenos_state_path(service, "ready");
  g_key_file_save_to_file(file, path, NULL);
  g_free(path);
  g_key_file_unref(file);
}

void proxenos_forget_check_result(const gchar *service) {
  gchar *path = proxenos_state_path(service, "ready");
  g_unlink(path);
  g_free(path);
}

gchar *proxenos_debug_log_path(void) {
  if (debug_path)
    return g_strdup(debug_path);

  const gchar *override = g_getenv("PROXENOS_DEBUG_LOG");
  if (override && *override)
    return g_strdup(override);

  gchar *directory = g_build_filename(g_get_user_state_dir(), "proxenos", NULL);
  g_mkdir_with_parents(directory, 0700);
  gchar *path = g_build_filename(directory, "debug.log", NULL);
  g_free(directory);
  return path;
}

void proxenos_debug_log_configure(const gchar *component, const gchar *path) {
  if (component)
    debug_component = component;
  if (path && *path) {
    g_free(debug_path);
    debug_path = g_strdup(path);
  }
}

void proxenos_debug_log(const gchar *format, ...) {
  gchar *path = proxenos_debug_log_path();
  FILE *file = fopen(path, "a");
  g_free(path);
  if (!file)
    return;

  time_t now = time(NULL);
  struct tm local;
  localtime_r(&now, &local);
  fprintf(file, "%04d-%02d-%02d %02d:%02d:%02d %s: ",
          local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
          local.tm_hour, local.tm_min, local.tm_sec, debug_component);

  va_list args;
  va_start(args, format);
  vfprintf(file, format, args);
  va_end(args);

  fputc('\n', file);
  fclose(file);
}
