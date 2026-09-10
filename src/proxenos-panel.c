/* XFCE panel plugin: a button whose menu lists every registered service with a
   check mark for the ones that are running.  All state lives in the same
   places proxenos-cli uses, so the panel and the window never disagree. */
#include "proxenos-common.h"

#include <libxfce4panel/libxfce4panel.h>

static void run_service_command(const gchar *verb, const gchar *service) {
  const gchar *binary = g_getenv("PROXENOS_BIN");
  if (!binary || !*binary)
    binary = "proxenos-cli";

  gchar *argv[] = { (gchar *)binary, (gchar *)verb, (gchar *)service, NULL };
  g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                NULL, NULL, NULL, NULL);
}

static void on_service_toggled(GtkCheckMenuItem *item, gpointer data) {
  const gchar *service = g_object_get_data(G_OBJECT(item), "proxenos-service");
  (void)data;
  run_service_command(gtk_check_menu_item_get_active(item) ? "up" : "down", service);
}

static void append_disabled_label(GtkWidget *menu, const gchar *text) {
  GtkWidget *item = gtk_menu_item_new_with_label(text);
  gtk_widget_set_sensitive(item, FALSE);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  gtk_widget_show(item);
}

/* Proxenos always captures stdout and stderr; `logs=` names files the service
   writes itself, which are listed for reference only. */
static void append_log_locations(GtkWidget *menu, GKeyFile *config, const gchar *name) {
  gchar *captured = proxenos_state_path(name, "log");
  gchar *label = g_strdup_printf("  captured: %s", captured);
  append_disabled_label(menu, label);
  g_free(label);
  g_free(captured);

  gsize count = 0;
  gchar **declared = g_key_file_get_string_list(config, name, "logs", &count, NULL);
  for (gsize i = 0; declared && i < count; i++) {
    label = g_strdup_printf("  configured: %s", declared[i]);
    append_disabled_label(menu, label);
    g_free(label);
  }
  g_strfreev(declared);
}

static void append_service(GtkWidget *menu, const ProxenosListeners *listeners,
                           GKeyFile *config, const gchar *name) {
  gint port = g_key_file_get_integer(config, name, "port", NULL);
  gchar *ready = g_key_file_get_string(config, name, "ready", NULL);
  if (g_strcmp0(ready, "none") == 0)
    port = 0;
  g_free(ready);

  ProxenosServiceState state = proxenos_listeners_service_state(listeners, name, port);
  gboolean running = state != PROXENOS_SERVICE_STOPPED;

  /* The menu says running and ready separately, because a service can be one
     without the other. */
  gchar *label = state == PROXENOS_SERVICE_READY || state == PROXENOS_SERVICE_STOPPED
      ? g_strdup(name)
      : g_strdup_printf("%s  (%s)", name, proxenos_service_state_text(state));
  GtkWidget *item = gtk_check_menu_item_new_with_label(label);
  g_free(label);

  g_object_set_data_full(G_OBJECT(item), "proxenos-service", g_strdup(name), g_free);

  /* The check mark is set before the handler is connected, so showing the menu
     never looks like a click. */
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), running);
  g_signal_connect(item, "toggled", G_CALLBACK(on_service_toggled), NULL);

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  gtk_widget_show(item);
  append_log_locations(menu, config, name);
}

static void popup_service_menu(XfcePanelPlugin *plugin, gpointer data) {
  GtkWidget *menu = gtk_menu_new();
  gchar *path = proxenos_config_path();
  GKeyFile *config = g_key_file_new();
  (void)data;

  if (g_key_file_load_from_file(config, path, G_KEY_FILE_NONE, NULL)) {
    gsize count = 0;
    gchar **names = g_key_file_get_groups(config, &count);
    ProxenosListeners *listeners = proxenos_listeners_snapshot();
    for (gsize i = 0; i < count; i++)
      append_service(menu, listeners, config, names[i]);
    if (count == 0)
      append_disabled_label(menu, "No services are registered");
    proxenos_listeners_free(listeners);
    g_strfreev(names);
  } else {
    append_disabled_label(menu, "No Proxenos configuration");
  }

  g_free(path);
  g_key_file_unref(config);
  xfce_panel_plugin_popup_menu(plugin, GTK_MENU(menu), NULL, NULL);
}

static void proxenos_construct(XfcePanelPlugin *plugin) {
  GtkWidget *button = xfce_panel_create_button();
  GtkWidget *label = gtk_label_new("Proxenos");

  gtk_container_add(GTK_CONTAINER(button), label);
  gtk_widget_show_all(button);
  gtk_widget_set_tooltip_text(button, "Toggle local services");
  gtk_container_add(GTK_CONTAINER(plugin), button);
  g_signal_connect_swapped(button, "clicked", G_CALLBACK(popup_service_menu), plugin);
}

XFCE_PANEL_PLUGIN_REGISTER(proxenos_construct)
