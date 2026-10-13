#include "x11_gamma.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    RRCrtc crtc;
    int ramp_size;
    XRRCrtcGamma *original;
    RROutput lut_output;
    int lut_size;
    unsigned short *original_lut;
    unsigned long original_lut_length;
    unsigned short *last_lut;
    unsigned long last_lut_length;
    Atom baseline_property;
    Atom applied_property;
    Atom previous_property;
    GPtrArray *output_names;
    gboolean adjusted;
} XsCrtc;

struct _XsGammaController {
    Display *display;
    Window root;
    int randr_event_base;
    int randr_error_base;
    Time config_timestamp;
    gboolean outputs_valid;
    Atom gamma_lut;
    Atom gamma_lut_size;
    GPtrArray *crtcs;
};

static GQuark
xs_gamma_error_quark(void)
{
    return g_quark_from_static_string("x11shift-xrandr-error");
}

static void
crtc_free(gpointer data)
{
    XsCrtc *crtc = data;
    if (crtc == NULL) {
        return;
    }
    if (crtc->original != NULL) {
        XRRFreeGamma(crtc->original);
    }
    g_free(crtc->original_lut);
    g_free(crtc->last_lut);
    if (crtc->output_names != NULL) {
        g_ptr_array_unref(crtc->output_names);
    }
    g_free(crtc);
}

static XsCrtc *
find_crtc(XsGammaController *controller, RRCrtc id)
{
    guint index;
    for (index = 0; index < controller->crtcs->len; index++) {
        XsCrtc *crtc = g_ptr_array_index(controller->crtcs, index);
        if (crtc->crtc == id) {
            return crtc;
        }
    }
    return NULL;
}

static gboolean
valid_native_lut(const unsigned short *data, unsigned long length, int size)
{
    return data != NULL &&
        (length == (unsigned long)size * 4 || (length == 1 && data[0] == 0));
}

static unsigned short *
read_saved_lut(XsGammaController *controller, XsCrtc *crtc, Atom property,
               unsigned long *length)
{
    Atom type;
    int format;
    unsigned long remaining;
    unsigned char *data = NULL;
    unsigned short *copy = NULL;

    if (XGetWindowProperty(controller->display, controller->root, property,
            0, crtc->lut_size * 2, False, XA_INTEGER, &type, &format,
            length, &remaining, &data) == Success &&
        type == XA_INTEGER && format == 16 && remaining == 0 &&
        valid_native_lut((unsigned short *)data, *length, crtc->lut_size)) {
        copy = g_memdup2(data, *length * sizeof(unsigned short));
    }
    XFree(data);
    return copy;
}

static Atom
recovery_property(XsGammaController *controller, XsCrtc *crtc, const char *kind)
{
    gchar *name = g_strdup_printf("_X11SHIFT_NATIVE_%s_%lx", kind,
                                  (unsigned long)crtc->crtc);
    Atom property = XInternAtom(controller->display, name, False);
    g_free(name);
    return property;
}

static void
recover_native_baseline(XsGammaController *controller, XsCrtc *crtc)
{
    Atom candidates[2];
    guint index;

    crtc->baseline_property = recovery_property(controller, crtc, "BASELINE");
    crtc->applied_property = recovery_property(controller, crtc, "APPLIED");
    crtc->previous_property = recovery_property(controller, crtc, "PREVIOUS");
    crtc->last_lut = g_memdup2(crtc->original_lut,
        crtc->original_lut_length * sizeof(unsigned short));
    crtc->last_lut_length = crtc->original_lut_length;
    candidates[0] = crtc->applied_property;
    candidates[1] = crtc->previous_property;

    /* Root properties survive a client crash, but not the X11 session. Only
     * reuse the saved calibration if this is one of our own recent LUTs;
     * an independently changed color profile remains the new baseline. */
    for (index = 0; index < G_N_ELEMENTS(candidates); index++) {
        unsigned long length = 0;
        unsigned short *applied = read_saved_lut(controller, crtc,
                                                 candidates[index], &length);
        gboolean matches = applied != NULL &&
            length == crtc->original_lut_length &&
            memcmp(applied, crtc->original_lut,
                   length * sizeof(unsigned short)) == 0;
        g_free(applied);
        if (matches) {
            unsigned short *baseline = read_saved_lut(controller, crtc,
                crtc->baseline_property, &length);
            if (baseline != NULL) {
                g_free(crtc->original_lut);
                crtc->original_lut = baseline;
                crtc->original_lut_length = length;
                /* The disabled startup path must restore this inherited tint
                 * even before this controller has applied any new colors. */
                crtc->adjusted = TRUE;
            }
            return;
        }
    }
}

