/*
 * test_format_epoch.c - host test for ls_time.c's format_epoch().
 *
 * format_epoch converts a Unix-style epoch (seconds since 2000-01-01 00:00:00)
 * into a "DD.MM.YYYY HH:MM:SS" text string.  The tricky cases are leap years:
 * February's day count differs, and the month loop must not swallow Feb 29 into
 * March.
 *
 * The function lives in ls_time.c, which this test links against via the
 * linker-closure test runner (apps/ is now in SOURCE_DIRS).
 */
#include "ls_time.h"
#include <stdint.h>
#include <string.h>

/*
 * A check table.  Epochs are seconds since 2000-01-01.  Each entry pairs an
 * epoch with the exact string format_epoch should produce.
 *
 * The leap-year cases are the focus:
 *   - Feb 29 in 2000 (div by 400) and 2004 (div by 4, not 100) must render
 *     "29.02".
 *   - Feb 28 in 2100 (div by 100, not 400) must be the last day of February;
 *     the day after must be March 1, not Feb 29.
 */
static const struct {
    uint32_t epoch;
    const char *expected;
} test_cases[] = {
    {           0, "--.--.---- --:--:--" },                /* sentinel: no timestamp    */
    {        5097600, "29.02.2000 00:00:00" },             /* leap day, leap year 2000  */
    {     36633600, "28.02.2001 00:00:00" },              /* non-leap Feb 28           */
    {     36720000, "01.03.2001 00:00:00" },              /* day after non-leap Feb 28 */
    {    131373045, "29.02.2004 12:30:45" },             /* leap day + time           */
    {    3160857600, "01.03.2100 00:00:00" },            /* 2100: Feb has only 28     */
    {    31622399, "31.12.2000 23:59:59" },               /* end of leap year          */
    {     31622400, "01.01.2001 00:00:00" },              /* first day after leap year */
    { 3160771200, "28.02.2100 00:00:00" },             /* century, not leap (100)   */
    {     5142896, "29.02.2000 12:34:56" },             /* leap day with odd time    */
    {     94694400, "01.01.2003 00:00:00" },              /* normal Jan 1              */
};

int main(void)
{
    char buf[20];
    int i;

    for (i = 0; i < (int)(sizeof(test_cases) / sizeof(test_cases[0])); i++) {
        format_epoch(test_cases[i].epoch, buf);
        if (strcmp(buf, test_cases[i].expected) != 0) {
            return i + 1;
        }
    }
    return 0;
}
