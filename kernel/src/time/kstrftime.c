/*
 * kernel/src/time/kstrftime.c
 * © suhas pai
 */

#include "dev/printk.h"
#include "lib/parse_strftime.h"

#include "kstrftime.h"

static uint64_t
time_format_to_string_sv_callback(
    const struct strftime_spec_info *const spec_info,
    void *const cb_info,
    const struct string_view sv,
    bool *const should_cont_out)
{
    (void)spec_info;
    if (string_append_sv((struct string *)cb_info, sv) == NULL) {
        *should_cont_out = false;
        return 0;
    }

    return sv.length;
}

struct string kstrftime(const char *const format, const struct tm *const tm) {
    struct string string = STRING_NULL();
    parse_strftime_format(time_format_to_string_sv_callback,
                          &string,
                          format,
                          tm);

    return string;
}

static uint64_t
time_print_format_callback(const struct strftime_spec_info *const spec_info,
                           void *const cb_info,
                           const struct string_view sv,
                           bool *const should_cont_out)
{
    (void)spec_info;
    (void)cb_info;
    (void)should_cont_out;

    putk_sv((enum log_level)(uint64_t)cb_info, sv);
    return sv.length;
}

void
printk_strftime(const enum log_level level,
                const char *const format,
                const struct tm *const tm)
{
    parse_strftime_format(time_print_format_callback,
                          (void *)level,
                          format,
                          tm);
}