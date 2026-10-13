#define _POSIX_C_SOURCE 200809L

#include "schedule.h"
#include "resume_monitor.h"
#include "x11_gamma.h"

#include <gtk/gtk.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define APP_ID "io.x11shift.X11Shift"
#define APP_NAME "X11Shift"
#define APP_VERSION "1.1.6"
#define UPDATE_INTERVAL_SECONDS 5
#define DEFAULT_DAY_TEMPERATURE 6500
#define DEFAULT_NIGHT_TEMPERATURE 3400
#define DEFAULT_DAY_BRIGHTNESS 1.0
#define DEFAULT_NIGHT_BRIGHTNESS 0.9
#define DEFAULT_COLOR_STRENGTH 1.0

typedef struct {
    gboolean enabled;
    XsScheduleMode mode;
    int day_temperature;
    int night_temperature;
    double day_brightness;
    double night_brightness;
    double strength;
    int transition_minutes;
    int custom_day_minute;
    int custom_night_minute;
    double latitude;
    double longitude;
    gboolean solar_location_set;
    gboolean limit_outputs;
    GHashTable *selected_outputs;
} XsSettings;

typedef struct {
    GtkApplication *application;
    GtkWidget *window;
    GtkWidget *enabled_switch;
    GtkWidget *mode_combo;
    GtkWidget *day_temperature;
    GtkWidget *night_temperature;
    GtkWidget *day_brightness;
    GtkWidget *night_brightness;
    GtkWidget *strength;
    GtkWidget *transition;
    GtkWidget *custom_day_entry;
    GtkWidget *custom_night_entry;
    GtkWidget *latitude;
    GtkWidget *longitude;
    GtkWidget *custom_grid;
    GtkWidget *solar_grid;
    GtkWidget *display_box;
    GtkWidget *autostart_check;
    GtkWidget *hide_button;
    GtkWidget *status_label;
    GtkWidget *detail_label;
    GtkStatusIcon *status_icon;
    GtkWidget *tray_enabled_item;
    XsSettings settings;
    XsGammaController *gamma;
    XsResumeMonitor *resume_monitor;
    guint timer_id;
    time_t paused_until;
    gboolean updating_widgets;
    gboolean start_hidden;
    gboolean first_activation;
    gchar *config_path;
} XsApp;

static void app_update(XsApp *app);
static void settings_changed(GtkWidget *widget, gpointer user_data);

static void
enabled_switch_changed(GObject *object, GParamSpec *parameter,
                       gpointer user_data)
{
    (void)parameter;
    settings_changed(GTK_WIDGET(object), user_data);
}

static const char *
mode_to_string(XsScheduleMode mode)
{
    switch (mode) {
    case XS_SCHEDULE_SOLAR:
        return "solar";
    case XS_SCHEDULE_CUSTOM:
        return "custom";
    case XS_SCHEDULE_ALWAYS_NIGHT:
        return "always-night";
    case XS_SCHEDULE_ALWAYS_DAY:
        return "always-day";
    }
    return "solar";
}

static XsScheduleMode
mode_from_string(const char *mode)
{
    if (g_strcmp0(mode, "custom") == 0) {
        return XS_SCHEDULE_CUSTOM;
    }
    if (g_strcmp0(mode, "always-night") == 0) {
        return XS_SCHEDULE_ALWAYS_NIGHT;
    }
    if (g_strcmp0(mode, "always-day") == 0) {
        return XS_SCHEDULE_ALWAYS_DAY;
    }
    return XS_SCHEDULE_SOLAR;
}

static gboolean
key_file_boolean(GKeyFile *file, const char *group, const char *key,
                 gboolean fallback)
{
    GError *error = NULL;
    gboolean result = g_key_file_get_boolean(file, group, key, &error);
    if (error != NULL) {
        g_clear_error(&error);
        return fallback;
    }
    return result;
}

static int
key_file_integer(GKeyFile *file, const char *group, const char *key,
                 int fallback)
{
    GError *error = NULL;
    int result = g_key_file_get_integer(file, group, key, &error);
    if (error != NULL) {
        g_clear_error(&error);
        return fallback;
    }
    return result;
}

static double
key_file_double(GKeyFile *file, const char *group, const char *key,
                double fallback)
{
    GError *error = NULL;
    double result = g_key_file_get_double(file, group, key, &error);
    if (error != NULL) {
        g_clear_error(&error);
        return fallback;
    }
    return result;
}

static gchar *
key_file_string(GKeyFile *file, const char *group, const char *key,
                const char *fallback)
{
    GError *error = NULL;
    gchar *result = g_key_file_get_string(file, group, key, &error);
    if (error != NULL) {
        g_clear_error(&error);
        return g_strdup(fallback);
    }
    return result;
}

static void
settings_defaults(XsSettings *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->enabled = TRUE;
    settings->mode = XS_SCHEDULE_CUSTOM;
    settings->day_temperature = DEFAULT_DAY_TEMPERATURE;
    settings->night_temperature = DEFAULT_NIGHT_TEMPERATURE;
    settings->day_brightness = DEFAULT_DAY_BRIGHTNESS;
    settings->night_brightness = DEFAULT_NIGHT_BRIGHTNESS;
    settings->strength = DEFAULT_COLOR_STRENGTH;
    settings->transition_minutes = 45;
    settings->custom_day_minute = 7 * 60;
    settings->custom_night_minute = 19 * 60;
    settings->latitude = 0.0;
    settings->longitude = 0.0;
    settings->solar_location_set = FALSE;
    settings->selected_outputs = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                       g_free, NULL);
}

