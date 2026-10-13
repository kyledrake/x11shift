#include "resume_monitor.h"

struct _XsResumeMonitor {
    GDBusConnection *connection;
    guint subscription_id;
    XsResumeCallback callback;
    gpointer user_data;
};

static void
prepare_for_sleep(GDBusConnection *connection, const gchar *sender_name,
                  const gchar *object_path, const gchar *interface_name,
                  const gchar *signal_name, GVariant *parameters,
                  gpointer user_data)
{
    XsResumeMonitor *monitor = user_data;
    gboolean sleeping;

    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(b)"))) {
        return;
    }
    g_variant_get(parameters, "(b)", &sleeping);
    if (!sleeping) {
        monitor->callback(monitor->user_data);
    }
}

XsResumeMonitor *
xs_resume_monitor_new(XsResumeCallback callback, gpointer user_data,
                       GError **error)
{
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
    XsResumeMonitor *monitor;

    if (connection == NULL) {
        return NULL;
    }
    monitor = g_new0(XsResumeMonitor, 1);
    monitor->connection = connection;
    monitor->callback = callback;
    monitor->user_data = user_data;
    monitor->subscription_id = g_dbus_connection_signal_subscribe(
        connection, "org.freedesktop.login1", "org.freedesktop.login1.Manager",
        "PrepareForSleep", "/org/freedesktop/login1", NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, prepare_for_sleep, monitor, NULL);
    return monitor;
}

void
xs_resume_monitor_free(XsResumeMonitor *monitor)
{
    if (monitor == NULL) {
        return;
    }
    g_dbus_connection_signal_unsubscribe(monitor->connection,
                                          monitor->subscription_id);
    g_object_unref(monitor->connection);
    g_free(monitor);
}
