/* The Proxenos window: one row per registered service, a switch that runs
   proxenos-cli, and the local-names control that starts the router. */
#define _XOPEN_SOURCE 700
#include "proxenos-common.h"

#include <glib-unix.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PROXENOS_VERSION
#define PROXENOS_VERSION "development"
#endif

#define PROXENOS_SYMBOLIC_ICON "proxenos-symbolic"

#define REFRESH_INTERVAL_SECONDS 2
/* A switch waiting for the world to catch up waits out the service's own
   configured timeout — ready_timeout_seconds on the way up, and
   stop_timeout_seconds on the way down — and then reports what it sees. */
#define DEFAULT_STOP_TIMEOUT_SECONDS 10
#define DEFAULT_READY_TIMEOUT_SECONDS 30
/* The /proc scan that recovers a router with no pid file runs this rarely. */
#define THOROUGH_ROUTER_CHECK_EVERY 15
/* A check that failed is tried again this often, so a service that was slow to
   finish waking up is not reported as broken forever. */
#define RECHECK_FAILED_EVERY 15

typedef enum { INTENT_NONE, INTENT_START, INTENT_STOP } Intent;

typedef struct {
  gchar *name;
  GtkWidget *toggle;
  GtkWidget *state;
  gulong toggle_handler;
  Intent intent;
  gint pending_refreshes;
  gint stop_timeout_seconds;
  gint ready_timeout_seconds;
  gint port;
  gboolean has_ready_command;
  pid_t checked_pid;          /* which process the recorded check result is about */
  gint recheck_countdown;
  ProxenosServiceState known_state;
} ServiceRow;

typedef enum { LOCAL_NAMES_OFF, LOCAL_NAMES_ROUTER_ONLY, LOCAL_NAMES_READY } LocalNamesState;

typedef struct {
  GtkWidget *window;
  GtkWidget *list;
  GtkWidget *message;
  GtkWidget *router_button;
  GtkWidget *router_state;
  GtkStatusIcon *tray;
  gchar *config_path;
  GPtrArray *rows;
  guint refresh_count;
  gboolean router_command_running;
} App;

static void stop_services_and_quit(GtkMenuItem *item, gpointer data);

/* Prefers the installed icon and falls back to the one in the source tree, so
   the window looks right when run straight out of a build directory. */
static gchar *icon_path(void) {
  gchar *installed = g_build_filename(g_get_user_data_dir(), "icons", "hicolor",
                                      "scalable", "apps", "proxenos.svg", NULL);
  if (g_file_test(installed, G_FILE_TEST_IS_REGULAR))
    return installed;
  g_free(installed);
  return g_build_filename("assets", "proxenos.svg", NULL);
}

/* The symbolic icon is asked for by name instead of by path, because the name
   is what makes GTK recolor it: an icon whose name ends in -symbolic is drawn
   in the foreground color of the widget that holds it, and redrawn when the
   theme changes.

   The source tree keeps its copy inside a hicolor layout rather than loose in
   assets, because a loose file is found by an explicit lookup but not by the
   one GtkImage makes: a name ending in -symbolic that no theme claims falls
   back to the same name without the suffix, which is the gold brand icon.  A
   copy under hicolor is claimed by the theme, so the build tree and an
   installed copy resolve to the same file. */
static gboolean symbolic_icon_available(void) {
  static gboolean search_path_offered = FALSE;
  GtkIconTheme *theme = gtk_icon_theme_get_default();

  if (!search_path_offered) {
    search_path_offered = TRUE;
    if (g_file_test("assets/icons/hicolor", G_FILE_TEST_IS_DIR))
      gtk_icon_theme_append_search_path(theme, "assets/icons");
  }

  /* is_symbolic is checked as well as the lookup succeeding, so that the
     fallback to the gold icon described above is reported as a miss rather
     than passed off as a symbolic icon. */
  GtkIconInfo *info = gtk_icon_theme_lookup_icon(theme, PROXENOS_SYMBOLIC_ICON, 48, 0);
  gboolean found = info != NULL && gtk_icon_info_is_symbolic(info);
  g_clear_object(&info);
  return found;
}

/* A hostname resolves only when the router answers for it and the system
   resolver has been pointed at the router; the two are configured separately
   and can be in either state, so the UI reports them separately too. */
static gboolean resolver_is_configured(void) {
  gchar *config = NULL;
  gboolean declared = g_file_get_contents("/etc/systemd/resolved.conf.d/proxenos.conf", &config, NULL, NULL)
      && g_strstr_len(config, -1, "DNS=127.0.0.1")
      && g_strstr_len(config, -1, "Domains=~proxenos");
  g_free(config);
  if (!declared)
    return FALSE;

  char target[PATH_MAX];
  return realpath("/etc/resolv.conf", target)
      && g_strcmp0(target, "/run/systemd/resolve/stub-resolv.conf") == 0;
}

static LocalNamesState local_names_state(gboolean thorough) {
  gboolean running = thorough ? proxenos_router_is_running() : proxenos_router_pid_from_file(NULL);
  if (!running)
    return LOCAL_NAMES_OFF;
  return resolver_is_configured() ? LOCAL_NAMES_READY : LOCAL_NAMES_ROUTER_ONLY;
}

typedef struct {
  const gchar *state;
  const gchar *button;
  const gchar *tooltip;
} LocalNamesText;

