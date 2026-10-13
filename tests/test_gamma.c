#include "x11_gamma.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#include <string.h>

/* A small fake X server lets us retain the night ramp while changing the
 * display configuration, as a real driver can do during suspend/resume. */
#define RAMP_SIZE 4
#define LUT_SIZE 8
#define LUT_ATOM 30
#define LUT_SIZE_ATOM 31
#define BASELINE_ATOM 32
#define APPLIED_ATOM 33
#define PREVIOUS_ATOM 34

typedef struct {
    unsigned short data[LUT_SIZE * 4];
    unsigned long length;
} SavedLut;

static SavedLut saved_luts[3];

static Time config_timestamp;
static gboolean connected;
static gboolean native_lut_supported;
static gboolean malformed_lut;
static gboolean legacy_gamma_supported;
static unsigned short native_lut[LUT_SIZE * 4];
static unsigned long native_lut_length;
static guint native_writes;
static guint legacy_writes;
static unsigned short current_ramp[3][RAMP_SIZE];
static const unsigned short original_ramp[3][RAMP_SIZE] = {
    {0, 21000, 42000, 63000},
    {0, 20000, 40000, 60000},
    {0, 19000, 38000, 57000}
};

Display *
XOpenDisplay(const char *name)
{
    _XPrivDisplay display = g_malloc0(sizeof(*display));
    (void)name;
    display->screens = g_new0(Screen, 1);
    display->screens[0].root = 1;
    return (Display *)display;
}

int
XCloseDisplay(Display *display)
{
    g_free(((_XPrivDisplay)display)->screens);
    g_free(display);
    return 0;
}

int
XFlush(Display *display)
{
    (void)display;
    return 0;
}

int
XFree(void *data)
{
    g_free(data);
    return 0;
}

Atom
XInternAtom(Display *display, const char *name, Bool only_if_exists)
{
    (void)display;
    if (g_str_has_prefix(name, "_X11SHIFT_NATIVE_")) {
        g_assert_false(only_if_exists);
        if (g_str_has_prefix(name, "_X11SHIFT_NATIVE_BASELINE_")) {
            return BASELINE_ATOM;
        }
        if (g_str_has_prefix(name, "_X11SHIFT_NATIVE_APPLIED_")) {
            return APPLIED_ATOM;
        }
        g_assert_true(g_str_has_prefix(name, "_X11SHIFT_NATIVE_PREVIOUS_"));
        return PREVIOUS_ATOM;
    }
    g_assert_true(only_if_exists);
    if (!native_lut_supported) {
        return None;
    }
    return strcmp(name, "GAMMA_LUT") == 0 ? LUT_ATOM : LUT_SIZE_ATOM;
}

static SavedLut *
saved_lut(Atom property)
{
    g_assert_cmpuint(property, >=, BASELINE_ATOM);
    g_assert_cmpuint(property, <=, PREVIOUS_ATOM);
    return &saved_luts[property - BASELINE_ATOM];
}

int
XGetWindowProperty(Display *display, Window window, Atom property,
                   long offset, long requested_length, Bool delete,
                   Atom requested_type, Atom *actual_type, int *actual_format,
                   unsigned long *nitems, unsigned long *bytes_after,
                   unsigned char **data)
{
    SavedLut *saved = saved_lut(property);
    (void)display;
    g_assert_cmpuint(window, ==, 1);
    g_assert_cmpint(offset, ==, 0);
    g_assert_cmpint(requested_length, ==, LUT_SIZE * 2);
    g_assert_false(delete);
    g_assert_cmpuint(requested_type, ==, XA_INTEGER);
    *actual_type = saved->length > 0 ? XA_INTEGER : None;
    *actual_format = saved->length > 0 ? 16 : 0;
    *nitems = saved->length;
    *bytes_after = 0;
    *data = saved->length > 0
        ? g_memdup2(saved->data, saved->length * sizeof(unsigned short)) : NULL;
    return Success;
}

int
XChangeProperty(Display *display, Window window, Atom property, Atom type,
                int format, int mode, const unsigned char *data, int length)
{
    SavedLut *saved = saved_lut(property);
    (void)display;
    g_assert_cmpuint(window, ==, 1);
    g_assert_cmpuint(type, ==, XA_INTEGER);
    g_assert_cmpint(format, ==, 16);
    g_assert_cmpint(mode, ==, PropModeReplace);
    g_assert_true(length == 1 || length == LUT_SIZE * 4);
    saved->length = (unsigned long)length;
    memcpy(saved->data, data, length * sizeof(unsigned short));
    return 1;
}