static void
save_lut(XsGammaController *controller, Atom property,
          const unsigned short *data, unsigned long length)
{
    XChangeProperty(controller->display, controller->root, property,
        XA_INTEGER, 16, PropModeReplace, (const unsigned char *)data, (int)length);
}

static void
clear_native_recovery(XsGammaController *controller, XsCrtc *crtc)
{
    XDeleteProperty(controller->display, controller->root, crtc->baseline_property);
    XDeleteProperty(controller->display, controller->root, crtc->applied_property);
    XDeleteProperty(controller->display, controller->root, crtc->previous_property);
}

static gboolean
read_native_lut(XsGammaController *controller, XsCrtc *crtc, RROutput output)
{
    Atom type;
    int format;
    unsigned long length;
    unsigned long remaining;
    unsigned char *data = NULL;
    unsigned long size;

    if (controller->gamma_lut == None || controller->gamma_lut_size == None) {
        return FALSE;
    }
    if (XRRGetOutputProperty(controller->display, output,
            controller->gamma_lut_size, 0, 1, False, False, XA_INTEGER,
            &type, &format, &length, &remaining, &data) != Success) {
        return FALSE;
    }
    if (type != XA_INTEGER || format != 32 || length != 1 || remaining != 0) {
        XFree(data);
        return FALSE;
    }
    /* Xlib returns format-32 property elements as native unsigned longs. */
    size = *(unsigned long *)data;
    XFree(data);
    if (size < 2 || size > 65536) {
        return FALSE;
    }
    data = NULL;
    if (XRRGetOutputProperty(controller->display, output,
            controller->gamma_lut, 0, (long)(size * 2), False, False,
            XA_INTEGER, &type, &format, &length, &remaining, &data) != Success) {
        return FALSE;
    }
    /* Native LUT entries are interleaved red/green/blue/reserved uint16s.
     * A single zero means the driver's default (identity) LUT. */
    if (type != XA_INTEGER || format != 16 || remaining != 0 ||
        !valid_native_lut((unsigned short *)data, length, (int)size)) {
        XFree(data);
        return FALSE;
    }
    crtc->lut_output = output;
    crtc->lut_size = (int)size;
    crtc->original_lut_length = length;
    crtc->original_lut = g_memdup2(data, length * sizeof(unsigned short));
    XFree(data);
    recover_native_baseline(controller, crtc);
    return TRUE;
}

static gboolean
refresh_outputs(XsGammaController *controller, gboolean force, GError **error)
{
    XRRScreenResources *resources;
    int output_index;
    guint crtc_index;
    guint active_count = 0;

    resources = XRRGetScreenResourcesCurrent(controller->display,
                                             controller->root);
    if (resources == NULL) {
        g_set_error_literal(error, xs_gamma_error_quark(), 1,
                            "Could not read the XRandR screen resources");
        return FALSE;
    }
    if (!force && controller->outputs_valid &&
        resources->configTimestamp == controller->config_timestamp) {
        XRRFreeScreenResources(resources);
        return TRUE;
    }

    /* Keep the original ramps even while an output is disconnected. Reading
     * them again after resume can capture our own night tint as the baseline. */
    controller->outputs_valid = FALSE;
    for (crtc_index = 0; crtc_index < controller->crtcs->len; crtc_index++) {
        XsCrtc *crtc = g_ptr_array_index(controller->crtcs, crtc_index);
        g_ptr_array_set_size(crtc->output_names, 0);
    }
    for (output_index = 0; output_index < resources->noutput; output_index++) {
        XRROutputInfo *output;
        XsCrtc *crtc;
        int ramp_size;

        output = XRRGetOutputInfo(controller->display, resources,
                                  resources->outputs[output_index]);
        if (output == NULL) {
            continue;
        }
        if (output->connection != RR_Connected || output->crtc == None) {
            XRRFreeOutputInfo(output);
            continue;
        }

        crtc = find_crtc(controller, output->crtc);
        if (crtc == NULL) {
            ramp_size = XRRGetCrtcGammaSize(controller->display, output->crtc);
            crtc = g_new0(XsCrtc, 1);
            crtc->crtc = output->crtc;
            crtc->ramp_size = ramp_size;
            if (ramp_size > 1) {
                crtc->original = XRRGetCrtcGamma(controller->display, output->crtc);
            }
            crtc->output_names = g_ptr_array_new_with_free_func(g_free);
            if (!read_native_lut(controller, crtc, resources->outputs[output_index]) &&
                crtc->original == NULL) {
                crtc_free(crtc);
                XRRFreeOutputInfo(output);
                continue;
            }
            g_ptr_array_add(controller->crtcs, crtc);
        }
        if (crtc->lut_size > 0) {
            crtc->lut_output = resources->outputs[output_index];
        }
        g_ptr_array_add(crtc->output_names,
                        g_strndup(output->name, (gsize)output->nameLen));
        active_count++;
        XRRFreeOutputInfo(output);
    }
    controller->config_timestamp = resources->configTimestamp;
    XRRFreeScreenResources(resources);

    if (active_count == 0) {
        g_set_error_literal(error, xs_gamma_error_quark(), 2,
                            "No active X11 display with a gamma ramp was found");
        return FALSE;
    }
    controller->outputs_valid = TRUE;
    return TRUE;
}

