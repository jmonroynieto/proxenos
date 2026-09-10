/* proxenos-cli: registers nothing, starts and stops what services.conf
   already registered.  A launch means the process was started; version 0.1
   makes no claim that the service is ready to answer. */
#define _GNU_SOURCE
#include "proxenos-common.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_STOP_TIMEOUT_SECONDS 10
#define DEFAULT_READY_TIMEOUT_SECONDS 30
#define STOP_POLL_INTERVAL_MS 100
#define READY_POLL_INTERVAL_MS 500

typedef struct {
  gchar *name;
  gchar *command;
  gchar *directory;
  gchar *stop_signal;
  gchar **environment;
  gchar *ready_command;
  gint port;
  gint stop_timeout;
  gint ready_timeout;
} Service;

static void service_clear(Service *service) {
  g_free(service->name);
  g_free(service->command);
  g_free(service->directory);
  g_free(service->stop_signal);
  g_free(service->ready_command);
  g_strfreev(service->environment);
  memset(service, 0, sizeof *service);
}

/* `backend` is an explicit field so that a Pitchfork or Proxenos-owned
   backend can arrive later without rewriting anyone's service file.  Only
   `direct` is implemented, and an unknown value is refused rather than
   silently treated as direct. */
static gboolean service_load(GKeyFile *config, const gchar *name, Service *service, GError **error) {
  memset(service, 0, sizeof *service);

  if (!g_key_file_has_group(config, name)) {
    g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND, "No service named '%s'", name);
    return FALSE;
  }

  gchar *backend = g_key_file_get_string(config, name, "backend", NULL);
  gboolean supported = !backend || g_strcmp0(backend, "direct") == 0;
  if (!supported)
    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                "Service '%s' uses unsupported backend '%s'", name, backend);
  g_free(backend);
  if (!supported)
    return FALSE;

  service->command = g_key_file_get_string(config, name, "command", error);
  if (!service->command)
    return FALSE;

  service->name = g_strdup(name);
  service->directory = g_key_file_get_string(config, name, "working_directory", NULL);
  service->stop_signal = g_key_file_get_string(config, name, "stop_signal", NULL);
  if (!service->stop_signal)
    service->stop_signal = g_strdup("TERM");

  service->stop_timeout = g_key_file_get_integer(config, name, "stop_timeout_seconds", NULL);
  if (service->stop_timeout <= 0)
    service->stop_timeout = DEFAULT_STOP_TIMEOUT_SECONDS;

  service->environment = g_key_file_get_string_list(config, name, "environment", NULL, NULL);
  service->port = g_key_file_get_integer(config, name, "port", NULL);

  /* `ready` is explicit for the same reason `backend` is: the check a service
     needs is a property of the service, not something to be guessed.  With a
     port declared and no `ready` line, the free port check applies. */
  gchar *ready = g_key_file_get_string(config, name, "ready", NULL);
  service->ready_command = g_key_file_get_string(config, name, "ready_command", NULL);
  if (g_strcmp0(ready, "none") == 0) {
    service->port = 0;
    g_clear_pointer(&service->ready_command, g_free);
  } else if (g_strcmp0(ready, "port") == 0) {
    g_clear_pointer(&service->ready_command, g_free);
  }
  g_free(ready);

  service->ready_timeout = g_key_file_get_integer(config, name, "ready_timeout_seconds", NULL);
  if (service->ready_timeout <= 0)
    service->ready_timeout = DEFAULT_READY_TIMEOUT_SECONDS;
  return TRUE;
}

/* The registered `environment=` entries laid over the caller's environment.
   Built before the fork: between fork and exec, only async-signal-safe calls
   are allowed, and GLib's allocator is not one of them. */
static gchar **service_environment(const Service *service) {
  gchar **environment = g_get_environ();
  for (gchar **entry = service->environment; entry && *entry; entry++) {
    gchar **pair = g_strsplit(*entry, "=", 2);
    if (pair[0] && pair[0][0] && pair[1])
      environment = g_environ_setenv(environment, pair[0], pair[1], TRUE);
    g_strfreev(pair);
  }
  return environment;
}

/* Failures between fork and exec have nowhere to print: stdout and stderr
   already belong to the service log by then.  The child sends its errno down
   a close-on-exec pipe instead, which the successful exec closes silently. */
