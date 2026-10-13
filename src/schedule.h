#ifndef X11_SHIFT_SCHEDULE_H
#define X11_SHIFT_SCHEDULE_H

#include <stdbool.h>
#include <time.h>

typedef enum {
    XS_SCHEDULE_SOLAR = 0,
    XS_SCHEDULE_CUSTOM,
    XS_SCHEDULE_ALWAYS_NIGHT,
    XS_SCHEDULE_ALWAYS_DAY
} XsScheduleMode;

typedef enum {
    XS_TRANSITION_NONE = 0,
    XS_TRANSITION_TO_DAY,
    XS_TRANSITION_TO_NIGHT
} XsTransition;

typedef struct {
    double daylight_factor;
    double sunrise_minute;
    double sunset_minute;
    bool solar_times_valid;
    bool polar_day;
    XsTransition transition;
    double transition_progress; /* Elapsed fraction, before color easing. */
    double transition_remaining_seconds;
} XsScheduleResult;

bool xs_parse_time(const char *text, int *minutes);
void xs_format_time(int minutes, char output[6]);

double xs_schedule_factor_for_events(double now_seconds,
                                     double day_event_seconds,
                                     double night_event_seconds,
                                     double transition_seconds);

XsScheduleResult xs_schedule_calculate(XsScheduleMode mode,
                                       time_t now,
                                       int custom_day_minute,
                                       int custom_night_minute,
                                       double latitude,
                                       double longitude,
                                       int transition_minutes);

#endif