XsGammaController *
xs_gamma_controller_new(GError **error)
{
    XsGammaController *controller;
    int major = 0;
    int minor = 0;

    controller = g_new0(XsGammaController, 1);
    controller->display = XOpenDisplay(NULL);
    if (controller->display == NULL) {
        g_set_error_literal(error, xs_gamma_error_quark(), 3,
                            "Could not open the X11 display. X11Shift requires an X11 session");
        g_free(controller);
        return NULL;
    }
    if (!XRRQueryExtension(controller->display, &controller->randr_event_base,
                          &controller->randr_error_base) ||
        !XRRQueryVersion(controller->display, &major, &minor) ||
        major < 1 || (major == 1 && minor < 3)) {
        g_set_error_literal(error, xs_gamma_error_quark(), 4,
                            "XRandR 1.3 or newer is required");
        XCloseDisplay(controller->display);
        g_free(controller);
        return NULL;
    }

    controller->root = DefaultRootWindow(controller->display);
    controller->gamma_lut = XInternAtom(controller->display, "GAMMA_LUT", True);
    controller->gamma_lut_size = XInternAtom(controller->display, "GAMMA_LUT_SIZE", True);
    controller->crtcs = g_ptr_array_new_with_free_func(crtc_free);
    if (!refresh_outputs(controller, TRUE, error)) {
        xs_gamma_controller_free(controller);
        return NULL;
    }
    return controller;
}

static void
set_original_gamma(XsGammaController *controller, XsCrtc *crtc)
{
    if (crtc->lut_size > 0) {
        XRRChangeOutputProperty(controller->display, crtc->lut_output,
            controller->gamma_lut, XA_INTEGER, 16, PropModeReplace,
            (unsigned char *)crtc->original_lut, (int)crtc->original_lut_length);
        clear_native_recovery(controller, crtc);
        g_free(crtc->last_lut);
        crtc->last_lut = g_memdup2(crtc->original_lut,
            crtc->original_lut_length * sizeof(unsigned short));
        crtc->last_lut_length = crtc->original_lut_length;
        crtc->adjusted = FALSE;
    } else if (crtc->original != NULL) {
        XRRSetCrtcGamma(controller->display, crtc->crtc, crtc->original);
        crtc->adjusted = FALSE;
    }
}

void
xs_gamma_controller_restore(XsGammaController *controller)
{
    guint index;
    if (controller == NULL || controller->display == NULL) {
        return;
    }
    refresh_outputs(controller, FALSE, NULL);
    for (index = 0; index < controller->crtcs->len; index++) {
        XsCrtc *crtc = g_ptr_array_index(controller->crtcs, index);
        if (crtc->adjusted && crtc->output_names->len > 0) {
            set_original_gamma(controller, crtc);
        }
    }
    XFlush(controller->display);
}