static void show_local_names_state(App *app, LocalNamesState state) {
  static const LocalNamesText wording[] = {
    [LOCAL_NAMES_OFF] = { "Local names · off", "Turn on",
                          "Proxenos Router is not running." },
    [LOCAL_NAMES_ROUTER_ONLY] = { "Local names · needs repair", "Repair",
                          "Proxenos Router is running, but the persistent system DNS route is incomplete." },
    [LOCAL_NAMES_READY] = { "Local names · active", "Turn off",
                          "Router and persistent DNS integration are active." },
  };

  const LocalNamesText *text = &wording[state];
  gtk_label_set_text(GTK_LABEL(app->router_state), text->state);
  gtk_button_set_label(GTK_BUTTON(app->router_button), text->button);
  gtk_widget_set_tooltip_text(app->router_state, text->tooltip);
}

/* Runs between fork and exec, where only async-signal-safe calls are allowed.
   A `down` issued on the way out has to finish after the window is gone, and a
   graceful stop can take as long as the service's timeout, so the helper is
   given its own session rather than being torn down with ours. */
static void detach_child(gpointer data) {
  (void)data;
  setsid();
}

static void run_service_command(const gchar *verb, const gchar *name) {
  const gchar *binary = g_getenv("PROXENOS_BIN");
  if (!binary || !*binary)
    binary = "proxenos-cli";

  gchar *argv[] = { (gchar *)binary, (gchar *)verb, (gchar *)name, NULL };
  GError *error = NULL;
  if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                     detach_child, NULL, NULL, &error)) {
    proxenos_debug_log("cannot run %s %s %s: %s", binary, verb, name, error->message);
    g_error_free(error);
  }
}

static gboolean on_switch_flipped(GtkSwitch *toggle, gboolean active, gpointer data) {
  ServiceRow *row = data;
  (void)toggle;

  run_service_command(active ? "up" : "down", row->name);
  row->intent = active ? INTENT_START : INTENT_STOP;
  row->pending_refreshes = 0;
  gtk_label_set_text(GTK_LABEL(row->state), active ? "Starting…" : "Stopping…");
  return FALSE;
}

/* Running and ready are different questions, and the row answers both: the
   switch says whether the process exists, the label says whether it can
   answer.  A service listening on its port whose own check fails is the case
   neither question could express on its own. */
static void show_row_state(ServiceRow *row, ProxenosServiceState state) {
  const gchar *label = "Stopped";
  gchar *detail = NULL;

  switch (state) {
    case PROXENOS_SERVICE_READY:
      label = "Ready";
      if (row->has_ready_command
          && proxenos_read_check_result(row->name, &detail) == PROXENOS_CHECK_FAILED)
        label = "Listening, check failing";
      break;
    case PROXENOS_SERVICE_STARTING:   label = "Starting…"; break;
    case PROXENOS_SERVICE_RUNNING:    label = "Running"; break;
    case PROXENOS_SERVICE_PORT_TAKEN: label = "Port in use by another program"; break;
    default: break;
  }

  gtk_label_set_text(GTK_LABEL(row->state), label);
  gtk_widget_set_tooltip_text(row->state, detail);
  g_free(detail);

  gboolean running = state != PROXENOS_SERVICE_STOPPED;
  if (gtk_switch_get_active(GTK_SWITCH(row->toggle)) == running)
    return;

  /* Moving the switch in code emits ::state-set exactly as a click does, so a
     plain sync would start the service the user has just stopped.  Blocking
     the handler separates "the user asked" from "the world changed". */
  g_signal_handler_block(row->toggle, row->toggle_handler);
  gtk_switch_set_active(GTK_SWITCH(row->toggle), running);
  g_signal_handler_unblock(row->toggle, row->toggle_handler);
}

/* How many refreshes a row waits before it stops believing the user and starts
   reporting what it sees.  A graceful stop is allowed its configured timeout
   plus a refresh, since proxenos-cli only sends KILL once that has run out. */
static gint pending_refresh_limit(const ServiceRow *row) {
  gint seconds = row->intent == INTENT_STOP ? row->stop_timeout_seconds : row->ready_timeout_seconds;
  return seconds / REFRESH_INTERVAL_SECONDS + 2;
}

static void refresh_row(const ProxenosListeners *listeners, ServiceRow *row) {
  ProxenosServiceState state = proxenos_listeners_service_state(listeners, row->name, row->port);
  gboolean running = state != PROXENOS_SERVICE_STOPPED;

  /* A configured check is run once per process, not once per observed start:
     a service stopped and restarted between two refreshes never appears
     stopped, and its new process still deserves its own answer.  A check that
     failed is reconsidered periodically, so a service that simply took longer
     to finish waking up stops being reported as broken.  The window never runs
     the check itself — it asks proxenos-cli, as it does for up and down. */
  if (row->has_ready_command && state == PROXENOS_SERVICE_READY) {
    pid_t pid = 0;
    proxenos_service_pid(row->name, &pid);

    if (pid != row->checked_pid) {
      row->checked_pid = pid;
      row->recheck_countdown = RECHECK_FAILED_EVERY;
      run_service_command("ready", row->name);
    } else if (row->recheck_countdown > 0 && --row->recheck_countdown == 0
               && proxenos_read_check_result(row->name, NULL) == PROXENOS_CHECK_FAILED) {
      row->recheck_countdown = RECHECK_FAILED_EVERY;
      run_service_command("ready", row->name);
    }
  } else if (!running) {
    row->checked_pid = 0;
    row->recheck_countdown = 0;
  }

  if (row->intent != INTENT_NONE) {
    gboolean satisfied = row->intent == INTENT_START
        ? state == PROXENOS_SERVICE_READY || state == PROXENOS_SERVICE_RUNNING
        : !running;
    if (!satisfied && ++row->pending_refreshes < pending_refresh_limit(row))
      return;
    row->intent = INTENT_NONE;
  } else if (state == row->known_state) {
    return;
  }

  row->known_state = state;
  show_row_state(row, state);
}

