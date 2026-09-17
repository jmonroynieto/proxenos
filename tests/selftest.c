/* Checks on the shared helpers.  Run with `make check`; the test redirects
   XDG_STATE_HOME so it never touches a real service's pid file. */
#include "proxenos-common.h"

#include <stdio.h>
#include <string.h>
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

/* ---- config group order ---------------------------------------------- */

static gchar *write_config(const gchar *contents) {
  gchar *path = g_build_filename(sandbox, "services.conf", NULL);
  g_assert_true(g_file_set_contents(path, contents, -1, NULL));
  return path;
}

static gchar *read_config(const gchar *path) {
  gchar *contents = NULL;
  g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
  return contents;
}

/* The file people actually keep: a preamble that belongs to nobody, a group
   whose explanation sits directly above its header, and a group with a blank
   line between it and the comment above. */
static const gchar *const sample_config =
    "# Proxenos services.\n"
    "# Copy this file and edit it.\n"
    "\n"
    "[alpha]\n"
    "port=1\n"
    "\n"
    "# Beta is measured, not defaulted to.\n"
    "[beta]\n"
    "port=2\n"
    "\n"
    "# Gamma keeps its own listener.\n"
    "[gamma]\n"
    "port=3\n";

static void check_reorder_moves_whole_blocks(void) {
  gchar *path = write_config(sample_config);
  const gchar *order[] = { "gamma", "alpha", "beta" };
  GError *error = NULL;

  g_assert_true(proxenos_config_reorder(path, order, G_N_ELEMENTS(order), &error));
  g_assert_no_error(error);

  gchar *contents = read_config(path);
  g_assert_cmpstr(contents, ==,
      "# Proxenos services.\n"
      "# Copy this file and edit it.\n"
      "\n"
      "# Gamma keeps its own listener.\n"
      "[gamma]\n"
      "port=3\n"
      "\n"
      "[alpha]\n"
      "port=1\n"
      "\n"
      "# Beta is measured, not defaulted to.\n"
      "[beta]\n"
      "port=2\n");
  g_free(contents);
  g_free(path);
}

/* Asking for the order the file already has must not rewrite it. */
static void check_reorder_of_an_unchanged_order_writes_nothing(void) {
  gchar *path = write_config(sample_config);
  const gchar *order[] = { "alpha", "beta", "gamma" };

  g_assert_true(proxenos_config_reorder(path, order, G_N_ELEMENTS(order), NULL));

  gchar *contents = read_config(path);
  g_assert_cmpstr(contents, ==, sample_config);
  g_free(contents);
  g_free(path);
}

/* A group added to the file by hand while the window was open is kept, after
   the groups the window knew about. */
static void check_reorder_keeps_unnamed_groups(void) {
  gchar *path = write_config(sample_config);
  const gchar *order[] = { "gamma", "alpha" };

  g_assert_true(proxenos_config_reorder(path, order, G_N_ELEMENTS(order), NULL));

  gchar *contents = read_config(path);
  g_assert_nonnull(strstr(contents, "[beta]"));
  g_assert_nonnull(strstr(contents, "# Beta is measured, not defaulted to."));
  g_assert_cmpint(strstr(contents, "[gamma]") < strstr(contents, "[alpha]"), ==, TRUE);
  g_assert_cmpint(strstr(contents, "[alpha]") < strstr(contents, "[beta]"), ==, TRUE);
  g_free(contents);
  g_free(path);
}

/* A file with no newline at the end still has none afterwards. */
static void check_reorder_preserves_a_missing_final_newline(void) {
  gchar *path = write_config("[alpha]\nport=1\n\n[beta]\nport=2");
  const gchar *order[] = { "beta", "alpha" };

  g_assert_true(proxenos_config_reorder(path, order, G_N_ELEMENTS(order), NULL));

  gchar *contents = read_config(path);
  g_assert_cmpstr(contents, ==, "[beta]\nport=2\n\n[alpha]\nport=1");
  g_free(contents);
  g_free(path);
}

/* Moving a block away and back must return the file it started from.  A
   comment run trailing a block is the case that breaks this if block
   separation is not guaranteed, so the sample ends with one. */