void
xs_gamma_controller_free(XsGammaController *controller)
{
    if (controller == NULL) {
        return;
    }
    xs_gamma_controller_restore(controller);
    if (controller->crtcs != NULL) {
        g_ptr_array_unref(controller->crtcs);
    }
    if (controller->display != NULL) {
        XCloseDisplay(controller->display);
    }
    g_free(controller);
}

GPtrArray *
xs_gamma_controller_get_outputs(XsGammaController *controller)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    guint crtc_index;

    if (controller == NULL) {
        return names;
    }
    refresh_outputs(controller, FALSE, NULL);
    for (crtc_index = 0; crtc_index < controller->crtcs->len; crtc_index++) {
        XsCrtc *crtc = g_ptr_array_index(controller->crtcs, crtc_index);
        guint name_index;
        for (name_index = 0; name_index < crtc->output_names->len; name_index++) {
            g_ptr_array_add(names, g_strdup(
                g_ptr_array_index(crtc->output_names, name_index)));
        }
    }
    return names;
}

static void
temperature_rgb(double temperature, double rgb[3])
{
    double value = fmax(1000.0, fmin(10000.0, temperature)) / 100.0;

    if (value <= 66.0) {
        rgb[0] = 255.0;
        rgb[1] = 99.4708025861 * log(value) - 161.1195681661;
        rgb[2] = value <= 19.0
            ? 0.0
            : 138.5177312231 * log(value - 10.0) - 305.0447927307;
    } else {
        rgb[0] = 329.698727446 * pow(value - 60.0, -0.1332047592);
        rgb[1] = 288.1221695283 * pow(value - 60.0, -0.0755148492);
        rgb[2] = 255.0;
    }
    rgb[0] = fmax(0.0, fmin(255.0, rgb[0])) / 255.0;
    rgb[1] = fmax(0.0, fmin(255.0, rgb[1])) / 255.0;
    rgb[2] = fmax(0.0, fmin(255.0, rgb[2])) / 255.0;
}

static gboolean
crtc_is_selected(XsCrtc *crtc, gboolean limit_outputs,
                 GHashTable *selected_outputs)
{
    guint index;
    if (!limit_outputs) {
        return TRUE;
    }
    for (index = 0; index < crtc->output_names->len; index++) {
        if (g_hash_table_contains(selected_outputs,
                                  g_ptr_array_index(crtc->output_names, index))) {
            return TRUE;
        }
    }
    return FALSE;
}

static void
set_native_gamma(XsGammaController *controller, XsCrtc *crtc,
                 const double rgb[3], double brightness)
{
    unsigned short *lut = g_new0(unsigned short, crtc->lut_size * 4);
    int index;
    int channel;

    /* Some AMD Xorg configurations accept legacy CRTC ramps but only update
     * X11's cached values. GAMMA_LUT commits the actual hardware table. Leave
     * the legacy ramp alone: the driver composes it with this native LUT. */
    for (index = 0; index < crtc->lut_size; index++) {
        for (channel = 0; channel < 3; channel++) {
            double original = crtc->original_lut_length == 1
                ? 65535.0 * index / (crtc->lut_size - 1)
                : crtc->original_lut[index * 4 + channel];
            lut[index * 4 + channel] = (uint16_t)fmin(
                65535.0, original * rgb[channel] * brightness);
        }
    }
    if (!crtc->adjusted) {
        save_lut(controller, crtc->baseline_property, crtc->original_lut,
                  crtc->original_lut_length);
    }
    /* Keep both the current and pending values so recovery also works if the
     * client dies between saving the marker and committing the next LUT. */
    save_lut(controller, crtc->previous_property, crtc->last_lut,
              crtc->last_lut_length);
    save_lut(controller, crtc->applied_property, lut, crtc->lut_size * 4);
    XRRChangeOutputProperty(controller->display, crtc->lut_output,
        controller->gamma_lut, XA_INTEGER, 16, PropModeReplace,
        (unsigned char *)lut, crtc->lut_size * 4);
    g_free(crtc->last_lut);
    crtc->last_lut = lut;
    crtc->last_lut_length = crtc->lut_size * 4;
    crtc->adjusted = TRUE;
}

