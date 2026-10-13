#define _POSIX_C_SOURCE 200809L

#include "schedule.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SECONDS_PER_DAY 86400.0

static double
normalize(double value, double maximum)
{
    value = fmod(value, maximum);
    return value < 0.0 ? value + maximum : value;
}

static double
degrees_to_radians(double degrees)
{
    return degrees * M_PI / 180.0;
}

static double
radians_to_degrees(double radians)
{
    return radians * 180.0 / M_PI;
}

bool
xs_parse_time(const char *text, int *minutes)
{
    int hour;
    int minute;
    char trailing;

    if (text == NULL || minutes == NULL ||
        sscanf(text, " %d : %d %c", &hour, &minute, &trailing) != 2 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return false;
    }

    *minutes = hour * 60 + minute;
    return true;
}

void
xs_format_time(int minutes, char output[6])
{
    minutes %= 24 * 60;
    if (minutes < 0) {
        minutes += 24 * 60;
    }
    snprintf(output, 6, "%02d:%02d", minutes / 60, minutes % 60);
}

static void
calculate_for_events(double now_seconds,
                     double day_event_seconds,
                     double night_event_seconds,
                     double transition_seconds,
                     XsScheduleResult *result)
{
    double since_day;
    double since_night;
    double elapsed;
    double progress;
    bool to_day;

    now_seconds = normalize(now_seconds, SECONDS_PER_DAY);
    day_event_seconds = normalize(day_event_seconds, SECONDS_PER_DAY);
    night_event_seconds = normalize(night_event_seconds, SECONDS_PER_DAY);
    since_day = normalize(now_seconds - day_event_seconds, SECONDS_PER_DAY);
    since_night = normalize(now_seconds - night_event_seconds, SECONDS_PER_DAY);

    to_day = since_day < since_night;
    elapsed = to_day ? since_day : since_night;
    result->daylight_factor = to_day ? 1.0 : 0.0;
    if (transition_seconds <= 0.0 || elapsed >= transition_seconds) {
        return;
    }

    result->transition = to_day ? XS_TRANSITION_TO_DAY : XS_TRANSITION_TO_NIGHT;
    result->transition_progress = elapsed / transition_seconds;
    result->transition_remaining_seconds = transition_seconds - elapsed;
    progress = result->transition_progress;
    progress = progress * progress * (3.0 - 2.0 * progress);
    result->daylight_factor = to_day ? progress : 1.0 - progress;
}

double
xs_schedule_factor_for_events(double now_seconds,
                              double day_event_seconds,
                              double night_event_seconds,
                              double transition_seconds)
{
    XsScheduleResult result = {0};
    calculate_for_events(now_seconds, day_event_seconds, night_event_seconds,
                          transition_seconds, &result);
    return result.daylight_factor;
}

static int
utc_offset_minutes(const struct tm *local_time)
{
    char offset[8] = {0};
    int hour = 0;
    int minute = 0;
    int sign = 1;

    if (strftime(offset, sizeof(offset), "%z", local_time) == 0) {
        return 0;
    }
    if (offset[0] == '-') {
        sign = -1;
    }
    if ((offset[0] == '+' || offset[0] == '-') &&
        isdigit((unsigned char)offset[1]) &&
        isdigit((unsigned char)offset[2]) &&
        isdigit((unsigned char)offset[3]) &&
        isdigit((unsigned char)offset[4])) {
        hour = (offset[1] - '0') * 10 + offset[2] - '0';
        minute = (offset[3] - '0') * 10 + offset[4] - '0';
    }
    return sign * (hour * 60 + minute);
}