static pid_t spawn_service(const Service *service, gchar **argv, const gchar *log_path, int *child_errno) {
  int report[2];
  if (pipe2(report, O_CLOEXEC) != 0)
    return -1;

  gchar **environment = service_environment(service);
  pid_t pid = fork();
  if (pid < 0) {
    int saved = errno;
    close(report[0]);
    close(report[1]);
    g_strfreev(environment);
    errno = saved;
    return -1;
  }

  if (pid == 0) {
    close(report[0]);
    setsid();
    int failure = 0;
    int log = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (log < 0)
      failure = errno;
    else if (service->directory && chdir(service->directory) != 0)
      failure = errno;
    if (!failure) {
      dup2(log, STDOUT_FILENO);
      dup2(log, STDERR_FILENO);
      if (log > STDERR_FILENO)
        close(log);
      execvpe(argv[0], argv, environment);
      failure = errno;
    }
    ssize_t ignored = write(report[1], &failure, sizeof failure);
    (void)ignored;
    _exit(failure == ENOENT ? 127 : 126);
  }

  close(report[1]);
  int failure = 0;
  gboolean reported = read(report[0], &failure, sizeof failure) == (ssize_t)sizeof failure;
  close(report[0]);
  g_strfreev(environment);

  if (reported) {
    /* The child is already gone; collect it so it does not linger as a zombie. */
    waitpid(pid, NULL, 0);
    *child_errno = failure;
    return -1;
  }
  *child_errno = 0;
  return pid;
}

static int service_up(const Service *service) {
  pid_t existing;
  if (proxenos_service_pid(service->name, &existing)) {
    g_printerr("%s is already running (pid %d)\n", service->name, (int)existing);
    return 1;
  }
  proxenos_forget_service_pid(service->name);
  proxenos_forget_check_result(service->name);

  /* The command is parsed as an argv command and never handed to a shell.
     Use a small script as the command when shell features are needed. */
  gchar **argv = NULL;
  GError *error = NULL;
  if (!g_shell_parse_argv(service->command, NULL, &argv, &error)) {
    g_printerr("Invalid command for %s: %s\n", service->name, error->message);
    g_error_free(error);
    return 1;
  }

  gchar *log_path = proxenos_state_path(service->name, "log");
  int child_errno = 0;
  pid_t pid = spawn_service(service, argv, log_path, &child_errno);
  g_strfreev(argv);

  int result = 1;
  if (pid < 0) {
    g_printerr("Cannot start %s: %s\n", service->name, g_strerror(child_errno ? child_errno : errno));
  } else if (!proxenos_record_service_pid(service->name, pid, &error)) {
    g_printerr("%s started (pid %d) but its pid could not be recorded: %s\n",
               service->name, (int)pid, error->message);
    g_clear_error(&error);
  } else {
    g_print("%s started (pid %d)\nlog: %s\n", service->name, (int)pid, log_path);
    result = 0;
  }

  g_free(log_path);
  return result;
}

/* Runs the configured check once.  The contract is the shell's: exit status
   zero means the service is ready.  Anything can be a check that way — curl, a
   script, a test on a socket file — without Proxenos knowing the protocol. */
static gboolean run_ready_command(const Service *service, gchar **detail) {
  gchar **argv = NULL;
  GError *error = NULL;
  if (!g_shell_parse_argv(service->ready_command, NULL, &argv, &error)) {
    *detail = g_strdup_printf("ready_command could not be parsed: %s", error->message);
    g_error_free(error);
    return FALSE;
  }

  /* The check's own output is captured rather than printed: during a wait it
     would otherwise repeat once a second, and its first line of complaint is
     more useful recorded beside the result than scrolling past. */
  gint status = 0;
  gchar *complaint = NULL;
  gboolean ran = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                              NULL, &complaint, &status, &error);
  g_strfreev(argv);

  if (!ran) {
    *detail = g_strdup_printf("ready_command could not be run: %s", error->message);
    g_error_free(error);
    g_free(complaint);
    return FALSE;
  }

  gboolean passed = g_spawn_check_wait_status(status, NULL);
  if (!passed) {
    gchar *first_line = complaint ? g_strstrip(g_strdelimit(complaint, "\n", '\0')) : NULL;
    *detail = first_line && *first_line
        ? g_strdup_printf("ready_command exited with status %d: %s", WEXITSTATUS(status), first_line)
        : g_strdup_printf("ready_command exited with status %d", WEXITSTATUS(status));
  }

  g_free(complaint);
  return passed;
}

/* What a service's readiness is at this instant, without waiting for it.  The
   port check is free enough to run on sight; the command check is not, so its
   last recorded result is read rather than re-run. */
static ProxenosServiceState observed_state(const Service *service) {
  return proxenos_service_state(service->name, service->port);
}

