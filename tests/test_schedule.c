#define _POSIX_C_SOURCE 200809L

#include "schedule.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void
assert_close(double actual, double expected)
{
    assert(fabs(actual - expected) < 0.0001);
}

static time_t
local_timestamp(int month, int day, int hour, int minute)
{
    struct tm local_time = {0};
    local_time.tm_year = 2026 - 1900;
    local_time.tm_mon = month - 1;
    local_time.tm_mday = day;
    local_time.tm_hour = hour;
    local_time.tm_min = minute;
    local_time.tm_isdst = -1;
    return mktime(&local_time);
}

static void
test_overnight_resume(void)
{
    XsScheduleResult result;
    XsScheduleMode modes[] = {XS_SCHEDULE_CUSTOM, XS_SCHEDULE_SOLAR};
    size_t index;

    setenv("TZ", "America/Chicago", 1);
    tzset();
    for (index = 0; index < sizeof(modes) / sizeof(modes[0]); index++) {
        result = xs_schedule_calculate(modes[index],
            local_timestamp(9, 6, 23, 0), 420, 1140, 41.8781, -87.6298, 180);
        assert_close(result.daylight_factor, 0.0);
        /* No intermediate ticks: resume with the next morning's wall time. */
        result = xs_schedule_calculate(modes[index],
            local_timestamp(9, 7, 10, 0), 420, 1140, 41.8781, -87.6298, 180);
        assert_close(result.daylight_factor, 1.0);
        /* The same jump must work across a daylight-saving change. */
        result = xs_schedule_calculate(modes[index],
            local_timestamp(3, 7, 23, 0), 420, 1140, 41.8781, -87.6298, 45);
        assert_close(result.daylight_factor, 0.0);
        result = xs_schedule_calculate(modes[index],
            local_timestamp(3, 8, 10, 0), 420, 1140, 41.8781, -87.6298, 45);
        assert_close(result.daylight_factor, 1.0);
    }
    result = xs_schedule_calculate(XS_SCHEDULE_CUSTOM,
        local_timestamp(9, 7, 8, 30), 420, 1140, 0, 0, 180);
    assert_close(result.daylight_factor, 0.5);
    result = xs_schedule_calculate(XS_SCHEDULE_ALWAYS_NIGHT,
        local_timestamp(9, 7, 10, 0), 420, 1140, 0, 0, 45);
    assert_close(result.daylight_factor, 0.0);
}