int
XDeleteProperty(Display *display, Window window, Atom property)
{
    (void)display;
    g_assert_cmpuint(window, ==, 1);
    saved_lut(property)->length = 0;
    return 1;
}

int
XRRGetOutputProperty(Display *display, RROutput output, Atom property,
                     long offset, long requested_length, Bool delete,
                     Bool pending, Atom requested_type, Atom *actual_type,
                     int *actual_format, unsigned long *nitems,
                     unsigned long *bytes_after, unsigned char **data)
{
    (void)display;
    g_assert_true(connected);
    g_assert_cmpuint(output, ==, 10);
    g_assert_cmpint(offset, ==, 0);
    g_assert_false(delete);
    g_assert_false(pending);
    g_assert_cmpuint(requested_type, ==, XA_INTEGER);
    *actual_type = XA_INTEGER;
    *bytes_after = 0;
    if (property == LUT_SIZE_ATOM) {
        unsigned long *size = g_new(unsigned long, 1);
        g_assert_cmpint(requested_length, ==, 1);
        *size = LUT_SIZE;
        *actual_format = 32;
        *nitems = 1;
        *data = (unsigned char *)size;
    } else {
        g_assert_cmpuint(property, ==, LUT_ATOM);
        g_assert_cmpint(requested_length, ==, LUT_SIZE * 2);
        *actual_format = malformed_lut ? 32 : 16;
        *nitems = native_lut_length;
        *data = g_memdup2(native_lut, native_lut_length * sizeof(unsigned short));
    }
    return Success;
}

void
XRRChangeOutputProperty(Display *display, RROutput output, Atom property,
                        Atom type, int format, int mode,
                        const unsigned char *data, int length)
{
    (void)display;
    g_assert_true(connected);
    g_assert_cmpuint(output, ==, 10);
    g_assert_cmpuint(property, ==, LUT_ATOM);
    g_assert_cmpuint(type, ==, XA_INTEGER);
    g_assert_cmpint(format, ==, 16);
    g_assert_cmpint(mode, ==, PropModeReplace);
    g_assert_true(length == 1 || length == LUT_SIZE * 4);
    native_lut_length = (unsigned long)length;
    memcpy(native_lut, data, length * sizeof(unsigned short));
    native_writes++;
}

Bool
XRRQueryExtension(Display *display, int *event_base, int *error_base)
{
    (void)display;
    *event_base = 0;
    *error_base = 0;
    return True;
}

Status
XRRQueryVersion(Display *display, int *major, int *minor)
{
    (void)display;
    *major = 1;
    *minor = 3;
    return 1;
}

XRRScreenResources *
XRRGetScreenResourcesCurrent(Display *display, Window window)
{
    XRRScreenResources *resources = g_new0(XRRScreenResources, 1);
    (void)display;
    (void)window;
    resources->configTimestamp = config_timestamp;
    resources->noutput = 1;
    resources->outputs = g_new(RROutput, 1);
    resources->outputs[0] = 10;
    return resources;
}

void
XRRFreeScreenResources(XRRScreenResources *resources)
{
    g_free(resources->outputs);
    g_free(resources);
}

XRROutputInfo *
XRRGetOutputInfo(Display *display, XRRScreenResources *resources, RROutput output)
{
    XRROutputInfo *info = g_new0(XRROutputInfo, 1);
    (void)display;
    (void)resources;
    (void)output;
    info->connection = connected ? RR_Connected : RR_Disconnected;
    info->crtc = connected ? 20 : None;
    info->name = g_strdup("eDP");
    info->nameLen = 3;
    return info;
}

void
XRRFreeOutputInfo(XRROutputInfo *info)
{
    g_free(info->name);
    g_free(info);
}

int
XRRGetCrtcGammaSize(Display *display, RRCrtc crtc)
{
    (void)display;
    g_assert_cmpuint(crtc, ==, 20);
    return legacy_gamma_supported ? RAMP_SIZE : 0;
}

XRRCrtcGamma *
XRRAllocGamma(int size)
{
    XRRCrtcGamma *gamma = g_new0(XRRCrtcGamma, 1);
    gamma->size = size;
    gamma->red = g_new0(unsigned short, size);
    gamma->green = g_new0(unsigned short, size);
    gamma->blue = g_new0(unsigned short, size);
    return gamma;
}