static void
settings_load(XsApp *app)
{
    GKeyFile *file = g_key_file_new();
    gchar *mode;
    gchar **outputs;
    gsize output_count = 0;
    guint index;

    settings_defaults(&app->settings);
    if (!g_key_file_load_from_file(file, app->config_path,
                                   G_KEY_FILE_NONE, NULL)) {
        g_key_file_unref(file);
        return;
    }

    app->settings.enabled = key_file_boolean(file, "General", "Enabled", TRUE);
    mode = key_file_string(file, "Schedule", "Mode", "custom");
    app->settings.mode = mode_from_string(mode);
    g_free(mode);
    app->settings.day_temperature = CLAMP(
        key_file_integer(file, "Color", "DayTemperature", DEFAULT_DAY_TEMPERATURE), 2500, 7500);
    app->settings.night_temperature = CLAMP(
        key_file_integer(file, "Color", "NightTemperature", DEFAULT_NIGHT_TEMPERATURE), 1000, 6500);
    app->settings.day_brightness = CLAMP(
        key_file_double(file, "Color", "DayBrightness", DEFAULT_DAY_BRIGHTNESS), 0.2, 1.0);
    app->settings.night_brightness = CLAMP(
        key_file_double(file, "Color", "NightBrightness", DEFAULT_NIGHT_BRIGHTNESS), 0.2, 1.0);
    app->settings.strength = CLAMP(
        key_file_double(file, "Color", "Strength", DEFAULT_COLOR_STRENGTH), 0.0, 1.0);
    app->settings.transition_minutes = CLAMP(
        key_file_integer(file, "Schedule", "TransitionMinutes", 45), 0, 180);
    app->settings.custom_day_minute = CLAMP(
        key_file_integer(file, "Schedule", "DayStartMinute", 420), 0, 1439);
    app->settings.custom_night_minute = CLAMP(
        key_file_integer(file, "Schedule", "NightStartMinute", 1140), 0, 1439);
    app->settings.latitude = CLAMP(
        key_file_double(file, "Location", "Latitude", 0.0), -89.8, 89.8);
    app->settings.longitude = CLAMP(
        key_file_double(file, "Location", "Longitude", 0.0), -180.0, 180.0);
    app->settings.solar_location_set = key_file_boolean(
        file, "Location", "Configured",
        fabs(app->settings.latitude) > 0.00005 ||
        fabs(app->settings.longitude) > 0.00005);
    app->settings.limit_outputs = key_file_boolean(
        file, "Displays", "LimitToSelected", FALSE);
    outputs = g_key_file_get_string_list(file, "Displays", "Selected",
                                         &output_count, NULL);
    for (index = 0; outputs != NULL && index < output_count; index++) {
        g_hash_table_add(app->settings.selected_outputs, g_strdup(outputs[index]));
    }
    g_strfreev(outputs);
    g_key_file_unref(file);
}

static void
settings_save(XsApp *app)
{
    GKeyFile *file = g_key_file_new();
    gchar *data;
    gsize data_size;
    gchar *directory;
    GHashTableIter iterator;
    gpointer key;
    GPtrArray *outputs;

    g_key_file_set_boolean(file, "General", "Enabled", app->settings.enabled);
    g_key_file_set_string(file, "Schedule", "Mode",
                          mode_to_string(app->settings.mode));
    g_key_file_set_integer(file, "Schedule", "TransitionMinutes",
                           app->settings.transition_minutes);
    g_key_file_set_integer(file, "Schedule", "DayStartMinute",
                           app->settings.custom_day_minute);
    g_key_file_set_integer(file, "Schedule", "NightStartMinute",
                           app->settings.custom_night_minute);
    g_key_file_set_integer(file, "Color", "DayTemperature",
                           app->settings.day_temperature);
    g_key_file_set_integer(file, "Color", "NightTemperature",
                           app->settings.night_temperature);
    g_key_file_set_double(file, "Color", "DayBrightness",
                          app->settings.day_brightness);
    g_key_file_set_double(file, "Color", "NightBrightness",
                          app->settings.night_brightness);
    g_key_file_set_double(file, "Color", "Strength", app->settings.strength);
    g_key_file_set_double(file, "Location", "Latitude", app->settings.latitude);
    g_key_file_set_double(file, "Location", "Longitude", app->settings.longitude);
    g_key_file_set_boolean(file, "Location", "Configured",
                           app->settings.solar_location_set);
    g_key_file_set_boolean(file, "Displays", "LimitToSelected",
                           app->settings.limit_outputs);

    outputs = g_ptr_array_new();
    g_hash_table_iter_init(&iterator, app->settings.selected_outputs);
    while (g_hash_table_iter_next(&iterator, &key, NULL)) {
        g_ptr_array_add(outputs, key);
    }
    g_ptr_array_add(outputs, NULL);
    g_key_file_set_string_list(file, "Displays", "Selected",
                               (const gchar * const *)outputs->pdata,
                               outputs->len - 1);
    g_ptr_array_unref(outputs);

    data = g_key_file_to_data(file, &data_size, NULL);
    directory = g_path_get_dirname(app->config_path);
    if (g_mkdir_with_parents(directory, 0700) != 0 ||
        !g_file_set_contents(app->config_path, data, data_size, NULL)) {
        g_warning("Could not save settings to %s", app->config_path);
    }
    g_free(directory);
    g_free(data);
    g_key_file_unref(file);
}

static void
migrate_legacy_config(const gchar *destination)
{
    gchar *source = g_build_filename(g_get_user_config_dir(), "xfceshift",
                                     "config.ini", NULL);
    gchar *contents = NULL;
    gchar *directory;
    gsize length = 0;

    if (g_file_test(destination, G_FILE_TEST_EXISTS) ||
        !g_file_test(source, G_FILE_TEST_IS_REGULAR)) {
        g_free(source);
        return;
    }
    if (!g_file_get_contents(source, &contents, &length, NULL)) {
        g_warning("Could not read legacy settings from %s", source);
        g_free(source);
        return;
    }

    directory = g_path_get_dirname(destination);
    if (g_mkdir_with_parents(directory, 0700) != 0 ||
        !g_file_set_contents(destination, contents, length, NULL)) {
        g_warning("Could not migrate settings to %s", destination);
    } else {
        g_message("Imported settings from %s", source);
    }
    g_free(directory);
    g_free(contents);
    g_free(source);
}