gboolean
xs_gamma_controller_apply(XsGammaController *controller,
                          double temperature,
                          double brightness,
                          double strength,
                          gboolean limit_outputs,
                          GHashTable *selected_outputs,
                          GError **error)
{
    double rgb[3];
    guint crtc_index;

    if (controller == NULL) {
        g_set_error_literal(error, xs_gamma_error_quark(), 5,
                            "X11 display control is unavailable");
        return FALSE;
    }
    if (!refresh_outputs(controller, FALSE, error)) {
        return FALSE;
    }
    temperature_rgb(temperature, rgb);
    brightness = fmax(0.1, fmin(1.0, brightness));
    strength = fmax(0.0, fmin(1.0, strength));
    rgb[0] = 1.0 + (rgb[0] - 1.0) * strength;
    rgb[1] = 1.0 + (rgb[1] - 1.0) * strength;
    rgb[2] = 1.0 + (rgb[2] - 1.0) * strength;

    for (crtc_index = 0; crtc_index < controller->crtcs->len; crtc_index++) {
        XsCrtc *crtc = g_ptr_array_index(controller->crtcs, crtc_index);
        XRRCrtcGamma *gamma;
        int ramp_index;

        if (crtc->output_names->len == 0) {
            continue;
        }
        if (!crtc_is_selected(crtc, limit_outputs, selected_outputs)) {
            if (crtc->adjusted) {
                set_original_gamma(controller, crtc);
            }
            continue;
        }
        if (crtc->lut_size > 0) {
            set_native_gamma(controller, crtc, rgb, brightness);
            continue;
        }
        gamma = XRRAllocGamma(crtc->ramp_size);
        if (gamma == NULL) {
            g_set_error_literal(error, xs_gamma_error_quark(), 6,
                                "Could not allocate an XRandR gamma ramp");
            return FALSE;
        }
        for (ramp_index = 0; ramp_index < crtc->ramp_size; ramp_index++) {
            double red = crtc->original->red[ramp_index] * rgb[0] * brightness;
            double green = crtc->original->green[ramp_index] * rgb[1] * brightness;
            double blue = crtc->original->blue[ramp_index] * rgb[2] * brightness;
            gamma->red[ramp_index] = (uint16_t)fmin(65535.0, red);
            gamma->green[ramp_index] = (uint16_t)fmin(65535.0, green);
            gamma->blue[ramp_index] = (uint16_t)fmin(65535.0, blue);
        }
        XRRSetCrtcGamma(controller->display, crtc->crtc, gamma);
        XRRFreeGamma(gamma);
        crtc->adjusted = TRUE;
    }
    XFlush(controller->display);
    return TRUE;
}

void
xs_gamma_controller_reset_linear(XsGammaController *controller)
{
    guint crtc_index;
    if (controller == NULL) {
        return;
    }
    refresh_outputs(controller, FALSE, NULL);
    for (crtc_index = 0; crtc_index < controller->crtcs->len; crtc_index++) {
        XsCrtc *crtc = g_ptr_array_index(controller->crtcs, crtc_index);
        XRRCrtcGamma *gamma;
        int ramp_index;
        if (crtc->output_names->len == 0) {
            continue;
        }
        if (crtc->original != NULL) {
            gamma = XRRAllocGamma(crtc->ramp_size);
            if (gamma == NULL) {
                continue;
            }
            for (ramp_index = 0; ramp_index < crtc->ramp_size; ramp_index++) {
                uint16_t value = (uint16_t)lrint(
                    65535.0 * ramp_index / (crtc->ramp_size - 1));
                gamma->red[ramp_index] = value;
                gamma->green[ramp_index] = value;
                gamma->blue[ramp_index] = value;
            }
            XRRSetCrtcGamma(controller->display, crtc->crtc, gamma);
            XRRFreeGamma(gamma);
        }
        if (crtc->lut_size > 0) {
            unsigned short identity = 0;
            XRRChangeOutputProperty(controller->display, crtc->lut_output,
                controller->gamma_lut, XA_INTEGER, 16, PropModeReplace,
                (unsigned char *)&identity, 1);
            clear_native_recovery(controller, crtc);
            g_free(crtc->last_lut);
            crtc->last_lut = g_memdup2(&identity, sizeof(identity));
            crtc->last_lut_length = 1;
        }
        crtc->adjusted = FALSE;
    }
    XFlush(controller->display);
}