void
XRRFreeGamma(XRRCrtcGamma *gamma)
{
    g_free(gamma->red);
    g_free(gamma->green);
    g_free(gamma->blue);
    g_free(gamma);
}

XRRCrtcGamma *
XRRGetCrtcGamma(Display *display, RRCrtc crtc)
{
    XRRCrtcGamma *gamma = XRRAllocGamma(XRRGetCrtcGammaSize(display, crtc));
    g_assert_true(connected);
    memcpy(gamma->red, current_ramp[0], sizeof(current_ramp[0]));
    memcpy(gamma->green, current_ramp[1], sizeof(current_ramp[1]));
    memcpy(gamma->blue, current_ramp[2], sizeof(current_ramp[2]));
    return gamma;
}

void
XRRSetCrtcGamma(Display *display, RRCrtc crtc, XRRCrtcGamma *gamma)
{
    (void)display;
    g_assert_true(connected);
    g_assert_cmpuint(crtc, ==, 20);
    g_assert_cmpint(gamma->size, ==, RAMP_SIZE);
    legacy_writes++;
    memcpy(current_ramp[0], gamma->red, sizeof(current_ramp[0]));
    memcpy(current_ramp[1], gamma->green, sizeof(current_ramp[1]));
    memcpy(current_ramp[2], gamma->blue, sizeof(current_ramp[2]));
}

static XsGammaController *
new_test_controller(gboolean native, gboolean calibrated,
                     gboolean legacy, gboolean malformed)
{
    GError *error = NULL;
    XsGammaController *controller;
    connected = TRUE;
    config_timestamp = 1;
    native_lut_supported = native;
    malformed_lut = malformed;
    legacy_gamma_supported = legacy;
    native_writes = 0;
    legacy_writes = 0;
    memset(saved_luts, 0, sizeof(saved_luts));
    memset(native_lut, 0, sizeof(native_lut));
    native_lut_length = calibrated ? LUT_SIZE * 4 : 1;
    if (calibrated) {
        int index;
        for (index = 0; index < LUT_SIZE; index++) {
            native_lut[index * 4] = 63000 * index / (LUT_SIZE - 1);
            native_lut[index * 4 + 1] = 60000 * index / (LUT_SIZE - 1);
            native_lut[index * 4 + 2] = 57000 * index / (LUT_SIZE - 1);
        }
    }
    memcpy(current_ramp, original_ramp, sizeof(current_ramp));
    controller = xs_gamma_controller_new(&error);
    g_assert_no_error(error);
    g_assert_nonnull(controller);
    return controller;
}

static XsGammaController *
new_controller(void)
{
    return new_test_controller(FALSE, FALSE, TRUE, FALSE);
}

static void
apply_colors(XsGammaController *controller, gboolean daylight)
{
    GError *error = NULL;
    g_assert_true(xs_gamma_controller_apply(controller,
        daylight ? 6500 : 3400, daylight ? 1.0 : 0.9, 1.0,
        FALSE, NULL, &error));
    g_assert_no_error(error);
}

static void
test_resume_preserves_baseline(void)
{
    XsGammaController *controller = new_controller();
    unsigned short day_ramp[3][RAMP_SIZE];
    unsigned short night_ramp[3][RAMP_SIZE];
    GPtrArray *outputs;

    apply_colors(controller, TRUE);
    memcpy(day_ramp, current_ramp, sizeof(day_ramp));
    apply_colors(controller, FALSE);
    memcpy(night_ramp, current_ramp, sizeof(night_ramp));
    g_assert_cmpuint(night_ramp[2][RAMP_SIZE - 1], <,
                     day_ramp[2][RAMP_SIZE - 1]);

    config_timestamp++;
    outputs = xs_gamma_controller_get_outputs(controller);
    g_assert_cmpuint(outputs->len, ==, 1);
    g_ptr_array_unref(outputs);
    apply_colors(controller, FALSE);
    g_assert_cmpmem(current_ramp, sizeof(current_ramp), night_ramp, sizeof(night_ramp));
    apply_colors(controller, TRUE);
    g_assert_cmpmem(current_ramp, sizeof(current_ramp), day_ramp, sizeof(day_ramp));

    xs_gamma_controller_free(controller);
    g_assert_cmpmem(current_ramp, sizeof(current_ramp), original_ramp, sizeof(original_ramp));
}