static int report_readiness(const Service *service) {
  ProxenosServiceState state = observed_state(service);
  gchar *detail = NULL;
  ProxenosCheckResult recorded = service->ready_command
      ? proxenos_read_check_result(service->name, &detail)
      : PROXENOS_CHECK_UNKNOWN;

  if (state == PROXENOS_SERVICE_READY && recorded == PROXENOS_CHECK_FAILED) {
    g_print("%s is listening but its check failed: %s\n", service->name,
            detail ? detail : "no detail recorded");
    g_free(detail);
    return 3;
  }

  g_print("%s is %s\n", service->name, proxenos_service_state_text(state));
  g_free(detail);
  return state == PROXENOS_SERVICE_READY || state == PROXENOS_SERVICE_RUNNING ? 0 : 3;
}

/* Waits for the port to open, then runs the command check if one is
   configured.  This is the only place a command check runs on a schedule:
   "did it come up?" is the question `up --wait` is already asking. */
static int wait_until_ready(const Service *service) {
  gint64 deadline = g_get_monotonic_time() + (gint64)service->ready_timeout * G_USEC_PER_SEC;
  gchar *detail = NULL;
  pid_t pid = 0;
  proxenos_service_pid(service->name, &pid);

  while (g_get_monotonic_time() < deadline) {
    /* While waiting, proxenos-cli is still the service's parent, so it can
       collect an exit status the moment the service gives one — and must, or
       the exited service would linger as a zombie and look alive. */
    int status = 0;
    pid_t exited = pid > 0 ? waitpid(pid, &status, WNOHANG) : 0;
    if (exited > 0 || !proxenos_service_is_running(service->name)) {
      proxenos_forget_service_pid(service->name);
      if (exited > 0 && WIFEXITED(status))
        g_printerr("%s exited with status %d before it became ready; see its log\n",
                   service->name, WEXITSTATUS(status));
      else if (exited > 0 && WIFSIGNALED(status))
        g_printerr("%s was killed by signal %d before it became ready; see its log\n",
                   service->name, WTERMSIG(status));
      else
        g_printerr("%s exited before it became ready; see its log\n", service->name);
      return 4;
    }

    ProxenosServiceState state = observed_state(service);
    if (state == PROXENOS_SERVICE_PORT_TAKEN) {
      g_printerr("%s cannot use port %d: another user's process is listening on it\n",
                 service->name, service->port);
      return 4;
    }

    if (state == PROXENOS_SERVICE_READY || state == PROXENOS_SERVICE_RUNNING) {
      if (!service->ready_command) {
        g_print("%s is ready\n", service->name);
        return 0;
      }
      g_clear_pointer(&detail, g_free);
      if (run_ready_command(service, &detail)) {
        proxenos_write_check_result(service->name, PROXENOS_CHECK_PASSED, NULL);
        g_print("%s is ready\n", service->name);
        return 0;
      }
    }

    struct timespec pause = { .tv_sec = 0, .tv_nsec = READY_POLL_INTERVAL_MS * 1000000L };
    nanosleep(&pause, NULL);
  }

  proxenos_write_check_result(service->name, PROXENOS_CHECK_FAILED,
                              detail ? detail : "the port did not open in time");
  g_printerr("%s did not become ready within %d seconds%s%s\n", service->name,
             service->ready_timeout, detail ? ": " : "", detail ? detail : "");
  g_free(detail);
  return 4;
}

static int check_readiness(const Service *service) {
  if (!proxenos_service_is_running(service->name)) {
    g_print("%s is stopped\n", service->name);
    return 3;
  }
  if (!service->ready_command)
    return report_readiness(service);

  gchar *detail = NULL;
  gboolean passed = run_ready_command(service, &detail);
  proxenos_write_check_result(service->name, passed ? PROXENOS_CHECK_PASSED : PROXENOS_CHECK_FAILED, detail);

  if (passed)
    g_print("%s is ready\n", service->name);
  else
    g_print("%s is not ready: %s\n", service->name, detail);

  g_free(detail);
  return passed ? 0 : 3;
}

static const struct { const gchar *name; int number; } stop_signals[] = {
  { "TERM", SIGTERM }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "HUP", SIGHUP },
  { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 },
};

static int signal_from_name(const gchar *name) {
  const gchar *bare = g_str_has_prefix(name, "SIG") ? name + 3 : name;
  for (gsize i = 0; i < G_N_ELEMENTS(stop_signals); i++)
    if (g_ascii_strcasecmp(bare, stop_signals[i].name) == 0)
      return stop_signals[i].number;

  g_printerr("Unknown stop_signal '%s'; using TERM\n", name);
  return SIGTERM;
}