static gboolean refresh_everything(gpointer data) {
  App *app = data;

  /* The kernel builds its socket table by walking every socket on the machine,
     so it is read once here and asked about each service in turn. */
  ProxenosListeners *listeners = proxenos_listeners_snapshot();
  for (guint i = 0; i < app->rows->len; i++)
    refresh_row(listeners, g_ptr_array_index(app->rows, i));
  proxenos_listeners_free(listeners);

  /* While pkexec is asking for a password the button shows its own text. */
  if (!app->router_command_running) {
    gboolean thorough = app->refresh_count % THOROUGH_ROUTER_CHECK_EVERY == 0;
    show_local_names_state(app, local_names_state(thorough));
  }
  app->refresh_count++;
  return G_SOURCE_CONTINUE;
}

static void on_router_command_finished(GObject *source, GAsyncResult *result, gpointer data) {
  App *app = data;
  GSubprocess *process = G_SUBPROCESS(source);
  gchar *output = NULL, *diagnostic = NULL;
  GError *error = NULL;

  app->router_command_running = FALSE;

  if (!g_subprocess_communicate_utf8_finish(process, result, &output, &diagnostic, &error)) {
    gchar *message = g_strdup_printf("Routing request failed: %s", error->message);
    gtk_label_set_text(GTK_LABEL(app->message), message);
    g_free(message);
    g_error_free(error);
  } else if (!g_subprocess_get_successful(process)) {
    const gchar *detail = diagnostic ? g_strstrip(diagnostic) : "";
    const gchar *message =
        "Local names were not enabled because the system permission request was cancelled or denied.";

    if (g_strstr_len(detail, -1, "Address already in use"))
      message = "Local names could not start because another program is using its DNS or web port. "
                "See the diagnostic log for details.";
    else if (g_strstr_len(detail, -1, "/etc/resolv.conf is not using its stub resolver"))
      message = "Local names need systemd-resolved to handle DNS, but your computer is using a "
                "different DNS setup. Proxenos left that setup unchanged.";
    else if (*detail)
      message = "Local names could not be enabled. Hover here to see the system detail.";

    gtk_label_set_text(GTK_LABEL(app->message), message);
    gtk_widget_set_tooltip_text(app->message, *detail ? detail : NULL);
    proxenos_debug_log("routing request failed: %s",
                       *detail ? detail : "authentication was cancelled or denied");
  } else {
    gtk_label_set_text(GTK_LABEL(app->message), "");
    gtk_widget_set_tooltip_text(app->message, NULL);
  }

  show_local_names_state(app, local_names_state(TRUE));
  g_free(output);
  g_free(diagnostic);
}

/* Binding ports 53 and 80 needs privilege, so the router is launched through
   pkexec.  The password dialog belongs to the desktop's Polkit agent;
   Proxenos never sees what is typed into it. */
static void on_router_button_clicked(GtkButton *button, gpointer data) {
  App *app = data;
  LocalNamesState state = local_names_state(TRUE);
  gboolean turning_on = state != LOCAL_NAMES_READY;
  proxenos_debug_log("local names button clicked while state was %d", state);

  gchar *router = g_find_program_in_path("proxenos-router");
  if (!router) {
    gtk_label_set_text(GTK_LABEL(app->message), "Cannot find proxenos-router on PATH.");
    return;
  }

  gchar *debug_path = proxenos_debug_log_path();
  GPtrArray *argv = g_ptr_array_new();
  g_ptr_array_add(argv, "pkexec");
  g_ptr_array_add(argv, router);
  g_ptr_array_add(argv, turning_on ? "--setup-manjaro" : "--stop");
  g_ptr_array_add(argv, "--debug-log");
  g_ptr_array_add(argv, debug_path);
  if (turning_on) {
    g_ptr_array_add(argv, "--config");
    g_ptr_array_add(argv, app->config_path);
  }
  g_ptr_array_add(argv, NULL);

  GError *error = NULL;
  GSubprocess *process = g_subprocess_newv(
      (const gchar * const *)argv->pdata,
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);

  if (!process) {
    gchar *message = g_strdup_printf("Could not request routing permission: %s", error->message);
    gtk_label_set_text(GTK_LABEL(app->message), message);
    g_free(message);
    g_error_free(error);
  } else {
    app->router_command_running = TRUE;
    gtk_label_set_text(GTK_LABEL(app->router_state),
                       turning_on ? "Waiting for system permission…" : "Stopping local names…");
    gtk_button_set_label(GTK_BUTTON(button), turning_on ? "Authorizing…" : "Stopping…");
    g_subprocess_communicate_utf8_async(process, NULL, NULL, on_router_command_finished, app);
    g_object_unref(process);
  }

  g_ptr_array_free(argv, TRUE);
  g_free(debug_path);
  g_free(router);
}

static void free_row(gpointer data) {
  ServiceRow *row = data;
  g_free(row->name);
  g_free(row);
}

/* ---- the log window -----------------------------------------------------

   A path shown on screen that has to be retyped is a path the window has made
   the reader's problem.  Every location here is a button that copies itself,
   and the captured output is read rather than only named. */