static void
test_delayed_reconnect(void)
{
    XsGammaController *controller = new_controller();
    unsigned short day_ramp[3][RAMP_SIZE];
    GError *error = NULL;
    int attempt;

    apply_colors(controller, TRUE);
    memcpy(day_ramp, current_ramp, sizeof(day_ramp));
    apply_colors(controller, FALSE);
    connected = FALSE;
    config_timestamp++;
    for (attempt = 0; attempt < 2; attempt++) {
        g_assert_false(xs_gamma_controller_apply(controller, 6500, 1.0, 1.0,
                                                  FALSE, NULL, &error));
        g_assert_nonnull(error);
        g_clear_error(&error);
    }
    xs_gamma_controller_restore(controller);
    xs_gamma_controller_reset_linear(controller);

    /* Retry even if XRandR still reports the same configuration timestamp. */
    connected = TRUE;
    apply_colors(controller, TRUE);
    g_assert_cmpmem(current_ramp, sizeof(current_ramp), day_ramp, sizeof(day_ramp));
    xs_gamma_controller_free(controller);
    g_assert_cmpmem(current_ramp, sizeof(current_ramp), original_ramp, sizeof(original_ramp));
}

static void
test_restore_after_reconfigure(void)
{
    XsGammaController *controller = new_controller();
    GHashTable *selected = g_hash_table_new(g_str_hash, g_str_equal);
    GError *error = NULL;

    apply_colors(controller, FALSE);
    config_timestamp++;
    /* Deselecting the output must restore the pre-adjustment calibration. */
    g_assert_true(xs_gamma_controller_apply(controller, 6500, 1.0, 1.0,
                                             TRUE, selected, &error));
    g_assert_no_error(error);
    g_assert_cmpmem(current_ramp, sizeof(current_ramp), original_ramp, sizeof(original_ramp));
    apply_colors(controller, FALSE);
    config_timestamp++;
    xs_gamma_controller_restore(controller);
    g_assert_cmpmem(current_ramp, sizeof(current_ramp), original_ramp, sizeof(original_ramp));
    g_hash_table_unref(selected);
    xs_gamma_controller_free(controller);
}

