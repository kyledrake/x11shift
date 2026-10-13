#include "resume_monitor.h"

static guint updates;
static guint fences;

static void
on_resume(gpointer user_data)
{
    guint *count = user_data;
    (*count)++;
}

static void
on_fence(GDBusConnection *connection, const gchar *sender_name,
          const gchar *object_path, const gchar *interface_name,
          const gchar *signal_name, GVariant *parameters, gpointer user_data)
{
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    (void)parameters;
    (void)user_data;
    fences++;
}

static gboolean
timeout_failure(gpointer user_data)
{
    (void)user_data;
    g_error("Timed out waiting for test bus messages");
    return G_SOURCE_REMOVE;
}

static void
emit_and_dispatch(GDBusConnection *sender, const char *path, GVariant *parameters)
{
    GError *error = NULL;
    guint previous_fences = fences;
    guint timeout_id;

    g_assert_true(g_dbus_connection_emit_signal(sender, NULL, path,
        "org.freedesktop.login1.Manager", "PrepareForSleep", parameters, &error));
    g_assert_no_error(error);
    /* A following signal fences delivery without arbitrary sleeps. */
    g_assert_true(g_dbus_connection_emit_signal(sender, NULL,
        "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
        "TestFence", NULL, &error));
    g_assert_no_error(error);
    timeout_id = g_timeout_add_seconds(5, timeout_failure, NULL);
    while (fences == previous_fences) {
        g_main_context_iteration(NULL, TRUE);
    }
    while (g_main_context_iteration(NULL, FALSE)) {
    }
    g_source_remove(timeout_id);
}

static GDBusConnection *
connect_bus(const char *address)
{
    GError *error = NULL;
    GDBusConnection *connection = g_dbus_connection_new_for_address_sync(address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(connection);
    return connection;
}

static void
test_resume_notifications(void)
{
    GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    GDBusConnection *sender;
    GDBusConnection *unrelated;
    GDBusConnection *receiver;
    GVariant *reply;
    GError *error = NULL;
    XsResumeMonitor *monitor;
    guint fence_id;

    g_test_dbus_up(bus);
    g_setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(bus), TRUE);
    sender = connect_bus(g_test_dbus_get_bus_address(bus));
    unrelated = connect_bus(g_test_dbus_get_bus_address(bus));
    reply = g_dbus_connection_call_sync(sender, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.freedesktop.login1", 0), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    g_assert_no_error(error);
    g_variant_unref(reply);

    monitor = xs_resume_monitor_new(on_resume, &updates, &error);
    g_assert_no_error(error);
    g_assert_nonnull(monitor);
    receiver = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    g_assert_no_error(error);
    fence_id = g_dbus_connection_signal_subscribe(receiver, NULL,
        "org.freedesktop.login1.Manager", "TestFence", "/org/freedesktop/login1",
        NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_fence, NULL, NULL);
    /* Wait for the bus to install the subscriptions before emitting. */
    reply = g_dbus_connection_call_sync(receiver, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames", NULL,
        NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    g_assert_no_error(error);
    g_variant_unref(reply);

    emit_and_dispatch(sender, "/org/freedesktop/login1", g_variant_new("(b)", TRUE));
    g_assert_cmpuint(updates, ==, 0);
    emit_and_dispatch(sender, "/org/freedesktop/login1", g_variant_new("(s)", "false"));
    emit_and_dispatch(sender, "/unrelated", g_variant_new("(b)", FALSE));
    emit_and_dispatch(unrelated, "/org/freedesktop/login1", g_variant_new("(b)", FALSE));
    g_assert_cmpuint(updates, ==, 0);
    emit_and_dispatch(sender, "/org/freedesktop/login1", g_variant_new("(b)", FALSE));
    g_assert_cmpuint(updates, ==, 1);
    emit_and_dispatch(sender, "/org/freedesktop/login1", g_variant_new("(b)", FALSE));
    g_assert_cmpuint(updates, ==, 2);
    xs_resume_monitor_free(monitor);
    emit_and_dispatch(sender, "/org/freedesktop/login1", g_variant_new("(b)", FALSE));
    g_assert_cmpuint(updates, ==, 2);

    g_dbus_connection_signal_unsubscribe(receiver, fence_id);
    g_object_unref(receiver);
    g_object_unref(unrelated);
    g_object_unref(sender);
    g_test_dbus_down(bus);
    g_object_unref(bus);
}

static void
test_missing_bus(void)
{
    if (g_test_subprocess()) {
        GError *error = NULL;
        XsResumeMonitor *monitor;
        g_setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/x11shift-test-bus", TRUE);
        monitor = xs_resume_monitor_new(on_resume, &updates, &error);
        g_assert_null(monitor);
        g_assert_nonnull(error);
        g_clear_error(&error);
        xs_resume_monitor_free(monitor);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/resume/missing-bus", test_missing_bus);
    g_test_add_func("/resume/notifications", test_resume_notifications);
    return g_test_run();
}