#define LOG_TAIL_LINES 300
/* Reading stops this far from the end, so a log left running for weeks opens
   as fast as one from this morning. */
#define LOG_TAIL_MAX_BYTES (256 * 1024)

typedef struct {
  gchar *captured;     /* the file Proxenos writes for this service */
  GtkWidget *view;     /* where the tail is shown */
  GtkWidget *status;   /* one line reporting what the last click did */
} LogWindow;

static void free_log_window(gpointer data, GClosure *closure) {
  LogWindow *log = data;
  (void)closure;
  g_free(log->captured);
  g_free(log);
}

static void on_copy_path_clicked(GtkButton *button, gpointer data) {
  LogWindow *log = data;
  const gchar *path = g_object_get_data(G_OBJECT(button), "proxenos-path");

  gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), path, -1);

  /* The result is reported in a line of its own rather than by changing the
     button, so the path stays readable and nothing has to be restored on a
     timer that may outlive the window. */
  gchar *message = g_strdup_printf("Copied to clipboard: %s", path);
  gtk_label_set_text(GTK_LABEL(log->status), message);
  g_free(message);
}

/* A path that copies itself when clicked.  It is a button so that the theme
   supplies the hover and focus handling; the styling only takes away the
   frame, leaving the label the theme would draw anyway. */
static GtkWidget *build_copy_row(LogWindow *log, const gchar *label, const gchar *path) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  GtkWidget *caption = gtk_label_new(label);
  gtk_widget_set_halign(caption, GTK_ALIGN_START);
  gtk_widget_set_size_request(caption, 130, -1);
  gtk_style_context_add_class(gtk_widget_get_style_context(caption), "proxenos-state");

  GtkWidget *button = gtk_button_new_with_label(path);
  gtk_widget_set_halign(button, GTK_ALIGN_START);
  gtk_widget_set_tooltip_text(button, "Click to copy this path");
  gtk_style_context_add_class(gtk_widget_get_style_context(button), "proxenos-copy");
  g_object_set_data_full(G_OBJECT(button), "proxenos-path", g_strdup(path), g_free);
  g_signal_connect(button, "clicked", G_CALLBACK(on_copy_path_clicked), log);

  GtkWidget *text = gtk_bin_get_child(GTK_BIN(button));
  gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_selectable(GTK_LABEL(text), FALSE);

  gtk_box_pack_start(GTK_BOX(box), caption, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), button, TRUE, TRUE, 0);
  return box;
}

static void show_log_tail(LogWindow *log) {
  GError *error = NULL;
  gchar *tail = proxenos_tail_file(log->captured, LOG_TAIL_LINES, LOG_TAIL_MAX_BYTES, &error);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log->view));

  if (!tail) {
    /* A service that has never run has no captured file, and saying so is more
       use than an empty box the reader has to interpret.  Prose is wrapped;
       log lines below are not, because a wrapped log line reads as two. */
    gchar *message = g_strdup_printf("Nothing to read yet: %s", error->message);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log->view), GTK_WRAP_WORD_CHAR);
    gtk_text_buffer_set_text(buffer, message, -1);
    g_free(message);
    g_error_free(error);
    return;
  }

  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log->view),
                              *tail ? GTK_WRAP_NONE : GTK_WRAP_WORD_CHAR);
  gtk_text_buffer_set_text(buffer, *tail ? tail : "The log is empty.", -1);
  g_free(tail);

  /* Opened at the end, where the newest lines are. */
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buffer, &end);
  GtkTextMark *mark = gtk_text_buffer_create_mark(buffer, NULL, &end, FALSE);
  gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(log->view), mark, 0, TRUE, 0, 1);
  gtk_text_buffer_delete_mark(buffer, mark);
}

static void on_logs_clicked(GtkButton *button, gpointer data) {
  LogWindow *log = data;
  gchar **declared = g_object_get_data(G_OBJECT(button), "proxenos-declared-logs");
  const gchar *name = g_object_get_data(G_OBJECT(button), "proxenos-service");

  gchar *title = g_strdup_printf("%s · logs", name);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      title, GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button))),
      GTK_DIALOG_DESTROY_WITH_PARENT, "_Refresh", 1, "_Close", GTK_RESPONSE_CLOSE, NULL);
  g_free(title);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 780, 520);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 12);
  gtk_box_set_spacing(GTK_BOX(content), 6);

  log->status = gtk_label_new("");
  gtk_widget_set_halign(log->status, GTK_ALIGN_START);
  gtk_label_set_ellipsize(GTK_LABEL(log->status), PANGO_ELLIPSIZE_MIDDLE);
  gtk_style_context_add_class(gtk_widget_get_style_context(log->status), "proxenos-state");

  gtk_box_pack_start(GTK_BOX(content),
                     build_copy_row(log, "Captured output", log->captured), FALSE, FALSE, 0);
  for (gchar **path = declared; path && *path; path++)
    gtk_box_pack_start(GTK_BOX(content),
                       build_copy_row(log, "Configured log", *path), FALSE, FALSE, 0);

  log->view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(log->view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(log->view), TRUE);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(log->view), 6);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(log->view), 6);

  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
  gtk_container_add(GTK_CONTAINER(scroll), log->view);
  gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(content), log->status, FALSE, FALSE, 0);

  show_log_tail(log);
  gtk_widget_show_all(dialog);

  /* Refresh leaves the window open; everything else closes it. */
  while (gtk_dialog_run(GTK_DIALOG(dialog)) == 1)
    show_log_tail(log);
  gtk_widget_destroy(dialog);
}