static void
test_native_lut_toggle(void)
{
    XsGammaController *controller = new_test_controller(TRUE, FALSE, TRUE, FALSE);
    unsigned short night_lut[LUT_SIZE * 4];
    GError *error = NULL;

    apply_colors(controller, FALSE);
    g_assert_cmpuint(native_writes, ==, 1);
    g_assert_cmpuint(legacy_writes, ==, 0);
    g_assert_cmpuint(native_lut_length, ==, LUT_SIZE * 4);
    g_assert_cmpuint(native_lut[(LUT_SIZE - 1) * 4 + 2], <,
                     native_lut[(LUT_SIZE - 1) * 4]);
    memcpy(night_lut, native_lut, sizeof(night_lut));
    apply_colors(controller, FALSE);
    g_assert_cmpmem(native_lut, sizeof(native_lut), night_lut, sizeof(night_lut));
    g_assert_cmpmem(current_ramp, sizeof(current_ramp), original_ramp, sizeof(original_ramp));

    xs_gamma_controller_restore(controller);
    g_assert_cmpuint(native_lut_length, ==, 1);
    g_assert_cmpuint(native_lut[0], ==, 0);
    apply_colors(controller, FALSE);
    g_assert_cmpmem(native_lut, sizeof(native_lut), night_lut, sizeof(night_lut));
    apply_colors(controller, TRUE);
    g_assert_cmpuint(native_lut[(LUT_SIZE - 1) * 4 + 2], >,
                     night_lut[(LUT_SIZE - 1) * 4 + 2]);
    g_assert_true(xs_gamma_controller_apply(controller, 3400, 1.0, 0.0,
                                             FALSE, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(native_lut[(LUT_SIZE - 1) * 4], ==, 65535);
    g_assert_cmpuint(native_lut[(LUT_SIZE - 1) * 4 + 1], ==, 65535);
    g_assert_cmpuint(native_lut[(LUT_SIZE - 1) * 4 + 2], ==, 65535);
    xs_gamma_controller_free(controller);
    g_assert_cmpuint(native_lut_length, ==, 1);
    g_assert_cmpuint(native_lut[0], ==, 0);
}

static void
test_native_lut_preserves_calibration(void)
{
    XsGammaController *controller = new_test_controller(TRUE, TRUE, TRUE, FALSE);
    unsigned short calibration[LUT_SIZE * 4];
    unsigned short night_lut[LUT_SIZE * 4];
    GHashTable *selected = g_hash_table_new(g_str_hash, g_str_equal);
    GError *error = NULL;

    memcpy(calibration, native_lut, sizeof(calibration));
    apply_colors(controller, FALSE);
    memcpy(night_lut, native_lut, sizeof(night_lut));
    config_timestamp++;
    apply_colors(controller, FALSE);
    g_assert_cmpmem(native_lut, sizeof(native_lut), night_lut, sizeof(night_lut));

    /* Deselecting, disabling, and quitting must all restore the native LUT. */
    g_assert_true(xs_gamma_controller_apply(controller, 3400, 0.9, 1.0,
                                             TRUE, selected, &error));
    g_assert_no_error(error);
    g_assert_cmpmem(native_lut, sizeof(native_lut), calibration, sizeof(calibration));
    apply_colors(controller, FALSE);
    connected = FALSE;
    config_timestamp++;
    xs_gamma_controller_restore(controller);
    connected = TRUE;
    apply_colors(controller, FALSE);
    g_assert_cmpmem(native_lut, sizeof(native_lut), night_lut, sizeof(night_lut));
    xs_gamma_controller_restore(controller);
    g_assert_cmpmem(native_lut, sizeof(native_lut), calibration, sizeof(calibration));
    apply_colors(controller, FALSE);
    xs_gamma_controller_free(controller);
    g_assert_cmpmem(native_lut, sizeof(native_lut), calibration, sizeof(calibration));
    g_assert_cmpuint(legacy_writes, ==, 0);
    g_hash_table_unref(selected);
}

static void
test_native_lut_without_legacy_gamma(void)
{
    XsGammaController *controller = new_test_controller(TRUE, FALSE, FALSE, FALSE);

    apply_colors(controller, FALSE);
    g_assert_cmpuint(native_writes, ==, 1);
    xs_gamma_controller_reset_linear(controller);
    g_assert_cmpuint(native_lut_length, ==, 1);
    g_assert_cmpuint(native_lut[0], ==, 0);
    g_assert_cmpuint(legacy_writes, ==, 0);
    xs_gamma_controller_free(controller);
}

static void
test_reset_clears_both_luts(void)
{
    XsGammaController *controller = new_test_controller(TRUE, TRUE, TRUE, FALSE);

    apply_colors(controller, FALSE);
    xs_gamma_controller_reset_linear(controller);
    g_assert_cmpuint(native_lut_length, ==, 1);
    g_assert_cmpuint(native_lut[0], ==, 0);
    g_assert_cmpuint(current_ramp[0][RAMP_SIZE - 1], ==, 65535);
    g_assert_cmpuint(current_ramp[1][RAMP_SIZE - 1], ==, 65535);
    g_assert_cmpuint(current_ramp[2][RAMP_SIZE - 1], ==, 65535);
    xs_gamma_controller_free(controller);
    g_assert_cmpuint(native_lut_length, ==, 1);
    g_assert_cmpuint(native_lut[0], ==, 0);
    g_assert_cmpuint(legacy_writes, ==, 1);
}

static void
test_malformed_native_lut_falls_back(void)
{
    XsGammaController *controller = new_test_controller(TRUE, FALSE, TRUE, TRUE);

    apply_colors(controller, FALSE);
    g_assert_cmpuint(native_writes, ==, 0);
    g_assert_cmpuint(legacy_writes, ==, 1);
    xs_gamma_controller_free(controller);
    g_assert_cmpmem(current_ramp, sizeof(current_ramp), original_ramp, sizeof(original_ramp));
}

static void
test_restart_after_night_restores_daylight(void)
{
    XsGammaController *previous = new_test_controller(TRUE, FALSE, TRUE, FALSE);
    XsGammaController *restarted;
    GError *error = NULL;

    apply_colors(previous, FALSE);
    /* Leave the server's night LUT intact, as after an abrupt process exit.
     * The new controller has no access to the previous controller's memory. */
    restarted = xs_gamma_controller_new(&error);
    g_assert_no_error(error);
    g_assert_nonnull(restarted);
    apply_colors(restarted, TRUE);
    g_assert_cmpuint(native_lut[(LUT_SIZE - 1) * 4], ==, 65535);
    g_assert_cmpuint(native_lut[(LUT_SIZE - 1) * 4 + 2], >, 64000);
    xs_gamma_controller_restore(restarted);
    g_assert_cmpuint(native_lut_length, ==, 1);
    g_assert_cmpuint(native_lut[0], ==, 0);
    g_assert_cmpuint(saved_luts[0].length, ==, 0);
    g_assert_cmpuint(saved_luts[1].length, ==, 0);
    g_assert_cmpuint(saved_luts[2].length, ==, 0);
    xs_gamma_controller_free(restarted);
    xs_gamma_controller_free(previous);
}

static void
test_restart_while_disabled_restores_colors(void)
{
    XsGammaController *previous = new_test_controller(TRUE, TRUE, TRUE, FALSE);
    XsGammaController *restarted;
    unsigned short calibration[LUT_SIZE * 4];
    GError *error = NULL;

    memcpy(calibration, native_lut, sizeof(calibration));
    apply_colors(previous, FALSE);
    restarted = xs_gamma_controller_new(&error);
    g_assert_no_error(error);
    g_assert_nonnull(restarted);
    /* This is app_update's disabled branch, without first applying colors. */
    xs_gamma_controller_restore(restarted);
    g_assert_cmpmem(native_lut, sizeof(native_lut), calibration, sizeof(calibration));
    xs_gamma_controller_free(restarted);
    xs_gamma_controller_free(previous);
}

static void
test_restart_preserves_external_calibration(void)
{
    XsGammaController *previous = new_test_controller(TRUE, FALSE, TRUE, FALSE);
    XsGammaController *restarted;
    unsigned short external_lut[LUT_SIZE * 4];
    GError *error = NULL;

    apply_colors(previous, FALSE);
    /* A color manager replaces our LUT after the old process stops. */
    native_lut[(LUT_SIZE - 1) * 4] = 61000;
    native_lut[(LUT_SIZE - 1) * 4 + 1] = 59000;
    native_lut[(LUT_SIZE - 1) * 4 + 2] = 57000;
    memcpy(external_lut, native_lut, sizeof(external_lut));
    restarted = xs_gamma_controller_new(&error);
    g_assert_no_error(error);
    g_assert_true(xs_gamma_controller_apply(restarted, 6500, 1.0, 0.0,
                                             FALSE, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpmem(native_lut, sizeof(native_lut), external_lut, sizeof(external_lut));
    xs_gamma_controller_free(restarted);
    g_assert_cmpmem(native_lut, sizeof(native_lut), external_lut, sizeof(external_lut));
    xs_gamma_controller_free(previous);
}

static void
test_restart_during_pending_update(void)
{
    XsGammaController *previous = new_test_controller(TRUE, FALSE, TRUE, FALSE);
    XsGammaController *restarted;
    unsigned short night_lut[LUT_SIZE * 4];
    GError *error = NULL;

    apply_colors(previous, FALSE);
    memcpy(night_lut, native_lut, sizeof(night_lut));
    apply_colors(previous, TRUE);
    /* Simulate death after the recovery marker was written but before the
     * pending daylight output property reached the server. */
    memcpy(native_lut, night_lut, sizeof(native_lut));
    restarted = xs_gamma_controller_new(&error);
    g_assert_no_error(error);
    xs_gamma_controller_restore(restarted);
    g_assert_cmpuint(native_lut_length, ==, 1);
    g_assert_cmpuint(native_lut[0], ==, 0);
    xs_gamma_controller_free(restarted);
    xs_gamma_controller_free(previous);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/gamma/resume-preserves-baseline", test_resume_preserves_baseline);
    g_test_add_func("/gamma/delayed-reconnect", test_delayed_reconnect);
    g_test_add_func("/gamma/restore-after-reconfigure", test_restore_after_reconfigure);
    g_test_add_func("/gamma/native-lut-toggle", test_native_lut_toggle);
    g_test_add_func("/gamma/native-lut-preserves-calibration", test_native_lut_preserves_calibration);
    g_test_add_func("/gamma/native-lut-without-legacy-gamma", test_native_lut_without_legacy_gamma);
    g_test_add_func("/gamma/reset-clears-both-luts", test_reset_clears_both_luts);
    g_test_add_func("/gamma/malformed-native-lut-falls-back", test_malformed_native_lut_falls_back);
    g_test_add_func("/gamma/restart-after-night-restores-daylight", test_restart_after_night_restores_daylight);
    g_test_add_func("/gamma/restart-while-disabled-restores-colors", test_restart_while_disabled_restores_colors);
    g_test_add_func("/gamma/restart-preserves-external-calibration", test_restart_preserves_external_calibration);
    g_test_add_func("/gamma/restart-during-pending-update", test_restart_during_pending_update);
    return g_test_run();
}
