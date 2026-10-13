#ifndef X11_SHIFT_X11_GAMMA_H
#define X11_SHIFT_X11_GAMMA_H

#include <glib.h>

typedef struct _XsGammaController XsGammaController;

XsGammaController *xs_gamma_controller_new(GError **error);
void xs_gamma_controller_free(XsGammaController *controller);

GPtrArray *xs_gamma_controller_get_outputs(XsGammaController *controller);

gboolean xs_gamma_controller_apply(XsGammaController *controller,
                                   double temperature,
                                   double brightness,
                                   double strength,
                                   gboolean limit_outputs,
                                   GHashTable *selected_outputs,
                                   GError **error);

void xs_gamma_controller_restore(XsGammaController *controller);
void xs_gamma_controller_reset_linear(XsGammaController *controller);

#endif