static gchar *
autostart_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "autostart",
                            "x11shift.desktop", NULL);
}

static gboolean
set_autostart(gboolean enabled, GError **error)
{
    static const char desktop_file[] =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=X11Shift\n"
        "Comment=Adjust screen color temperature in the background\n"
        "Exec=x11shift --background\n"
        "Icon=x11shift\n"
        "Terminal=false\n"
        "X-GNOME-Autostart-enabled=true\n";
    gchar *path = autostart_path();
    gchar *directory = g_path_get_dirname(path);
    gboolean success = TRUE;

    if (enabled) {
        if (g_mkdir_with_parents(directory, 0700) != 0) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        "Could not create %s", directory);
            success = FALSE;
        } else {
            success = g_file_set_contents(path, desktop_file, -1, error);
        }
    } else if (g_file_test(path, G_FILE_TEST_EXISTS) && g_unlink(path) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Could not remove %s", path);
        success = FALSE;
    }
    g_free(directory);
    g_free(path);
    return success;
}

static gchar *
legacy_autostart_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "autostart",
                            "xfceshift.desktop", NULL);
}

static void
migrate_legacy_autostart(void)
{
    gchar *legacy_path = legacy_autostart_path();
    gchar *new_path = autostart_path();
    GError *error = NULL;
    gboolean replacement_ready = TRUE;

    if (!g_file_test(legacy_path, G_FILE_TEST_EXISTS)) {
        g_free(new_path);
        g_free(legacy_path);
        return;
    }
    if (!g_file_test(new_path, G_FILE_TEST_EXISTS)) {
        replacement_ready = set_autostart(TRUE, &error);
    }
    if (replacement_ready) {
        if (g_unlink(legacy_path) != 0) {
            g_warning("Could not remove legacy autostart entry %s", legacy_path);
        }
    } else {
        g_warning("Could not migrate autostart: %s",
                  error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
    }
    g_free(new_path);
    g_free(legacy_path);
}

static GtkWidget *
make_heading(const char *text)
{
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_margin_top(label, 8);
    g_free(markup);
    return label;
}

static gboolean
block_control_scroll(GtkWidget *widget, GdkEventScroll *event,
                     gpointer user_data)
{
    (void)widget;
    (void)event;
    (void)user_data;
    return GDK_EVENT_STOP;
}

static GtkWidget *
make_scale(double minimum, double maximum, double step, double value,
           int digits)
{
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                minimum, maximum, step);
    gtk_range_set_value(GTK_RANGE(scale), value);
    gtk_scale_set_digits(GTK_SCALE(scale), digits);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_widget_set_size_request(scale, 280, -1);
    g_signal_connect(scale, "scroll-event",
                     G_CALLBACK(block_control_scroll), NULL);
    return scale;
}

static void
grid_add_row(GtkWidget *grid, int row, const char *label_text,
             GtkWidget *control)
{
    GtkWidget *label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
}

static void
update_schedule_visibility(XsApp *app)
{
    gtk_widget_set_visible(app->custom_grid,
        app->settings.mode == XS_SCHEDULE_CUSTOM);
    gtk_widget_set_visible(app->solar_grid,
        app->settings.mode == XS_SCHEDULE_SOLAR);
}

static gboolean
read_time_entry(GtkWidget *entry, int *minutes)
{
    GtkStyleContext *context = gtk_widget_get_style_context(entry);
    gboolean valid = xs_parse_time(gtk_entry_get_text(GTK_ENTRY(entry)), minutes);
    if (valid) {
        gtk_style_context_remove_class(context, "error");
    } else {
        gtk_style_context_add_class(context, "error");
    }
    return valid;
}

static void
read_display_selection(XsApp *app)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->display_box));
    GList *item;
    guint active_count = 0;
    guint total_count = 0;

    g_hash_table_remove_all(app->settings.selected_outputs);
    for (item = children; item != NULL; item = item->next) {
        GtkWidget *check = item->data;
        const gchar *name;
        if (!GTK_IS_TOGGLE_BUTTON(check)) {
            continue;
        }
        total_count++;
        if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check))) {
            continue;
        }
        active_count++;
        name = g_object_get_data(G_OBJECT(check), "output-name");
        if (name != NULL) {
            g_hash_table_add(app->settings.selected_outputs, g_strdup(name));
        }
    }
    app->settings.limit_outputs = total_count > 0 && active_count < total_count;
    g_list_free(children);
}

static void
settings_changed(GtkWidget *widget, gpointer user_data)
{
    XsApp *app = user_data;
    const gchar *mode_id;
    int day_minute;
    int night_minute;
    gboolean valid_times;

    (void)widget;
    if (app->updating_widgets) {
        return;
    }
    app->settings.enabled = gtk_switch_get_active(
        GTK_SWITCH(app->enabled_switch));
    mode_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->mode_combo));
    app->settings.mode = mode_from_string(mode_id);
    app->settings.day_temperature = (int)lrint(
        gtk_range_get_value(GTK_RANGE(app->day_temperature)));
    app->settings.night_temperature = (int)lrint(
        gtk_range_get_value(GTK_RANGE(app->night_temperature)));
    app->settings.day_brightness =
        gtk_range_get_value(GTK_RANGE(app->day_brightness)) / 100.0;
    app->settings.night_brightness =
        gtk_range_get_value(GTK_RANGE(app->night_brightness)) / 100.0;
    app->settings.strength =
        gtk_range_get_value(GTK_RANGE(app->strength)) / 100.0;
    app->settings.transition_minutes = (int)lrint(
        gtk_range_get_value(GTK_RANGE(app->transition)));
    app->settings.latitude = gtk_spin_button_get_value(
        GTK_SPIN_BUTTON(app->latitude));
    app->settings.longitude = gtk_spin_button_get_value(
        GTK_SPIN_BUTTON(app->longitude));
    valid_times = read_time_entry(app->custom_day_entry, &day_minute) &
                  read_time_entry(app->custom_night_entry, &night_minute);
    if (valid_times) {
        app->settings.custom_day_minute = day_minute;
        app->settings.custom_night_minute = night_minute;
    }
    read_display_selection(app);
    update_schedule_visibility(app);
    settings_save(app);
    app_update(app);
}

