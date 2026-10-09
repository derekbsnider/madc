/* parse-cost workload: the common C headers and a little use of each
   (scripts/parse_cost_gate.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>

static int cmp(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

int main(void)
{
    int v[] = { 5, 3, 9, 1 };
    char s[16];
    qsort(v, 4, sizeof v[0], cmp);
    strcpy(s, "abc");
    s[0] = (char)toupper((unsigned char)s[0]);
    printf("%d %d %s %.0f %d\n", v[0], v[3], s, sqrt(16.0), time(NULL) > 0);
    return 0;
}
