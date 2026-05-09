#include <stdio.h>

int main(argc, argv)
int argc;
char **argv;
{
    int i;

    printf("MCP-TOOL-COMPLETED");
    for (i = 1; i < argc; i++)
    {
        printf(" %s", argv[i]);
    }
    printf("\n");
    return 0;
}