static void
location_changed(GtkWidget *widget, gpointer user_data)
{
    XsApp *app = user_data;

    if (app->updating_widgets) {
        return;
    }
    app->settings.solar_location_set = TRUE;
    settings_changed(widget, user_data);
}

static void
autostart_changed(GtkToggleButton *button, gpointer user_data)
{
    XsApp *app = user_data;
    GError *error = NULL;
    gboolean enabled;

    if (app->updating_widgets) {
        return;
    }
    enabled = gtk_toggle_button_get_active(button);
    if (!set_autostart(enabled, &error)) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
            GTK_BUTTONS_CLOSE, "Could not update autostart");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                                 "%s", error->message);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_clear_error(&error);
        app->updating_widgets = TRUE;
        gtk_toggle_button_set_active(button, !enabled);
        app->updating_widgets = FALSE;
    }
}

static double
interpolate_temperature(int night_temperature, int day_temperature,
                        double daylight_factor)
{
    double night_mired = 1000000.0 / night_temperature;
    double day_mired = 1000000.0 / day_temperature;
    double mired = night_mired +
                   (day_mired - night_mired) * daylight_factor;
    return 1000000.0 / mired;
}

static void
set_status(XsApp *app, const char *status, const char *detail)
{
    gchar *markup = g_markup_printf_escaped(
        "<span size=\"x-large\"><b>%s</b></span>", status);
    gtk_label_set_markup(GTK_LABEL(app->status_label), markup);
    gtk_label_set_text(GTK_LABEL(app->detail_label), detail);
    if (app->status_icon != NULL) {
        gchar *tooltip = g_strdup_printf("X11Shift — %s\n%s", status, detail);
        gtk_status_icon_set_tooltip_text(app->status_icon, tooltip);
        g_free(tooltip);
    }
    g_free(markup);
}

static void
app_update(XsApp *app)
{
    XsScheduleResult schedule;
    XsScheduleMode effective_mode;
    time_t now = time(NULL);
    double temperature;
    double brightness;
    gchar detail[256];
    const char *status;
    GError *error = NULL;
    gboolean using_custom_fallback;

    if (app->window == NULL) {
        return;
    }
    app->updating_widgets = TRUE;
    gtk_switch_set_active(GTK_SWITCH(app->enabled_switch),
                          app->settings.enabled);
    if (app->tray_enabled_item != NULL) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->tray_enabled_item),
                                       app->settings.enabled);
    }
    app->updating_widgets = FALSE;

    if (!app->settings.enabled) {
        xs_gamma_controller_restore(app->gamma);
        set_status(app, "Color adjustment is off",
                   "The original display gamma ramps have been restored.");
        return;
    }
    if (app->gamma == NULL) {
        set_status(app, "X11 display control unavailable",
                   "Run X11Shift inside an X11 session.");
        return;
    }
    if (app->settings.limit_outputs &&
        g_hash_table_size(app->settings.selected_outputs) == 0) {
        xs_gamma_controller_restore(app->gamma);
        set_status(app, "No displays selected",
                   "Select at least one display below to apply color adjustment.");
        return;
    }

    using_custom_fallback = app->settings.mode == XS_SCHEDULE_SOLAR &&
                            !app->settings.solar_location_set;
    effective_mode = using_custom_fallback
        ? XS_SCHEDULE_CUSTOM : app->settings.mode;
    schedule = xs_schedule_calculate(
        effective_mode, now,
        app->settings.custom_day_minute,
        app->settings.custom_night_minute,
        app->settings.latitude,
        app->settings.longitude,
        app->settings.transition_minutes);
    if (app->paused_until > now) {
        schedule.daylight_factor = 1.0;
    }
    temperature = interpolate_temperature(
        app->settings.night_temperature,
        app->settings.day_temperature,
        schedule.daylight_factor);
    brightness = app->settings.night_brightness +
        (app->settings.day_brightness - app->settings.night_brightness) *
        schedule.daylight_factor;

    if (!xs_gamma_controller_apply(
            app->gamma, temperature, brightness, app->settings.strength,
            app->settings.limit_outputs, app->settings.selected_outputs,
            &error)) {
        set_status(app, "Could not adjust the display", error->message);
        g_clear_error(&error);
        return;
    }

    if (app->paused_until > now) {
        int minutes_left = (int)ceil((app->paused_until - now) / 60.0);
        g_snprintf(detail, sizeof(detail),
                   "%d K · %d%% brightness · daylight colors for %d more min",
                   (int)lrint(temperature), (int)lrint(brightness * 100.0),
                   minutes_left);
        set_status(app, "Daylight colors temporarily", detail);
        return;
    } else if (using_custom_fallback) {
        char day_start[6];
        char night_start[6];
        xs_format_time(app->settings.custom_day_minute, day_start);
        xs_format_time(app->settings.custom_night_minute, night_start);
        g_snprintf(detail, sizeof(detail),
                   "%d K · %d%% brightness · location unset; using %s–%s",
                   (int)lrint(temperature), (int)lrint(brightness * 100.0),
                   day_start, night_start);
    } else if (app->settings.mode == XS_SCHEDULE_SOLAR &&
               schedule.solar_times_valid) {
        char sunrise[6];
        char sunset[6];
        xs_format_time((int)lrint(schedule.sunrise_minute), sunrise);
        xs_format_time((int)lrint(schedule.sunset_minute), sunset);
        g_snprintf(detail, sizeof(detail),
                   "%d K · %d%% brightness · sunrise %s · sunset %s",
                   (int)lrint(temperature), (int)lrint(brightness * 100.0),
                   sunrise, sunset);
    } else {
        g_snprintf(detail, sizeof(detail), "%d K · %d%% brightness",
                   (int)lrint(temperature), (int)lrint(brightness * 100.0));
    }

    if (schedule.transition != XS_TRANSITION_NONE) {
        gchar transition_detail[384];
        int percent = (int)floor(schedule.transition_progress * 100.0);
        int minutes_left = (int)ceil(schedule.transition_remaining_seconds / 60.0);
        status = schedule.transition == XS_TRANSITION_TO_DAY
            ? "Transitioning to daylight" : "Transitioning to night";
        g_snprintf(transition_detail, sizeof(transition_detail),
                   "%s\nGradually changing colors · %d%% complete · %d min remaining",
                   detail, percent, minutes_left);
        set_status(app, status, transition_detail);
    } else {
        status = schedule.daylight_factor < 0.5
            ? "Night colors active" : "Daylight colors active";
        set_status(app, status, detail);
    }
}