static int service_down(const Service *service) {
  pid_t pid;
  if (!proxenos_service_pid(service->name, &pid)) {
    proxenos_forget_service_pid(service->name);
    proxenos_forget_check_result(service->name);
    g_print("%s is not running\n", service->name);
    return 0;
  }

  kill(pid, signal_from_name(service->stop_signal));

  int attempts = service->stop_timeout * (1000 / STOP_POLL_INTERVAL_MS);
  for (int attempt = 0; attempt < attempts; attempt++) {
    if (kill(pid, 0) != 0) {
      proxenos_forget_service_pid(service->name);
      proxenos_forget_check_result(service->name);
      g_print("%s stopped\n", service->name);
      return 0;
    }
    struct timespec pause = { .tv_sec = 0, .tv_nsec = STOP_POLL_INTERVAL_MS * 1000000L };
    nanosleep(&pause, NULL);
  }

  g_printerr("%s did not stop after %d seconds; sending KILL\n", service->name, service->stop_timeout);
  kill(pid, SIGKILL);
  proxenos_forget_service_pid(service->name);
  proxenos_forget_check_result(service->name);
  return 0;
}

static int service_status(const Service *service) {
  return report_readiness(service);
}

static int list_services(GKeyFile *config) {
  gsize count = 0;
  gchar **names = g_key_file_get_groups(config, &count);

  /* One snapshot answers for every service, so listing ten costs what listing
     one costs. */
  ProxenosListeners *listeners = proxenos_listeners_snapshot();

  for (gsize i = 0; i < count; i++) {
    gchar *hostname = g_key_file_get_string(config, names[i], "hostname", NULL);
    gint port = g_key_file_get_integer(config, names[i], "port", NULL);

    gchar *endpoint = NULL;
    if (hostname && port > 0)
      endpoint = g_strdup_printf("%s -> 127.0.0.1:%d", hostname, port);
    else if (hostname)
      endpoint = g_strdup(hostname);
    else if (port > 0)
      endpoint = g_strdup_printf("127.0.0.1:%d", port);

    ProxenosServiceState state = proxenos_listeners_service_state(listeners, names[i], port);
    g_print("%-20s %-11s %s\n", names[i], proxenos_service_state_text(state),
            endpoint ? endpoint : "");

    g_free(endpoint);
    g_free(hostname);
  }

  proxenos_listeners_free(listeners);
  g_strfreev(names);
  return 0;
}

static const gchar *usage =
    "Usage: proxenos-cli up [--wait] SERVICE\n"
    "       proxenos-cli down|status|ready SERVICE\n"
    "       proxenos-cli list\n"
    "\n"
    "  up --wait  waits for the service to become ready before returning\n"
    "  ready      runs the service's configured check once and records it\n"
    "\n"
    "Exit status: 0 ready or running, 3 stopped or not ready, 4 started but\n"
    "never became ready, 2 usage error, 1 failure.\n";

int main(int argc, char **argv) {
  if (argc < 2) {
    g_printerr("%s", usage);
    return 2;
  }

  const gchar *verb = argv[1];
  gboolean wait_for_ready = FALSE;
  const gchar *name = NULL;

  for (int i = 2; i < argc; i++) {
    if (g_strcmp0(argv[i], "--wait") == 0)
      wait_for_ready = TRUE;
    else if (!name)
      name = argv[i];
    else
      name = NULL, i = argc;              /* more than one service named */
  }

  gboolean takes_service = g_strcmp0(verb, "list") != 0;
  if ((takes_service && !name) || (!takes_service && name)) {
    g_printerr("%s", usage);
    return 2;
  }
  if (wait_for_ready && g_strcmp0(verb, "up") != 0) {
    g_printerr("--wait applies only to up\n%s", usage);
    return 2;
  }

  gchar *path = proxenos_config_path();
  GKeyFile *config = g_key_file_new();
  GError *error = NULL;
  if (!g_key_file_load_from_file(config, path, G_KEY_FILE_NONE, &error)) {
    g_printerr("Cannot load %s: %s\n", path, error->message);
    g_error_free(error);
    g_free(path);
    g_key_file_unref(config);
    return 1;
  }
  g_free(path);

  int result;
  if (!takes_service) {
    result = list_services(config);
  } else {
    Service service;
    if (!service_load(config, name, &service, &error)) {
      g_printerr("%s\n", error->message);
      g_error_free(error);
      g_key_file_unref(config);
      return 1;
    }

    if (g_strcmp0(verb, "up") == 0) {
      result = service_up(&service);
      if (result == 0 && wait_for_ready)
        result = wait_until_ready(&service);
    }
    else if (g_strcmp0(verb, "ready") == 0)
      result = check_readiness(&service);
    else if (g_strcmp0(verb, "down") == 0)
      result = service_down(&service);
    else if (g_strcmp0(verb, "status") == 0)
      result = service_status(&service);
    else {
      g_printerr("Unknown command '%s'\n%s", verb, usage);
      result = 2;
    }

    service_clear(&service);
  }

  g_key_file_unref(config);
  return result;
}