static void check_reorder_round_trip_restores_the_file(void) {
  const gchar *with_trailing_comment =
      "[alpha]\n"
      "port=1\n"
      "\n"
      "# Beta is measured, not defaulted to.\n"
      "[beta]\n"
      "port=2\n"
      "\n"
      "# A commented-out example kept at the end of the file.\n"
      "#[example]\n"
      "#port=3\n";
  gchar *path = write_config(with_trailing_comment);
  const gchar *moved[] = { "beta", "alpha" };
  const gchar *back[] = { "alpha", "beta" };

  g_assert_true(proxenos_config_reorder(path, moved, G_N_ELEMENTS(moved), NULL));
  g_assert_true(proxenos_config_reorder(path, back, G_N_ELEMENTS(back), NULL));

  gchar *contents = read_config(path);
  g_assert_cmpstr(contents, ==, with_trailing_comment);
  g_free(contents);
  g_free(path);
}

static void check_reorder_of_a_file_without_groups(void) {
  gchar *path = write_config("# nothing but a comment\n");
  const gchar *order[] = { "alpha" };

  g_assert_true(proxenos_config_reorder(path, order, G_N_ELEMENTS(order), NULL));

  gchar *contents = read_config(path);
  g_assert_cmpstr(contents, ==, "# nothing but a comment\n");
  g_free(contents);
  g_free(path);
}

static void check_reorder_reports_a_missing_file(void) {
  gchar *path = g_build_filename(sandbox, "absent.conf", NULL);
  const gchar *order[] = { "alpha" };
  GError *error = NULL;

  g_assert_false(proxenos_config_reorder(path, order, G_N_ELEMENTS(order), &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
  g_free(path);
}

/* ---- reading the end of a log ---------------------------------------- */

static gchar *write_log(const gchar *contents) {
  gchar *path = g_build_filename(sandbox, "tail.log", NULL);
  g_assert_true(g_file_set_contents(path, contents, -1, NULL));
  return path;
}

static void check_tail_returns_the_last_lines(void) {
  gchar *path = write_log("one\ntwo\nthree\nfour\nfive\n");
  GError *error = NULL;

  gchar *tail = proxenos_tail_file(path, 2, 1 << 20, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(tail, ==, "four\nfive\n");
  g_free(tail);

  /* Asking for more lines than the file holds gives the whole file. */
  tail = proxenos_tail_file(path, 50, 1 << 20, NULL);
  g_assert_cmpstr(tail, ==, "one\ntwo\nthree\nfour\nfive\n");
  g_free(tail);
  g_free(path);
}

static void check_tail_without_a_final_newline(void) {
  gchar *path = write_log("one\ntwo\nthree");
  gchar *tail = proxenos_tail_file(path, 2, 1 << 20, NULL);
  g_assert_cmpstr(tail, ==, "two\nthree");
  g_free(tail);
  g_free(path);
}

static void check_tail_of_an_empty_file(void) {
  gchar *path = write_log("");
  gchar *tail = proxenos_tail_file(path, 10, 1 << 20, NULL);
  g_assert_cmpstr(tail, ==, "");
  g_free(tail);
  g_free(path);
}

/* A cap that lands mid-line drops that partial line rather than showing it as
   though the log said it. */
static void check_tail_drops_a_partial_first_line(void) {
  gchar *path = write_log("aaaaaaaaaa\nbbbbbbbbbb\ncccccccccc\n");
  gchar *tail = proxenos_tail_file(path, 50, 16, NULL);
  g_assert_cmpstr(tail, ==, "cccccccccc\n");
  g_free(tail);
  g_free(path);
}

static void check_tail_reports_a_missing_file(void) {
  gchar *path = g_build_filename(sandbox, "not-there.log", NULL);
  GError *error = NULL;

  g_assert_null(proxenos_tail_file(path, 10, 1 << 20, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
  g_free(path);
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
  check_reorder_moves_whole_blocks();
  check_reorder_of_an_unchanged_order_writes_nothing();
  check_reorder_keeps_unnamed_groups();
  check_reorder_preserves_a_missing_final_newline();
  check_reorder_round_trip_restores_the_file();
  check_reorder_of_a_file_without_groups();
  check_reorder_reports_a_missing_file();
  check_tail_returns_the_last_lines();
  check_tail_without_a_final_newline();
  check_tail_of_an_empty_file();
  check_tail_drops_a_partial_first_line();
  check_tail_reports_a_missing_file();

  g_print("proxenos selftest: all checks passed\n");
  g_free(sandbox);
  return 0;
}