static gboolean
timer_callback(gpointer user_data)
{
    app_update(user_data);
    return G_SOURCE_CONTINUE;
}

static void
resume_callback(gpointer user_data)
{
    /* Recalculate from wall time now; do not replay a pre-suspend transition.
     * The regular timer also retries if the displays are not ready yet. */
    app_update(user_data);
}

static void
tray_embedding_changed(GObject *object, GParamSpec *parameter,
                       gpointer user_data)
{
    XsApp *app = user_data;
    gboolean embedded = gtk_status_icon_is_embedded(GTK_STATUS_ICON(object));

    (void)parameter;
    gtk_button_set_label(GTK_BUTTON(app->hide_button),
                         embedded ? "Hide to notification area" : "Hide window");
    gtk_widget_set_tooltip_text(
        app->hide_button,
        embedded
            ? "Keep X11Shift running in the desktop notification area"
            : "Reopen X11Shift from the application menu to show it again");
}

static void
show_window(XsApp *app)
{
    gtk_widget_show(app->window);
    gtk_window_present(GTK_WINDOW(app->window));
}

static gboolean
window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    (void)event;
    gtk_widget_hide(widget);
    gtk_status_icon_set_visible(((XsApp *)user_data)->status_icon, TRUE);
    return TRUE;
}

static void
hide_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    gtk_widget_hide(((XsApp *)user_data)->window);
}

static void
quit_app(XsApp *app)
{
    settings_save(app);
    xs_gamma_controller_restore(app->gamma);
    g_application_quit(G_APPLICATION(app->application));
}

static void
quit_clicked(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    quit_app(user_data);
}

static void
restore_clicked(GtkWidget *widget, gpointer user_data)
{
    XsApp *app = user_data;
    (void)widget;
    app->settings.enabled = FALSE;
    app->paused_until = 0;
    settings_save(app);
    app_update(app);
}

static void
tray_activate(GtkStatusIcon *icon, gpointer user_data)
{
    (void)icon;
    show_window(user_data);
}

static void
tray_enabled_toggled(GtkCheckMenuItem *item, gpointer user_data)
{
    XsApp *app = user_data;
    if (app->updating_widgets) {
        return;
    }
    app->settings.enabled = gtk_check_menu_item_get_active(item);
    settings_save(app);
    app_update(app);
}

static void
pause_one_hour(GtkWidget *item, gpointer user_data)
{
    XsApp *app = user_data;
    (void)item;
    app->paused_until = time(NULL) + 60 * 60;
    app_update(app);
}

static void
tray_popup(GtkStatusIcon *icon, guint button, guint activate_time,
           gpointer user_data)
{
    XsApp *app = user_data;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *show = gtk_menu_item_new_with_label("Open X11Shift");
    GtkWidget *enabled = gtk_check_menu_item_new_with_label("Color adjustment enabled");
    GtkWidget *pause = gtk_menu_item_new_with_label("Use daylight colors for 1 hour");
    GtkWidget *restore = gtk_menu_item_new_with_label("Restore original colors");
    GtkWidget *quit = gtk_menu_item_new_with_label("Quit");

    (void)icon;
    app->tray_enabled_item = enabled;
    app->updating_widgets = TRUE;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(enabled),
                                   app->settings.enabled);
    app->updating_widgets = FALSE;
    g_signal_connect_swapped(show, "activate", G_CALLBACK(show_window), app);
    g_signal_connect(enabled, "toggled", G_CALLBACK(tray_enabled_toggled), app);
    g_signal_connect(pause, "activate", G_CALLBACK(pause_one_hour), app);
    g_signal_connect(restore, "activate", G_CALLBACK(restore_clicked), app);
    g_signal_connect(quit, "activate", G_CALLBACK(quit_clicked), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), show);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), enabled);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), pause);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), restore);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);
    g_signal_connect(menu, "deactivate", G_CALLBACK(gtk_widget_destroy), NULL);
    g_signal_connect_swapped(menu, "destroy",
                             G_CALLBACK(g_nullify_pointer),
                             &app->tray_enabled_item);
    gtk_widget_show_all(menu);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL,
                   gtk_status_icon_position_menu, app->status_icon,
                   button, activate_time);
}

