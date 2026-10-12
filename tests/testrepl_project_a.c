// testrepl_project's entry TU (testrepl_project.prj.json).
#include <stdio.h>

int helper(void);

int main(int argc, char **argv)
{
    printf("argc=%d last=%s helper=%d\n", argc, argv[argc - 1], helper());
    return 0;
}