/* Proxenos always captures stdout and stderr into a file of its own; `logs=`
   names files the service writes itself, which are listed to be copied but
   not read, because their shape is the service's business and not Proxenos's. */
static gchar **declared_logs(GKeyFile *config, const gchar *name) {
  gsize count = 0;
  gchar **declared = g_key_file_get_string_list(config, name, "logs", &count, NULL);
  return declared;
}

/* ---- reordering ---------------------------------------------------------

   Rows are moved by dragging them, and the new order is written straight back
   to services.conf, so the window and the panel menu, which both read the file
   in group order, never disagree.  The drag carries the service name rather
   than a pointer to the row, which keeps a drop that arrives after the list
   has been rebuilt from moving a row that no longer exists. */

static const GtkTargetEntry service_row_targets[] = {
  { (gchar *)"PROXENOS_SERVICE_ROW", GTK_TARGET_SAME_APP, 0 }
};

static const gchar *row_service_name(gpointer row) {
  return g_object_get_data(G_OBJECT(row), "proxenos-service");
}

static GtkListBoxRow *row_named(GtkListBox *list, const gchar *name) {
  GList *children = gtk_container_get_children(GTK_CONTAINER(list));
  GtkListBoxRow *found = NULL;

  for (GList *child = children; child && !found; child = child->next)
    if (g_strcmp0(row_service_name(child->data), name) == 0)
      found = GTK_LIST_BOX_ROW(child->data);

  g_list_free(children);
  return found;
}

/* The order on screen, written back over the file's group order. */
static void save_service_order(App *app) {
  GList *children = gtk_container_get_children(GTK_CONTAINER(app->list));
  GPtrArray *names = g_ptr_array_new();

  for (GList *child = children; child; child = child->next) {
    const gchar *name = row_service_name(child->data);
    if (name)
      g_ptr_array_add(names, (gpointer)name);
  }

  GError *error = NULL;
  if (!proxenos_config_reorder(app->config_path, (const gchar *const *)names->pdata,
                               names->len, &error)) {
    gchar *message = g_strdup_printf("The new order could not be saved: %s", error->message);
    gtk_label_set_text(GTK_LABEL(app->message), message);
    g_free(message);
    g_error_free(error);
  }

  g_ptr_array_free(names, TRUE);
  g_list_free(children);
}

/* Drags the row itself, so what follows the pointer is the thing being moved
   rather than a generic icon. */
static void on_row_drag_begin(GtkWidget *handle, GdkDragContext *context, gpointer data) {
  GtkWidget *row = gtk_widget_get_ancestor(handle, GTK_TYPE_LIST_BOX_ROW);
  GtkAllocation allocation;
  gint x = 0, y = 0;
  (void)data;

  gtk_widget_get_allocation(row, &allocation);
  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
  cairo_t *cr = cairo_create(surface);

  /* The row is drawn with a background it does not normally have: in the list
     it sits on the list's own, and while it follows the pointer there is
     nothing behind it. */
  gtk_style_context_add_class(gtk_widget_get_style_context(row), "proxenos-dragging");
  gtk_widget_draw(row, cr);
  gtk_style_context_remove_class(gtk_widget_get_style_context(row), "proxenos-dragging");

  gtk_widget_translate_coordinates(handle, row, 0, 0, &x, &y);
  cairo_surface_set_device_offset(surface, -x, -y);
  gtk_drag_set_icon_surface(context, surface);

  cairo_destroy(cr);
  cairo_surface_destroy(surface);
}

static void on_row_drag_data_get(GtkWidget *handle, GdkDragContext *context,
                                 GtkSelectionData *selection, guint info,
                                 guint time, gpointer data) {
  GtkWidget *row = gtk_widget_get_ancestor(handle, GTK_TYPE_LIST_BOX_ROW);
  const gchar *name = row_service_name(row);
  (void)context;
  (void)info;
  (void)time;
  (void)data;

  if (name)
    gtk_selection_data_set(selection, gtk_selection_data_get_target(selection), 8,
                           (const guchar *)name, (gint)strlen(name));
}

/* Where the row goes, counted after it has been taken out of the list. */
static gint drop_index_for(GtkListBox *list, GtkListBoxRow *source, gint y) {
  GtkListBoxRow *target = gtk_list_box_get_row_at_y(list, y);
  if (!target || target == source)
    return -1;                     /* below the last row, or onto itself */

  gint target_y = 0;
  gtk_widget_translate_coordinates(GTK_WIDGET(list), GTK_WIDGET(target), 0, y, NULL, &target_y);

  GtkAllocation allocation;
  gtk_widget_get_allocation(GTK_WIDGET(target), &allocation);

  /* Past the middle of a row means after it, which is what lets a row be
     dropped into the last place as well as before every other row. */
  gint index = gtk_list_box_row_get_index(target);
  if (target_y > allocation.height / 2)
    index++;

  /* Everything below the row has already moved up one place by the time the
     insert happens, because the row is removed first. */
  if (gtk_list_box_row_get_index(source) < index)
    index--;
  return index;
}

