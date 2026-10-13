#ifndef X11_SHIFT_RESUME_MONITOR_H
#define X11_SHIFT_RESUME_MONITOR_H

#include <gio/gio.h>

typedef struct _XsResumeMonitor XsResumeMonitor;
typedef void (*XsResumeCallback)(gpointer user_data);

/* Optional: callers should retain their periodic updates if the bus is absent. */
XsResumeMonitor *xs_resume_monitor_new(XsResumeCallback callback,
                                        gpointer user_data,
                                        GError **error);
void xs_resume_monitor_free(XsResumeMonitor *monitor);

#endif
