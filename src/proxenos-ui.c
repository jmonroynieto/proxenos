/* The Proxenos window: one row per registered service, a switch that runs
   proxenos-cli, and the local-names control that starts the router. */
#define _XOPEN_SOURCE 700
#include "proxenos-common.h"

#include <glib-unix.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef PROXENOS_VERSION
#define PROXENOS_VERSION "development"
#endif

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

static void free_string_closure(gpointer data, GClosure *closure) {
  (void)closure;
  g_free(data);
}

static void on_logs_clicked(GtkButton *button, gpointer data) {
  const gchar *locations = data;
  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button))), GTK_DIALOG_MODAL,
      GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "Log locations");
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", locations);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

/* Proxenos always captures stdout and stderr; `logs=` names files the service
   writes itself, which Proxenos reports but does not read. */
static gchar *log_locations(GKeyFile *config, const gchar *name) {
  gchar *captured = proxenos_state_path(name, "log");
  GString *text = g_string_new(NULL);
  g_string_append_printf(text, "Captured output: %s", captured);
  g_free(captured);

  gsize count = 0;
  gchar **declared = g_key_file_get_string_list(config, name, "logs", &count, NULL);
  for (gsize i = 0; declared && i < count; i++)
    g_string_append_printf(text, "\nConfigured log: %s", declared[i]);
  g_strfreev(declared);

  return g_string_free(text, FALSE);
}

static GtkWidget *build_service_row(App *app, GKeyFile *config, const gchar *name) {
  GtkWidget *row = gtk_list_box_row_new();
  gtk_style_context_add_class(gtk_widget_get_style_context(row), "proxenos-service");

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
  gtk_container_set_border_width(GTK_CONTAINER(grid), 10);

  GtkWidget *title = gtk_label_new(name);
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(title), "proxenos-service-name");

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
  g_free(endpoint);
  g_free(hostname);

  GtkWidget *logs_button = gtk_button_new_with_label("Logs");
  g_signal_connect_data(logs_button, "clicked", G_CALLBACK(on_logs_clicked),
                        log_locations(config, name), free_string_closure, 0);

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

  gtk_grid_attach(GTK_GRID(grid), title, 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), state, 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), endpoint_label, 1, 0, 1, 2);
  gtk_grid_attach(GTK_GRID(grid), logs_button, 2, 0, 1, 2);
  gtk_grid_attach(GTK_GRID(grid), toggle, 3, 0, 1, 2);
  gtk_container_add(GTK_CONTAINER(row), grid);
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
  gchar *path = icon_path();
  GtkStatusIcon *tray = gtk_status_icon_new_from_file(path);
  g_free(path);

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
      ".proxenos-routing-button { padding: 2px 6px; }";

  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(provider, css, -1, NULL);
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
                                            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

static GtkWidget *build_brand_icon(void) {
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
