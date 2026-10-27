/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 1993-2026 Shannon Smith
 *
 * Deterministic historical calendar models not supplied by ICU.
 *
 * Revised Julian follows Milanković's 1923 leap rule: ordinary fourth years
 * leap, century years leap only when their remainder modulo 900 is 200 or 600.
 * The arithmetic continuation is anchored to 2000-01-01, when Revised Julian
 * and Gregorian dates coincide.
 *
 * Byzantine Anno Mundi uses the Julian calendar, a 1 September year boundary
 * and the Constantinopolitan creation era whose first year begins 1 September
 * 5509 BC. This resource uses astronomical year numbering internally but
 * formats the historical Anno Mundi year without changing Julian month/day.
 *
 * Egyptian civil dates use Ptolemy's Nabonassar era as the absolute epoch:
 * 1 Thoth, year 1 = JDN 1448638 (26 February 747 BC in the proleptic Julian
 * calendar). This makes the otherwise wandering 365-day Egyptian civil year
 * deterministic. It must not be confused with pharaonic regnal-year dating.
 */

#include "calendar-historical.h"

#include "calendar-helpers.h"
#include "julian-day.h"

#include <infiltratr/arithmetic.h>
#include <glib/gi18n-lib.h>
#include <math.h>

#define floor_divide calendar_floor_divide
#define positive_modulo calendar_positive_modulo
#define gregorian_to_jdn calendar_gregorian_to_jdn
#define jdn_to_julian calendar_jdn_to_julian
#define julian_to_jdn calendar_julian_to_jdn

#define REVISED_JULIAN_ANCHOR_YEAR ((gint64)2000)
#define EGYPTIAN_NABONASSAR_EPOCH_JDN ((gint64)1448638)

static const gchar *const common_months[] = {
    "", N_("January"), N_("February"), N_("March"), N_("April"),
    N_("May"), N_("June"), N_("July"), N_("August"), N_("September"),
    N_("October"), N_("November"), N_("December")
};


static gint64
count_congruence_inclusive(gint64 first,
                           gint64 last,
                           gint64 modulus,
                           gint64 residue)
{
    if (first > last)
        return 0;

    return calendar_i64_subtract_saturating(
        floor_divide(
            calendar_i64_subtract_saturating(last, residue),
            modulus),
        floor_divide(
            calendar_i64_subtract_saturating(
                calendar_i64_subtract_saturating(first, 1),
                residue),
            modulus));
}

static gboolean
revised_julian_is_leap(gint64 year)
{
    const gint64 century_remainder = positive_modulo(year, 900);

    if (positive_modulo(year, 4) != 0)
        return FALSE;
    if (positive_modulo(year, 100) != 0)
        return TRUE;

    return century_remainder == 200 || century_remainder == 600;
}

static gint64
revised_julian_leaps_inclusive(gint64 first,
                               gint64 last)
{
    gint64 result = calendar_count_multiples_inclusive(
        first, last, 4);

    result = calendar_i64_subtract_saturating(
        result,
        calendar_count_multiples_inclusive(first, last, 100));
    result = calendar_i64_add_saturating(
        result,
        count_congruence_inclusive(first, last, 900, 200));
    return calendar_i64_add_saturating(
        result,
        count_congruence_inclusive(first, last, 900, 600));
}

static gint64
revised_julian_year_start(gint64 year)
{
    const gint64 anchor = gregorian_to_jdn(2000, 1, 1);

    if (year >= REVISED_JULIAN_ANCHOR_YEAR)
    {
        const gint64 years = calendar_i64_subtract_saturating(
            year, REVISED_JULIAN_ANCHOR_YEAR);
        gint64 result = calendar_i64_add_saturating(
            anchor,
            calendar_i64_multiply_saturating(years, 365));

        return calendar_i64_add_saturating(
            result,
            revised_julian_leaps_inclusive(
                REVISED_JULIAN_ANCHOR_YEAR,
                calendar_i64_subtract_saturating(year, 1)));
    }

    {
        const gint64 years = calendar_i64_subtract_saturating(
            REVISED_JULIAN_ANCHOR_YEAR, year);
        gint64 result = calendar_i64_subtract_saturating(
            anchor,
            calendar_i64_multiply_saturating(years, 365));

        return calendar_i64_subtract_saturating(
            result,
            revised_julian_leaps_inclusive(
                year, REVISED_JULIAN_ANCHOR_YEAR - 1));
    }
}