static void
test_transition_status(void)
{
    static const struct {
        int hour;
        int minute;
        XsTransition transition;
        double progress;
        double remaining_minutes;
    } cases[] = {
        {6, 59, XS_TRANSITION_NONE, 0.0, 0.0},
        {7, 0, XS_TRANSITION_TO_DAY, 0.0, 180.0},
        {7, 45, XS_TRANSITION_TO_DAY, 0.25, 135.0},
        {8, 30, XS_TRANSITION_TO_DAY, 0.5, 90.0},
        {9, 15, XS_TRANSITION_TO_DAY, 0.75, 45.0},
        {10, 0, XS_TRANSITION_NONE, 0.0, 0.0},
        {19, 0, XS_TRANSITION_TO_NIGHT, 0.0, 180.0},
        {19, 45, XS_TRANSITION_TO_NIGHT, 0.25, 135.0},
        {20, 30, XS_TRANSITION_TO_NIGHT, 0.5, 90.0},
        {21, 15, XS_TRANSITION_TO_NIGHT, 0.75, 45.0},
        {22, 0, XS_TRANSITION_NONE, 0.0, 0.0}
    };
    size_t index;
    XsScheduleResult result;
    time_t midnight;

    setenv("TZ", "UTC", 1);
    tzset();
    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        result = xs_schedule_calculate(XS_SCHEDULE_CUSTOM,
            local_timestamp(9, 7, cases[index].hour, cases[index].minute),
            420, 1140, 0, 0, 180);
        assert(result.transition == cases[index].transition);
        assert_close(result.transition_progress, cases[index].progress);
        assert_close(result.transition_remaining_seconds,
                      cases[index].remaining_minutes * 60.0);
    }

    result = xs_schedule_calculate(XS_SCHEDULE_CUSTOM,
        local_timestamp(9, 7, 7, 0), 420, 1140, 0, 0, 0);
    assert(result.transition == XS_TRANSITION_NONE);
    assert_close(result.daylight_factor, 1.0);
    result = xs_schedule_calculate(XS_SCHEDULE_CUSTOM,
        local_timestamp(9, 8, 0, 30), 420, 23 * 60, 0, 0, 180);
    assert(result.transition == XS_TRANSITION_TO_NIGHT);
    assert_close(result.transition_progress, 0.5);
    assert_close(result.transition_remaining_seconds, 90 * 60);

    /* Solar schedules use their calculated events for direction and progress. */
    midnight = local_timestamp(3, 20, 0, 0);
    result = xs_schedule_calculate(XS_SCHEDULE_SOLAR, midnight,
                                   420, 1140, 0, 0, 60);
    assert(result.solar_times_valid);
    result = xs_schedule_calculate(XS_SCHEDULE_SOLAR,
        midnight + (time_t)ceil(result.sunrise_minute * 60) + 1800,
        420, 1140, 0, 0, 60);
    assert(result.transition == XS_TRANSITION_TO_DAY);
    assert(fabs(result.transition_progress - 0.5) < 0.001);
    assert(fabs(result.transition_remaining_seconds - 1800) < 1.0);
    result = xs_schedule_calculate(XS_SCHEDULE_SOLAR,
        midnight + (time_t)ceil(result.sunset_minute * 60) + 1800,
        420, 1140, 0, 0, 60);
    assert(result.transition == XS_TRANSITION_TO_NIGHT);
    assert(fabs(result.transition_progress - 0.5) < 0.001);

    for (index = XS_SCHEDULE_ALWAYS_NIGHT; index <= XS_SCHEDULE_ALWAYS_DAY; index++) {
        result = xs_schedule_calculate((XsScheduleMode)index,
            local_timestamp(9, 7, 8, 30), 420, 1140, 0, 0, 180);
        assert(result.transition == XS_TRANSITION_NONE);
    }
    result = xs_schedule_calculate(XS_SCHEDULE_SOLAR,
        local_timestamp(6, 21, 8, 30), 420, 1140, 89.8, 0, 180);
    assert(!result.solar_times_valid);
    assert(result.transition == XS_TRANSITION_NONE);
}

int
main(void)
{
    int minutes = -1;
    struct tm equinox = {0};
    time_t equinox_time;
    XsScheduleResult solar;
    char text[6];

    assert(xs_parse_time("07:30", &minutes) && minutes == 450);
    assert(xs_parse_time(" 7 : 05 ", &minutes) && minutes == 425);
    assert(!xs_parse_time("24:00", &minutes));
    assert(!xs_parse_time("12:60", &minutes));
    assert(!xs_parse_time("noon", &minutes));
    xs_format_time(5, text);
    assert(strcmp(text, "00:05") == 0);

    assert_close(xs_schedule_factor_for_events(12 * 3600, 7 * 3600,
                                                19 * 3600, 0), 1.0);
    assert_close(xs_schedule_factor_for_events(23 * 3600, 7 * 3600,
                                                19 * 3600, 0), 0.0);
    assert_close(xs_schedule_factor_for_events(7.5 * 3600, 7 * 3600,
                                                19 * 3600, 3600), 0.5);
    assert_close(xs_schedule_factor_for_events(19.5 * 3600, 7 * 3600,
                                                19 * 3600, 3600), 0.5);
    assert_close(xs_schedule_factor_for_events(1 * 3600, 22 * 3600,
                                                8 * 3600, 0), 1.0);
    assert_close(xs_schedule_factor_for_events(12 * 3600, 22 * 3600,
                                                8 * 3600, 0), 0.0);

    setenv("TZ", "UTC", 1);
    tzset();
    equinox.tm_year = 2026 - 1900;
    equinox.tm_mon = 2;
    equinox.tm_mday = 20;
    equinox.tm_hour = 12;
    equinox.tm_isdst = -1;
    equinox_time = mktime(&equinox);
    solar = xs_schedule_calculate(XS_SCHEDULE_SOLAR, equinox_time,
                                  420, 1140, 0.0, 0.0, 0);
    assert(solar.solar_times_valid);
    assert(solar.sunrise_minute > 350.0 && solar.sunrise_minute < 380.0);
    assert(solar.sunset_minute > 1070.0 && solar.sunset_minute < 1100.0);
    assert_close(solar.daylight_factor, 1.0);

    test_overnight_resume();
    test_transition_status();
    puts("schedule tests passed");
    return 0;
}