static void
connect_setting_signal(GtkWidget *widget, const char *signal, XsApp *app)
{
    g_signal_connect(widget, signal, G_CALLBACK(settings_changed), app);
}

static GtkWidget *
build_schedule_section(XsApp *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *mode = gtk_combo_box_text_new();
    GtkWidget *custom = gtk_grid_new();
    GtkWidget *solar = gtk_grid_new();
    GtkWidget *latitude;
    GtkWidget *longitude;
    char day_time[6];
    char night_time[6];

    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mode), "solar",
                              "Sunrise to sunset");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mode), "custom",
                              "Custom clock times");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mode), "always-night",
                              "Always use night colors");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mode), "always-day",
                              "Always use daylight colors");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(mode),
                                mode_to_string(app->settings.mode));
    g_signal_connect(mode, "scroll-event",
                     G_CALLBACK(block_control_scroll), NULL);
    app->mode_combo = mode;
    grid_add_row(grid, 0, "Schedule", mode);

    gtk_grid_set_column_spacing(GTK_GRID(custom), 18);
    gtk_grid_set_row_spacing(GTK_GRID(custom), 8);
    xs_format_time(app->settings.custom_day_minute, day_time);
    xs_format_time(app->settings.custom_night_minute, night_time);
    app->custom_day_entry = gtk_entry_new();
    app->custom_night_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(app->custom_day_entry), day_time);
    gtk_entry_set_text(GTK_ENTRY(app->custom_night_entry), night_time);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->custom_day_entry), "07:00");
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->custom_night_entry), "19:00");
    gtk_entry_set_max_length(GTK_ENTRY(app->custom_day_entry), 5);
    gtk_entry_set_max_length(GTK_ENTRY(app->custom_night_entry), 5);
    grid_add_row(custom, 0, "Daylight colors start", app->custom_day_entry);
    grid_add_row(custom, 1, "Night colors start", app->custom_night_entry);
    app->custom_grid = custom;

    gtk_grid_set_column_spacing(GTK_GRID(solar), 18);
    gtk_grid_set_row_spacing(GTK_GRID(solar), 8);
    latitude = gtk_spin_button_new_with_range(-89.8, 89.8, 0.1);
    longitude = gtk_spin_button_new_with_range(-180.0, 180.0, 0.1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(latitude), 4);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(longitude), 4);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(latitude), app->settings.latitude);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(longitude), app->settings.longitude);
    g_signal_connect(latitude, "scroll-event",
                     G_CALLBACK(block_control_scroll), NULL);
    g_signal_connect(longitude, "scroll-event",
                     G_CALLBACK(block_control_scroll), NULL);
    gtk_widget_set_tooltip_text(latitude, "North is positive; south is negative");
    gtk_widget_set_tooltip_text(longitude, "East is positive; west is negative");
    app->latitude = latitude;
    app->longitude = longitude;
    grid_add_row(solar, 0, "Latitude", latitude);
    grid_add_row(solar, 1, "Longitude", longitude);
    app->solar_grid = solar;

    app->transition = make_scale(0, 180, 5,
                                 app->settings.transition_minutes, 0);
    gtk_scale_add_mark(GTK_SCALE(app->transition), 0, GTK_POS_BOTTOM, "Instant");
    gtk_scale_add_mark(GTK_SCALE(app->transition), 60, GTK_POS_BOTTOM, "60 min");
    gtk_scale_add_mark(GTK_SCALE(app->transition), 180, GTK_POS_BOTTOM, "180 min");
    grid_add_row(grid, 1, "Transition", app->transition);

    gtk_box_pack_start(GTK_BOX(box), make_heading("Schedule"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), custom, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), solar, FALSE, FALSE, 0);
    return box;
}

static void
restore_color_defaults_clicked(GtkButton *button, gpointer user_data)
{
    XsApp *app = user_data;

    (void)button;
    app->updating_widgets = TRUE;
    gtk_range_set_value(GTK_RANGE(app->day_temperature),
                        DEFAULT_DAY_TEMPERATURE);
    gtk_range_set_value(GTK_RANGE(app->night_temperature),
                        DEFAULT_NIGHT_TEMPERATURE);
    gtk_range_set_value(GTK_RANGE(app->day_brightness),
                        DEFAULT_DAY_BRIGHTNESS * 100.0);
    gtk_range_set_value(GTK_RANGE(app->night_brightness),
                        DEFAULT_NIGHT_BRIGHTNESS * 100.0);
    gtk_range_set_value(GTK_RANGE(app->strength),
                        DEFAULT_COLOR_STRENGTH * 100.0);
    app->updating_widgets = FALSE;
    settings_changed(NULL, app);
}