static void on_list_drag_data_received(GtkWidget *list, GdkDragContext *context,
                                       gint x, gint y, GtkSelectionData *selection,
                                       guint info, guint time, gpointer data) {
  App *app = data;
  const guchar *raw = gtk_selection_data_get_data(selection);
  gint length = gtk_selection_data_get_length(selection);
  (void)x;
  (void)info;

  if (!raw || length <= 0) {
    gtk_drag_finish(context, FALSE, FALSE, time);
    return;
  }

  gchar *name = g_strndup((const gchar *)raw, (gsize)length);
  GtkListBoxRow *source = row_named(GTK_LIST_BOX(list), name);
  g_free(name);

  if (!source) {
    gtk_drag_finish(context, FALSE, FALSE, time);
    return;
  }

  gint index = drop_index_for(GTK_LIST_BOX(list), source, y);

  g_object_ref(source);
  gtk_container_remove(GTK_CONTAINER(list), GTK_WIDGET(source));
  gtk_list_box_insert(GTK_LIST_BOX(list), GTK_WIDGET(source), index);
  g_object_unref(source);

  save_service_order(app);
  gtk_drag_finish(context, TRUE, FALSE, time);
}

static GtkWidget *build_service_row(App *app, GKeyFile *config, const gchar *name) {
  GtkWidget *row = gtk_list_box_row_new();
  gtk_style_context_add_class(gtk_widget_get_style_context(row), "proxenos-service");
  g_object_set_data_full(G_OBJECT(row), "proxenos-service", g_strdup(name), g_free);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
  gtk_container_set_border_width(GTK_CONTAINER(grid), 10);

  GtkWidget *title = gtk_label_new(name);
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(title), "proxenos-service-name");

  /* The name and what the service is doing are one fact read together, so they
     share a line, separated by a dot rather than by a column of empty space. */
  GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *dot = gtk_label_new("·");
  gtk_style_context_add_class(gtk_widget_get_style_context(dot), "proxenos-dot");

  gint port = g_key_file_get_integer(config, name, "port", NULL);
  ProxenosServiceState initial = proxenos_service_state(name, port);
  gboolean running = initial != PROXENOS_SERVICE_STOPPED;
  GtkWidget *state = gtk_label_new("");
  gtk_widget_set_halign(state, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(state), "proxenos-state");

  gchar *hostname = g_key_file_get_string(config, name, "hostname", NULL);
  gchar *endpoint = hostname && port > 0
      ? g_strdup_printf("%s  →  127.0.0.1:%d", hostname, port)
      : g_strdup("No endpoint declared");
  GtkWidget *endpoint_label = gtk_label_new(endpoint);
  gtk_widget_set_halign(endpoint_label, GTK_ALIGN_START);
  gtk_widget_set_hexpand(endpoint_label, TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(endpoint_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_style_context_add_class(gtk_widget_get_style_context(endpoint_label), "proxenos-endpoint");
  g_free(endpoint);
  g_free(hostname);

  GtkWidget *logs_button = gtk_button_new_with_label("Logs");
  LogWindow *log = g_new0(LogWindow, 1);
  log->captured = proxenos_state_path(name, "log");
  g_object_set_data_full(G_OBJECT(logs_button), "proxenos-service", g_strdup(name), g_free);
  g_object_set_data_full(G_OBJECT(logs_button), "proxenos-declared-logs",
                         declared_logs(config, name), (GDestroyNotify)g_strfreev);
  g_signal_connect_data(logs_button, "clicked", G_CALLBACK(on_logs_clicked),
                        log, free_log_window, 0);

  GtkWidget *toggle = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(toggle), running);

  ServiceRow *service_row = g_new0(ServiceRow, 1);
  service_row->name = g_strdup(name);
  service_row->toggle = toggle;
  service_row->state = state;
  service_row->known_state = initial;
  service_row->port = port;
  service_row->stop_timeout_seconds =
      g_key_file_get_integer(config, name, "stop_timeout_seconds", NULL);
  if (service_row->stop_timeout_seconds <= 0)
    service_row->stop_timeout_seconds = DEFAULT_STOP_TIMEOUT_SECONDS;
  service_row->ready_timeout_seconds =
      g_key_file_get_integer(config, name, "ready_timeout_seconds", NULL);
  if (service_row->ready_timeout_seconds <= 0)
    service_row->ready_timeout_seconds = DEFAULT_READY_TIMEOUT_SECONDS;

  gchar *ready = g_key_file_get_string(config, name, "ready", NULL);
  gchar *ready_command = g_key_file_get_string(config, name, "ready_command", NULL);
  service_row->has_ready_command = ready_command && *ready_command
      && g_strcmp0(ready, "port") != 0 && g_strcmp0(ready, "none") != 0;
  if (g_strcmp0(ready, "none") == 0)
    service_row->port = 0;
  g_free(ready);
  g_free(ready_command);

  service_row->toggle_handler =
      g_signal_connect(toggle, "state-set", G_CALLBACK(on_switch_flipped), service_row);
  g_ptr_array_add(app->rows, service_row);
  show_row_state(service_row, initial);

  /* Two stacked lines on the left, the controls on the right spanning both:
     the endpoint then has the whole width the controls do not use, instead of
     a column sized to the longest service name. */
  gtk_box_pack_start(GTK_BOX(heading), title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(heading), dot, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(heading), state, FALSE, FALSE, 0);

  gtk_grid_attach(GTK_GRID(grid), heading, 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), endpoint_label, 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), logs_button, 1, 0, 1, 2);
  gtk_grid_attach(GTK_GRID(grid), toggle, 2, 0, 1, 2);
  /* A list box row has no window to take button presses from, so the drag
     starts from an event box around the row's contents.  The box is invisible
     and sits below the switch and the Logs button, which go on handling their
     own clicks: the row is dragged by its body, not by its controls. */
  GtkWidget *handle = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(handle), FALSE);
  gtk_container_add(GTK_CONTAINER(handle), grid);
  gtk_container_add(GTK_CONTAINER(row), handle);

  gtk_drag_source_set(handle, GDK_BUTTON1_MASK, service_row_targets,
                      G_N_ELEMENTS(service_row_targets), GDK_ACTION_MOVE);
  g_signal_connect(handle, "drag-begin", G_CALLBACK(on_row_drag_begin), app);
  g_signal_connect(handle, "drag-data-get", G_CALLBACK(on_row_drag_data_get), app);

  return row;
}