static gint
gregorian_shape_month_length(gint64 year,
                             gint month,
                             gboolean leap)
{
    (void)year;

    if (month == 2)
        return leap ? 29 : 28;
    return month == 4 || month == 6 || month == 9 || month == 11 ? 30 : 31;
}

gint
calendar_revised_julian_month_length(
    const CalendarCalendarFields *fields)
{
    g_return_val_if_fail(fields != NULL, 0);
    g_return_val_if_fail(fields->month >= 1 && fields->month <= 12, 0);

    return gregorian_shape_month_length(
        fields->year,
        fields->month,
        revised_julian_is_leap(fields->year));
}

gint64
calendar_revised_julian_fields_to_jdn(
    const CalendarCalendarFields *fields)
{
    gint64 jdn;
    gint month;

    g_return_val_if_fail(fields != NULL, gregorian_to_jdn(2000, 1, 1));

    jdn = revised_julian_year_start(fields->year);
    for (month = 1; month < fields->month; month++)
    {
        CalendarCalendarFields cursor = *fields;
        cursor.month = month;
        jdn = calendar_i64_add_saturating(
            jdn, calendar_revised_julian_month_length(&cursor));
    }

    return calendar_i64_add_saturating(
        jdn, fields->day - 1);
}

void
calendar_revised_julian_fields_from_jdn(
    gint64 jdn,
    CalendarCalendarFields *fields)
{
    const gint64 anchor = gregorian_to_jdn(2000, 1, 1);
    gint64 year;
    gint64 start;
    gint remaining;
    gint month = 1;

    g_return_if_fail(fields != NULL);
    if (!calendar_corrected_year_jdn_supported(jdn))
    {
        *fields = (CalendarCalendarFields){ 0 };
        return;
    }

    year = (gint64)floorl(
        ((long double)jdn - (long double)anchor) / 365.2422222222222L) +
        REVISED_JULIAN_ANCHOR_YEAR;

    /* Initial year estimates normally differ by at most one. At extreme
     * JDNs saturating conversion cannot converge; cap correction work and
     * signal unsupported input instead of trapping an app in a loop. */
    for (gint i = 0; i < 8 && jdn < revised_julian_year_start(year); ++i)
        year = calendar_i64_subtract_saturating(year, 1);
    for (gint i = 0; i < 8 &&
         jdn >= revised_julian_year_start(calendar_i64_add_saturating(year, 1)); ++i)
        year = calendar_i64_add_saturating(year, 1);
    if (jdn < revised_julian_year_start(year) ||
        jdn >= revised_julian_year_start(calendar_i64_add_saturating(year, 1)))
    {
        *fields = (CalendarCalendarFields){ 0 };
        return;
    }

    start = revised_julian_year_start(year);
    const gint64 offset = calendar_i64_subtract_saturating(jdn, start);
    if (offset < 0 || offset > 365)
    {
        *fields = (CalendarCalendarFields){ 0 };
        return;
    }
    remaining = (gint)offset;
    while (month < 12)
    {
        CalendarCalendarFields cursor = {
            .year = year,
            .month = month
        };
        const gint length =
            calendar_revised_julian_month_length(&cursor);

        if (remaining < length)
            break;
        remaining -= length;
        month++;
    }

    *fields = (CalendarCalendarFields){
        .year = year,
        .month = month,
        .day = remaining + 1,
        .auxiliary = 0,
        .special = FALSE
    };
}

gchar *
calendar_revised_julian_format(
    const CalendarCalendarFields *fields,
    CalendarDatePart part)
{
    g_return_val_if_fail(fields != NULL, g_strdup(""));
    return calendar_format_named_date(fields, part, common_months, G_N_ELEMENTS(common_months), NULL);
}


/*
 * Wandering 365-day years use one checked inverse. Saturation would make
 * INT64_MAX appear to round-trip even when the intermediate year*365 or
 * epoch addition overflowed. No provider may mistake that for a date.
 */


/*
 * Traditional Armenian wandering year.
 *
 * The numbered Armenian era begins 11 July 552 in the Julian calendar.
 * Each year has twelve 30-day months followed by five Aweleacʿ epagomenal
 * days and no leap day; this is deliberately separate from Sarkawag's reform.
 */
#define ARMENIAN_EPOCH_JDN (calendar_julian_to_jdn(552, 7, 11))