/* NOAA's sunrise/sunset approximation, accurate to roughly a minute. */
static bool
solar_event(int day_of_year,
            double latitude,
            double longitude,
            int timezone_offset_minutes,
            bool sunrise,
            double *local_minute,
            bool *polar_day)
{
    const double zenith = 90.833;
    double longitude_hour = longitude / 15.0;
    double approximate_time = day_of_year +
        (((sunrise ? 6.0 : 18.0) - longitude_hour) / 24.0);
    double mean_anomaly = 0.9856 * approximate_time - 3.289;
    double true_longitude = mean_anomaly +
        1.916 * sin(degrees_to_radians(mean_anomaly)) +
        0.020 * sin(2.0 * degrees_to_radians(mean_anomaly)) + 282.634;
    double right_ascension;
    double longitude_quadrant;
    double ascension_quadrant;
    double sin_declination;
    double cos_declination;
    double cosine_hour;
    double hour_angle;
    double local_mean_time;
    double utc_hour;
    double local_hour;

    true_longitude = normalize(true_longitude, 360.0);
    right_ascension = radians_to_degrees(
        atan(0.91764 * tan(degrees_to_radians(true_longitude))));
    right_ascension = normalize(right_ascension, 360.0);
    longitude_quadrant = floor(true_longitude / 90.0) * 90.0;
    ascension_quadrant = floor(right_ascension / 90.0) * 90.0;
    right_ascension = (right_ascension + longitude_quadrant -
                       ascension_quadrant) / 15.0;

    sin_declination = 0.39782 * sin(degrees_to_radians(true_longitude));
    cos_declination = cos(asin(sin_declination));
    cosine_hour = (cos(degrees_to_radians(zenith)) -
                   sin_declination * sin(degrees_to_radians(latitude))) /
                  (cos_declination * cos(degrees_to_radians(latitude)));

    if (cosine_hour > 1.0) {
        *polar_day = false;
        return false;
    }
    if (cosine_hour < -1.0) {
        *polar_day = true;
        return false;
    }

    hour_angle = sunrise
        ? 360.0 - radians_to_degrees(acos(cosine_hour))
        : radians_to_degrees(acos(cosine_hour));
    hour_angle /= 15.0;
    local_mean_time = hour_angle + right_ascension -
                      0.06571 * approximate_time - 6.622;
    utc_hour = normalize(local_mean_time - longitude_hour, 24.0);
    local_hour = normalize(utc_hour + timezone_offset_minutes / 60.0, 24.0);
    *local_minute = local_hour * 60.0;
    return true;
}

XsScheduleResult
xs_schedule_calculate(XsScheduleMode mode,
                      time_t now,
                      int custom_day_minute,
                      int custom_night_minute,
                      double latitude,
                      double longitude,
                      int transition_minutes)
{
    XsScheduleResult result = {0};
    struct tm local_time;
    double now_seconds;
    bool sunrise_ok;
    bool sunset_ok;

    if (mode == XS_SCHEDULE_ALWAYS_DAY) {
        result.daylight_factor = 1.0;
        return result;
    }
    if (mode == XS_SCHEDULE_ALWAYS_NIGHT) {
        return result;
    }

    localtime_r(&now, &local_time);
    now_seconds = local_time.tm_hour * 3600.0 +
                  local_time.tm_min * 60.0 + local_time.tm_sec;

    if (mode == XS_SCHEDULE_CUSTOM) {
        calculate_for_events(
            now_seconds,
            custom_day_minute * 60.0,
            custom_night_minute * 60.0,
            transition_minutes * 60.0, &result);
        return result;
    }

    latitude = fmax(-89.8, fmin(89.8, latitude));
    longitude = fmax(-180.0, fmin(180.0, longitude));
    sunrise_ok = solar_event(local_time.tm_yday + 1, latitude, longitude,
                             utc_offset_minutes(&local_time), true,
                             &result.sunrise_minute, &result.polar_day);
    sunset_ok = solar_event(local_time.tm_yday + 1, latitude, longitude,
                            utc_offset_minutes(&local_time), false,
                            &result.sunset_minute, &result.polar_day);
    result.solar_times_valid = sunrise_ok && sunset_ok;
    if (!result.solar_times_valid) {
        result.daylight_factor = result.polar_day ? 1.0 : 0.0;
        return result;
    }

    calculate_for_events(
        now_seconds,
        result.sunrise_minute * 60.0,
        result.sunset_minute * 60.0,
        transition_minutes * 60.0, &result);
    return result;
}