static void load_services(App *app) {
  GKeyFile *config = g_key_file_new();
  GError *error = NULL;

  if (!g_key_file_load_from_file(config, app->config_path, G_KEY_FILE_NONE, &error)) {
    gchar *message = g_strdup_printf("Cannot load %s: %s", app->config_path, error->message);
    gtk_label_set_text(GTK_LABEL(app->message), message);
    g_free(message);
    g_error_free(error);
    g_key_file_unref(config);
    return;
  }

  gsize count = 0;
  gchar **names = g_key_file_get_groups(config, &count);
  for (gsize i = 0; i < count; i++)
    gtk_container_add(GTK_CONTAINER(app->list), build_service_row(app, config, names[i]));
  if (count == 0)
    gtk_label_set_text(GTK_LABEL(app->message), "No services are registered.");

  g_strfreev(names);
  g_key_file_unref(config);
  gtk_widget_show_all(app->list);
}

static void stop_services_and_quit(GtkMenuItem *item, gpointer data) {
  App *app = data;
  (void)item;

  for (guint i = 0; i < app->rows->len; i++) {
    ServiceRow *row = g_ptr_array_index(app->rows, i);
    if (proxenos_service_is_running(row->name))
      run_service_command("down", row->name);
  }

  if (proxenos_router_is_running()) {
    gchar *router = g_find_program_in_path("proxenos-router");
    if (router) {
      gchar *argv[] = { "pkexec", router, "--stop", NULL };
      g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
      g_free(router);
    }
  }

  gtk_main_quit();
}

/* A termination request is a deliberate exit, so it means what the tray menu's
   quit means.  GLib dispatches this from the main loop rather than from the
   signal handler itself, which is what makes it safe to touch GTK here.

   Stopping the router needs an authentication prompt, and at logout there may
   be no agent left to answer it; the services stop either way. */
static gboolean on_termination_signal(gpointer data) {
  App *app = data;
  proxenos_debug_log("termination signal received; stopping services before exit");
  stop_services_and_quit(NULL, app);
  return G_SOURCE_REMOVE;
}

static void show_window(GtkStatusIcon *tray, gpointer data) {
  App *app = data;
  (void)tray;
  gtk_widget_show_all(app->window);
  gtk_window_present(GTK_WINDOW(app->window));
}

/* XFCE's notification area is a GtkStatusIcon host.  GTK 3 deprecated the
   widget without shipping a replacement, and the alternative is an external
   AppIndicator dependency, so the deprecation is accepted here deliberately. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

static void on_tray_menu(GtkStatusIcon *tray, guint button, guint activate_time, gpointer data) {
  App *app = data;
  (void)tray;
  (void)button;
  (void)activate_time;

  GtkWidget *menu = gtk_menu_new();
  GtkWidget *show = gtk_menu_item_new_with_label("Show Proxenos");
  GtkWidget *stop = gtk_menu_item_new_with_label("Stop services and quit");
  g_signal_connect(show, "activate", G_CALLBACK(show_window), app);
  g_signal_connect(stop, "activate", G_CALLBACK(stop_services_and_quit), app);

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), show);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), stop);
  gtk_widget_show_all(menu);
  gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
}

static GtkStatusIcon *build_tray(App *app) {
  GtkStatusIcon *tray;

  /* The tray sits among the panel's own symbolic icons, so it takes its color
     from the theme the same way they do.  Loading the brand file is what
     happens when the symbolic icon was not installed. */
  if (symbolic_icon_available()) {
    tray = gtk_status_icon_new_from_icon_name(PROXENOS_SYMBOLIC_ICON);
  } else {
    gchar *path = icon_path();
    tray = gtk_status_icon_new_from_file(path);
    g_free(path);
  }

  gtk_status_icon_set_tooltip_text(tray, "Proxenos local services");
  g_signal_connect(tray, "activate", G_CALLBACK(show_window), app);
  g_signal_connect(tray, "popup-menu", G_CALLBACK(on_tray_menu), app);
  return tray;
}

/* Closing the window keeps Proxenos in the notification area.  With no area to
   retreat to, closing means quitting, and quitting stops the services. */
static gboolean on_window_close(GtkWidget *window, GdkEvent *event, gpointer data) {
  App *app = data;
  (void)event;

  if (app->tray && gtk_status_icon_is_embedded(app->tray)) {
    gtk_widget_hide(window);
    return TRUE;
  }
  stop_services_and_quit(NULL, app);
  return TRUE;
}

G_GNUC_END_IGNORE_DEPRECATIONS