static GtkWidget *
build_color_section(XsApp *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *heading_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *heading = make_heading("Color and brightness");
    GtkWidget *defaults = gtk_button_new_with_label("Defaults");

    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    app->day_temperature = make_scale(2500, 7500, 100,
                                      app->settings.day_temperature, 0);
    app->night_temperature = make_scale(1000, 6500, 100,
                                        app->settings.night_temperature, 0);
    app->day_brightness = make_scale(20, 100, 1,
                                     app->settings.day_brightness * 100, 0);
    app->night_brightness = make_scale(20, 100, 1,
                                       app->settings.night_brightness * 100, 0);
    app->strength = make_scale(0, 100, 1, app->settings.strength * 100, 0);
    gtk_widget_set_tooltip_text(app->strength,
        "Blend between the original colors and the selected temperature");
    grid_add_row(grid, 0, "Day temperature (K)", app->day_temperature);
    grid_add_row(grid, 1, "Night temperature (K)", app->night_temperature);
    grid_add_row(grid, 2, "Day brightness (%)", app->day_brightness);
    grid_add_row(grid, 3, "Night brightness (%)", app->night_brightness);
    grid_add_row(grid, 4, "Color strength (%)", app->strength);
    gtk_widget_set_hexpand(heading, TRUE);
    gtk_widget_set_halign(defaults, GTK_ALIGN_END);
    gtk_widget_set_valign(defaults, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(
        defaults, "Restore default color temperatures, brightness, and strength");
    gtk_box_pack_start(GTK_BOX(heading_row), heading, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(heading_row), defaults, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), heading_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    g_signal_connect(defaults, "clicked",
                     G_CALLBACK(restore_color_defaults_clicked), app);
    return box;
}

static GtkWidget *
build_display_section(XsApp *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GPtrArray *outputs;
    guint index;

    app->display_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    outputs = xs_gamma_controller_get_outputs(app->gamma);
    if (outputs->len == 0) {
        GtkWidget *label = gtk_label_new("No adjustable X11 displays detected");
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_box_pack_start(GTK_BOX(app->display_box), label, FALSE, FALSE, 0);
    }
    for (index = 0; index < outputs->len; index++) {
        const gchar *name = g_ptr_array_index(outputs, index);
        GtkWidget *check = gtk_check_button_new_with_label(name);
        gboolean selected = !app->settings.limit_outputs ||
            g_hash_table_contains(app->settings.selected_outputs, name);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), selected);
        g_object_set_data_full(G_OBJECT(check), "output-name",
                               g_strdup(name), g_free);
        gtk_box_pack_start(GTK_BOX(app->display_box), check, FALSE, FALSE, 0);
        connect_setting_signal(check, "toggled", app);
    }
    g_ptr_array_unref(outputs);
    gtk_box_pack_start(GTK_BOX(box), make_heading("Displays"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), app->display_box, FALSE, FALSE, 0);
    return box;
}

static void
install_css(void)
{
    static const char css[] =
        ".x11shift-status { background: alpha(@theme_selected_bg_color, 0.10); "
        "border-radius: 8px; padding: 14px; }\n"
        "entry.error { border-color: #d33; }\n";
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static GdkPixbuf *
load_embedded_icon(int size)
{
    GError *error = NULL;
    GdkPixbuf *icon = gdk_pixbuf_new_from_resource_at_scale(
        "/io/x11shift/X11Shift/x11shift.svg", size, size, TRUE, &error);

    if (icon == NULL) {
        g_warning("Could not load the embedded X11Shift icon: %s",
                  error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
    }
    return icon;
}

static void
build_window(XsApp *app)
{
    GtkWidget *outer;
    GtkWidget *status_box;
    GtkWidget *header;
    GtkWidget *header_label;
    GtkWidget *content;
    GtkWidget *buttons;
    GtkWidget *hide;
    GtkWidget *quit;
    GdkPixbuf *window_icon;
    gchar *auto_path;

    install_css();
    app->window = gtk_application_window_new(app->application);
    gtk_window_set_title(GTK_WINDOW(app->window), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(app->window), 620, -1);
    window_icon = load_embedded_icon(64);
    if (window_icon != NULL) {
        gtk_window_set_icon(GTK_WINDOW(app->window), window_icon);
        g_object_unref(window_icon);
    } else {
        gtk_window_set_icon_name(GTK_WINDOW(app->window), "x11shift");
    }
    g_signal_connect(app->window, "delete-event", G_CALLBACK(window_delete), app);

    outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app->window), outer);
    header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(header, 18);
    gtk_widget_set_margin_end(header, 18);
    gtk_widget_set_margin_top(header, 14);
    gtk_widget_set_margin_bottom(header, 14);
    header_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(header_label),
                         "<span size=\"large\"><b>X11Shift</b></span>");
    gtk_label_set_xalign(GTK_LABEL(header_label), 0.0f);
    gtk_widget_set_hexpand(header_label, TRUE);
    app->enabled_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(app->enabled_switch), app->settings.enabled);
    gtk_widget_set_valign(app->enabled_switch, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(header), header_label, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(header), app->enabled_switch, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(content, 22);
    gtk_widget_set_margin_end(content, 22);
    gtk_widget_set_margin_top(content, 18);
    gtk_widget_set_margin_bottom(content, 18);
    gtk_box_pack_start(GTK_BOX(outer), content, TRUE, TRUE, 0);

    status_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_style_context_add_class(gtk_widget_get_style_context(status_box),
                                "x11shift-status");
    app->status_label = gtk_label_new("");
    app->detail_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->status_label), 0.0f);
    gtk_label_set_xalign(GTK_LABEL(app->detail_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(app->detail_label), TRUE);
    gtk_box_pack_start(GTK_BOX(status_box), app->status_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(status_box), app->detail_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), status_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), build_schedule_section(app),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), build_color_section(app),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), build_display_section(app),
                       FALSE, FALSE, 0);

    app->autostart_check = gtk_check_button_new_with_label(
        "Start X11Shift automatically when I log in");
    auto_path = autostart_path();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->autostart_check),
                                 g_file_test(auto_path, G_FILE_TEST_EXISTS));
    g_free(auto_path);
    gtk_box_pack_start(GTK_BOX(content), app->autostart_check, FALSE, FALSE, 4);

    buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END);
    gtk_box_set_spacing(GTK_BOX(buttons), 8);
    hide = gtk_button_new_with_label("Hide to tray");
    app->hide_button = hide;
    quit = gtk_button_new_with_label("Restore colors and quit");
    gtk_container_add(GTK_CONTAINER(buttons), hide);
    gtk_container_add(GTK_CONTAINER(buttons), quit);
    gtk_box_pack_start(GTK_BOX(content), buttons, FALSE, FALSE, 6);
    g_signal_connect(hide, "clicked", G_CALLBACK(hide_clicked), app);
    g_signal_connect(quit, "clicked", G_CALLBACK(quit_clicked), app);

    g_signal_connect(app->enabled_switch, "notify::active",
                     G_CALLBACK(enabled_switch_changed), app);
    connect_setting_signal(app->mode_combo, "changed", app);
    connect_setting_signal(app->day_temperature, "value-changed", app);
    connect_setting_signal(app->night_temperature, "value-changed", app);
    connect_setting_signal(app->day_brightness, "value-changed", app);
    connect_setting_signal(app->night_brightness, "value-changed", app);
    connect_setting_signal(app->strength, "value-changed", app);
    connect_setting_signal(app->transition, "value-changed", app);
    connect_setting_signal(app->custom_day_entry, "changed", app);
    connect_setting_signal(app->custom_night_entry, "changed", app);
    g_signal_connect(app->latitude, "value-changed",
                     G_CALLBACK(location_changed), app);
    g_signal_connect(app->longitude, "value-changed",
                     G_CALLBACK(location_changed), app);
    g_signal_connect(app->autostart_check, "toggled",
                     G_CALLBACK(autostart_changed), app);
    update_schedule_visibility(app);
    gtk_widget_show_all(outer);
    update_schedule_visibility(app);
}

