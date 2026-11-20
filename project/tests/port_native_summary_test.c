/* port_native_summary_test.c -- the shared switch rules of project/port_native_summary.h (identical in all three
 * ports; scratch\perf_final\run_tests.ps1 builds this against each repo's copy, as C and as C++). */
#include "port_native_summary.h"

static int s_fail, s_n;

static void check_int(const char* what, const char* in, int got, int want)
{
    s_n++;
    if (got != want) { s_fail++; printf("FAIL %s(\"%s\") = %d, want %d\n", what, in ? in : "(null)", got, want); }
}

static void check_str(const char* what, const char* got, const char* want)
{
    s_n++;
    if (strcmp(got, want)) { s_fail++; printf("FAIL %s:\n  got  %s\n  want %s\n", what, got, want); }
}

int main(void)
{
    static const struct { const char* in; int def, want; } k_bool[] = {
        { NULL, 1, 1 }, { NULL, 0, 0 }, { "", 1, 1 }, { "", 0, 0 },
        { "1", 0, 1 }, { "on", 0, 1 }, { "ON", 0, 1 }, { "True", 0, 1 }, { "yEs", 0, 1 },
        { "0", 1, 0 }, { "off", 1, 0 }, { "OFF", 1, 0 }, { "False", 1, 0 }, { "NO", 1, 0 },
        { " 1 ", 0, 1 }, { "\toff\r\n", 1, 0 }, { "  ", 1, 1 }, { "  ", 0, 0 },
        /* not a boolean word: the default, whatever it looks like (the review's cases) */
        { "2", 0, 0 }, { "2", 1, 1 }, { "0x1", 0, 0 }, { "-1", 0, 0 }, { "-1", 1, 1 }, { "00", 1, 1 },
        { "abc", 0, 0 }, { "abc", 1, 1 }, { "o n", 0, 0 }, { "onn", 0, 0 }, { "yesyesyes", 0, 0 }, { "1.0", 0, 0 },
    };
    static const struct { const char* in; int def, want; } k_level[] = {
        { NULL, 0, 0 }, { "", 3, 3 }, { "0", 1, 0 }, { "1", 0, 1 }, { "2", 0, 2 }, { " 4 ", 0, 4 }, { "9999", 0, 9999 },
        { "on", 0, 1 }, { "OFF", 2, 0 }, { "yes", 0, 1 },
        { "10000", 0, 0 }, { "-1", 0, 0 }, { "0x2", 0, 0 }, { "2a", 0, 0 }, { "abc", 5, 5 },
    };
    for (size_t i = 0; i < sizeof k_bool / sizeof k_bool[0]; i++)
        check_int("parse_bool", k_bool[i].in, port_native_parse_bool(k_bool[i].in, k_bool[i].def), k_bool[i].want);
    for (size_t i = 0; i < sizeof k_level / sizeof k_level[0]; i++)
        check_int("parse_level", k_level[i].in, port_native_parse_level(k_level[i].in, k_level[i].def), k_level[i].want);

    {   /* the line, from the switches and from an effective state */
        static const port_native_feature f[] = {
            { "a", "PNS_TEST_A", 1, 0 }, { "b", "PNS_TEST_B", 0, 0 }, { "c", "PNS_TEST_C", 0, 1 }, { "d", "PNS_TEST_D", 1, 0 },
        };
        char buf[256];
        const int st[] = { 0, 0, 0, 0 };
        _putenv("PNS_TEST_A=");
        _putenv("PNS_TEST_B=2");      /* unrecognised: default off, as the module */
        _putenv("PNS_TEST_C=2");      /* level switch: on */
        _putenv("PNS_TEST_D=False");
        check_str("format", port_native_format(f, 4, buf, sizeof buf),
                  "[native] on: a(PNS_TEST_A) c(PNS_TEST_C) | off: b(PNS_TEST_B) d(PNS_TEST_D)");
        check_str("format_state", port_native_format_state(f, st, 4, buf, sizeof buf),
                  "[native] on: - | off: a(PNS_TEST_A) b(PNS_TEST_B) c(PNS_TEST_C) d(PNS_TEST_D)");
        check_str("truncated", port_native_format_state(f, st, 4, buf, 12), "[native] on");
    }
    printf("port_native_summary_test: %d/%d passed\n", s_n - s_fail, s_n);
    return s_fail ? 1 : 0;
}