static void apply_theme_styles(void) {
  const gchar *css =
      ".proxenos-hero-title { font-weight: bold; font-size: 16pt; }"
      ".proxenos-hero-subtitle { opacity: 0.68; }"
      ".proxenos-service { margin: 4px 10px; border-radius: 8px; }"
      ".proxenos-service-name { font-weight: bold; }"
      ".proxenos-state { opacity: 0.72; }"
      ".proxenos-routing-button { padding: 2px 6px; }"
      ".proxenos-service.proxenos-dragging { background-color: @theme_bg_color; }"
      ".proxenos-dot { opacity: 0.45; }"
      ".proxenos-endpoint { opacity: 0.85; }"
      /* The copyable path keeps the theme's button behaviour and drops the
         frame, so what changes on hover is the colour the theme already uses
         to mean "this responds to you". */
      ".proxenos-copy { background: none; border: none; box-shadow: none;"
      " padding: 1px 4px; }"
      ".proxenos-copy:hover { color: @theme_selected_bg_color; }"
      ".proxenos-copy:active { color: @theme_selected_bg_color; }";

  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(provider, css, -1, NULL);
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
                                            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

static GtkWidget *build_brand_icon(void) {
  if (symbolic_icon_available()) {
    GtkWidget *icon = gtk_image_new_from_icon_name(PROXENOS_SYMBOLIC_ICON, GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 40);
    return icon;
  }

  gchar *path = icon_path();
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(path, 40, 40, TRUE, NULL);
  g_free(path);

  GtkWidget *icon = pixbuf ? gtk_image_new_from_pixbuf(pixbuf)
                           : gtk_image_new_from_icon_name("image-missing", GTK_ICON_SIZE_DIALOG);
  g_clear_object(&pixbuf);
  return icon;
}

static GtkWidget *build_local_names_control(App *app) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_halign(box, GTK_ALIGN_END);

  app->router_state = gtk_label_new("");
  gtk_widget_set_halign(app->router_state, GTK_ALIGN_END);

  app->router_button = gtk_button_new_with_label("");
  gtk_button_set_relief(GTK_BUTTON(app->router_button), GTK_RELIEF_NONE);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->router_button), "flat");
  gtk_style_context_add_class(gtk_widget_get_style_context(app->router_button), "proxenos-routing-button");
  g_signal_connect(app->router_button, "clicked", G_CALLBACK(on_router_button_clicked), app);

  gtk_box_pack_start(GTK_BOX(box), app->router_state, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), app->router_button, FALSE, FALSE, 0);
  return box;
}

static GtkWidget *build_header(App *app) {
  GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_container_set_border_width(GTK_CONTAINER(hero), 12);
  gtk_style_context_add_class(gtk_widget_get_style_context(hero), "proxenos-hero");
  gtk_box_pack_start(GTK_BOX(hero), build_brand_icon(), FALSE, FALSE, 0);

  GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *title = gtk_label_new("Proxenos");
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(title), "proxenos-hero-title");

  gchar *subtitle_text = g_strdup_printf("Your local services · %s", PROXENOS_VERSION);
  GtkWidget *subtitle = gtk_label_new(subtitle_text);
  g_free(subtitle_text);
  gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "proxenos-hero-subtitle");

  gtk_box_pack_start(GTK_BOX(text), title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(text), subtitle, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hero), text, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(hero), build_local_names_control(app), FALSE, FALSE, 0);
  return hero;
}

static GtkWidget *build_message_label(App *app) {
  app->message = gtk_label_new("");
  gtk_widget_set_halign(app->message, GTK_ALIGN_START);
  gtk_widget_set_margin_start(app->message, 18);
  gtk_widget_set_margin_end(app->message, 18);
  gtk_widget_set_margin_top(app->message, 8);
  gtk_label_set_line_wrap(GTK_LABEL(app->message), TRUE);
  gtk_label_set_line_wrap_mode(GTK_LABEL(app->message), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(app->message), 76);
  return app->message;
}

static GtkWidget *build_service_list(App *app) {
  app->list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->list), GTK_SELECTION_NONE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(app->list), FALSE);

  gtk_drag_dest_set(app->list, GTK_DEST_DEFAULT_ALL, service_row_targets,
                    G_N_ELEMENTS(service_row_targets), GDK_ACTION_MOVE);
  g_signal_connect(app->list, "drag-data-received", G_CALLBACK(on_list_drag_data_received), app);

  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(scroll), app->list);
  return scroll;
}

static GtkWidget *build_window(App *app) {
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "Proxenos");
  gtk_window_set_default_size(GTK_WINDOW(window), 720, 360);
  gtk_window_set_icon_name(GTK_WINDOW(window), "proxenos");

  gchar *path = icon_path();
  gtk_window_set_icon_from_file(GTK_WINDOW(window), path, NULL);
  g_free(path);

  app->window = window;
  g_signal_connect(window, "delete-event", G_CALLBACK(on_window_close), app);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(window), box);
  gtk_box_pack_start(GTK_BOX(box), build_header(app), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), build_message_label(app), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), build_service_list(app), TRUE, TRUE, 8);
  return window;
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  proxenos_debug_log_configure("ui", NULL);
  apply_theme_styles();

  App app = {0};
  app.config_path = proxenos_config_path();
  app.rows = g_ptr_array_new_with_free_func(free_row);

  GtkWidget *window = build_window(&app);
  app.tray = build_tray(&app);
  show_local_names_state(&app, local_names_state(TRUE));
  load_services(&app);
  g_timeout_add_seconds(REFRESH_INTERVAL_SECONDS, refresh_everything, &app);
  g_unix_signal_add(SIGTERM, on_termination_signal, &app);
  g_unix_signal_add(SIGINT, on_termination_signal, &app);
  g_unix_signal_add(SIGHUP, on_termination_signal, &app);

  gtk_widget_show_all(window);
  gtk_main();

  g_clear_object(&app.tray);
  g_ptr_array_unref(app.rows);
  g_free(app.config_path);
  return 0;
}