static void
application_activate(GtkApplication *application, gpointer user_data)
{
    XsApp *app = user_data;
    GdkPixbuf *tray_icon;
    GError *error = NULL;

    if (app->window == NULL) {
        g_application_hold(G_APPLICATION(app->application));
        app->gamma = xs_gamma_controller_new(&error);
        if (error != NULL) {
            g_warning("%s", error->message);
            g_clear_error(&error);
        }
        build_window(app);
        tray_icon = load_embedded_icon(22);
        app->status_icon = tray_icon != NULL
            ? gtk_status_icon_new_from_pixbuf(tray_icon)
            : gtk_status_icon_new_from_icon_name("x11shift");
        if (tray_icon != NULL) {
            g_object_unref(tray_icon);
        }
        gtk_status_icon_set_visible(app->status_icon, TRUE);
        g_signal_connect(app->status_icon, "notify::embedded",
                         G_CALLBACK(tray_embedding_changed), app);
        tray_embedding_changed(G_OBJECT(app->status_icon), NULL, app);
        g_signal_connect(app->status_icon, "activate",
                         G_CALLBACK(tray_activate), app);
        g_signal_connect(app->status_icon, "popup-menu",
                         G_CALLBACK(tray_popup), app);
        app->timer_id = g_timeout_add_seconds(UPDATE_INTERVAL_SECONDS,
                                              timer_callback, app);
        app->resume_monitor = xs_resume_monitor_new(resume_callback, app, &error);
        if (error != NULL) {
            g_debug("Resume notifications unavailable: %s", error->message);
            g_clear_error(&error);
        }
        app_update(app);
    }
    if (app->first_activation && app->start_hidden) {
        gtk_widget_hide(app->window);
    } else {
        show_window(app);
    }
    app->first_activation = FALSE;
    (void)application;
}

static gboolean
unix_quit_signal(gpointer user_data)
{
    quit_app(user_data);
    return G_SOURCE_REMOVE;
}

static void
print_usage(const char *program)
{
    g_print("Usage: %s [OPTION]\n\n", program);
    g_print("Launch the X11Shift graphical settings window.\n\n");
    g_print("  --background   start hidden in the notification area\n");
    g_print("  --reset        reset all XRandR gamma ramps to linear and exit\n");
    g_print("  --version      show the version and exit\n");
    g_print("  --help         show this help and exit\n");
}

int
main(int argc, char **argv)
{
    XsApp app = {0};
    int index;
    int status;
    char *application_argv[2] = {argv[0], NULL};

    app.first_activation = TRUE;
    for (index = 1; index < argc; index++) {
        if (g_strcmp0(argv[index], "--background") == 0) {
            app.start_hidden = TRUE;
        } else if (g_strcmp0(argv[index], "--version") == 0) {
            g_print("X11Shift %s\n", APP_VERSION);
            return EXIT_SUCCESS;
        } else if (g_strcmp0(argv[index], "--help") == 0 ||
                   g_strcmp0(argv[index], "-h") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (g_strcmp0(argv[index], "--reset") == 0) {
            GError *error = NULL;
            XsGammaController *gamma = xs_gamma_controller_new(&error);
            if (gamma == NULL) {
                g_printerr("x11shift: %s\n", error->message);
                g_clear_error(&error);
                return EXIT_FAILURE;
            }
            xs_gamma_controller_reset_linear(gamma);
            xs_gamma_controller_free(gamma);
            return EXIT_SUCCESS;
        } else {
            g_printerr("x11shift: unknown option: %s\n", argv[index]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    app.config_path = g_build_filename(g_get_user_config_dir(), "x11shift",
                                       "config.ini", NULL);
    migrate_legacy_config(app.config_path);
    migrate_legacy_autostart();
    settings_load(&app);
    app.application = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app.application, "activate",
                     G_CALLBACK(application_activate), &app);
    g_unix_signal_add(SIGINT, unix_quit_signal, &app);
    g_unix_signal_add(SIGTERM, unix_quit_signal, &app);
    status = g_application_run(G_APPLICATION(app.application), 1,
                               application_argv);

    if (app.timer_id != 0) {
        g_source_remove(app.timer_id);
    }
    xs_resume_monitor_free(app.resume_monitor);
    xs_gamma_controller_free(app.gamma);
    if (app.status_icon != NULL) {
        g_object_unref(app.status_icon);
    }
    g_hash_table_unref(app.settings.selected_outputs);
    g_clear_object(&app.application);
    g_free(app.config_path);
    return status;
}
