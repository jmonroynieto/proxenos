/* Checks on the shared helpers.  Run with `make check`; the test redirects
   XDG_STATE_HOME so it never touches a real service's pid file. */
#include "proxenos-common.h"

#include <stdio.h>
#include <unistd.h>

static gchar *sandbox = NULL;

static void write_pid_file(const gchar *service, const gchar *text) {
  gchar *path = proxenos_state_path(service, "pid");
  g_file_set_contents(path, text, -1, NULL);
  g_free(path);
}

static void check_live_pid_is_recognised(void) {
  GError *error = NULL;
  pid_t self = getpid(), found = 0;

  g_assert_true(proxenos_record_service_pid("selftest", self, &error));
  g_assert_no_error(error);
  g_assert_true(proxenos_service_pid("selftest", &found));
  g_assert_cmpint(found, ==, self);
}

/* A pid on its own is not an identity: the kernel hands the number out again
   once the process is gone.  The recorded start time is what distinguishes a
   service from whatever later inherited its pid. */
static void check_recycled_pid_is_rejected(void) {
  gchar *text = g_strdup_printf("%d 1\n", (int)getpid());
  write_pid_file("selftest", text);
  g_free(text);
  g_assert_false(proxenos_service_is_running("selftest"));
}

/* Pid files written before Proxenos recorded start times carry the pid alone. */
static void check_pid_file_without_start_time(void) {
  gchar *text = g_strdup_printf("%d\n", (int)getpid());
  write_pid_file("selftest", text);
  g_free(text);
  g_assert_true(proxenos_service_is_running("selftest"));
}

static void check_malformed_pid_files(void) {
  write_pid_file("selftest", "not a pid\n");
  g_assert_false(proxenos_service_is_running("selftest"));
  write_pid_file("selftest", "");
  g_assert_false(proxenos_service_is_running("selftest"));
  write_pid_file("selftest", "1\n");   /* init is never a Proxenos service */
  g_assert_false(proxenos_service_is_running("selftest"));
  write_pid_file("selftest", "-4\n");
  g_assert_false(proxenos_service_is_running("selftest"));
}

static void check_forget_removes_the_file(void) {
  proxenos_record_service_pid("selftest", getpid(), NULL);
  proxenos_forget_service_pid("selftest");
  g_assert_false(proxenos_service_is_running("selftest"));
}

static void check_paths_follow_the_environment(void) {
  g_setenv("PROXENOS_CONFIG", "/somewhere/services.conf", TRUE);
  gchar *config = proxenos_config_path();
  g_assert_cmpstr(config, ==, "/somewhere/services.conf");
  g_free(config);
  g_unsetenv("PROXENOS_CONFIG");

  gchar *state = proxenos_state_path("ollama", "log");
  g_assert_true(g_str_has_suffix(state, "/proxenos/ollama.log"));
  g_assert_true(g_str_has_prefix(state, sandbox));
  g_free(state);
}

/* Captured rows of /proc/net/tcp and /proc/net/tcp6.  The IPv6 row is a real
   Forgejo listener: it binds the IPv6 wildcard and never appears in the IPv4
   table, which is why both tables are always read. */
static const gchar *ipv4_table =
    "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
    "   0: 0100007F:2CAA 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 1120475 1 0000 100 0 0 10 0\n"
    "   1: 0100007F:1F90 0100007F:C350 01 00000000:00000000 00:00000000 00000000  1000        0 1120999 1 0000 20 4 30 10 -1\n";

static const gchar *ipv6_table =
    "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
    "   0: 00000000000000000000000000000000:0BB8 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 1115236 1 0000 100 0 0 10 0\n";

static void check_listening_ports_are_found(void) {
  guint64 inode = 0;
  uid_t owner = 0;

  g_assert_true(proxenos_find_listener(ipv4_table, 11434, &inode, &owner));
  g_assert_cmpuint(inode, ==, 1120475);
  g_assert_cmpuint(owner, ==, 1000);

  /* Forgejo, present only in the IPv6 table. */
  g_assert_true(proxenos_find_listener(ipv6_table, 3000, &inode, NULL));
  g_assert_cmpuint(inode, ==, 1115236);
  g_assert_false(proxenos_find_listener(ipv4_table, 3000, &inode, NULL));

  /* Port 8080 appears on the second IPv4 row, but as an established
     connection rather than a listener; only state 0A counts. */
  g_assert_false(proxenos_find_listener(ipv4_table, 8080, &inode, NULL));
  g_assert_false(proxenos_find_listener(ipv4_table, 9999, &inode, NULL));
}

static void check_malformed_tables(void) {
  guint64 inode = 0;
  g_assert_false(proxenos_find_listener("", 3000, &inode, NULL));
  g_assert_false(proxenos_find_listener("garbage\nwith no columns\n", 3000, &inode, NULL));
  g_assert_false(proxenos_find_listener("  sl  local_address\n", 3000, &inode, NULL));
  /* A row cut short mid-way must not be read as a listener. */
  g_assert_false(proxenos_find_listener("   0: 0100007F:2CAA 00000000:0000 0A 00000000:00000000\n",
                                        11434, &inode, NULL));
}

/* This process is listening on nothing, so its own port state is closed, and
   a service whose port never opens reads as starting rather than ready. */
static void check_service_state_without_a_listener(void) {
  proxenos_record_service_pid("selftest", getpid(), NULL);
  g_assert_cmpint(proxenos_service_state("selftest", 0), ==, PROXENOS_SERVICE_RUNNING);
  g_assert_cmpint(proxenos_service_state("selftest", 49999), ==, PROXENOS_SERVICE_STARTING);
  proxenos_forget_service_pid("selftest");
  g_assert_cmpint(proxenos_service_state("selftest", 49999), ==, PROXENOS_SERVICE_STOPPED);
}

static void check_recorded_check_results(void) {
  g_assert_cmpint(proxenos_read_check_result("selftest", NULL), ==, PROXENOS_CHECK_UNKNOWN);

  proxenos_write_check_result("selftest", PROXENOS_CHECK_FAILED, "ready_command exited with status 22");
  gchar *detail = NULL;
  g_assert_cmpint(proxenos_read_check_result("selftest", &detail), ==, PROXENOS_CHECK_FAILED);
  g_assert_cmpstr(detail, ==, "ready_command exited with status 22");
  g_free(detail);

  proxenos_write_check_result("selftest", PROXENOS_CHECK_PASSED, NULL);
  g_assert_cmpint(proxenos_read_check_result("selftest", NULL), ==, PROXENOS_CHECK_PASSED);

  proxenos_forget_check_result("selftest");
  g_assert_cmpint(proxenos_read_check_result("selftest", NULL), ==, PROXENOS_CHECK_UNKNOWN);
}

int main(void) {
  sandbox = g_dir_make_tmp("proxenos-selftest-XXXXXX", NULL);
  g_assert_nonnull(sandbox);
  g_setenv("XDG_STATE_HOME", sandbox, TRUE);

  check_live_pid_is_recognised();
  check_recycled_pid_is_rejected();
  check_pid_file_without_start_time();
  check_malformed_pid_files();
  check_forget_removes_the_file();
  check_paths_follow_the_environment();
  check_listening_ports_are_found();
  check_malformed_tables();
  check_service_state_without_a_listener();
  check_recorded_check_results();

  g_print("proxenos selftest: all checks passed\n");
  g_free(sandbox);
  return 0;
}
