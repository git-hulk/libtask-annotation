#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <task.h>

void
counttask1(void *arg)
{
    int i;
    for( i = 0; i < 5; i++) {
        printf("task1: %d\n", i);
        taskyield();
    }
}

void
counttask2(void *arg)
{
    int i;
    for( i = 5; i < 10; i++) {
        printf("task2: %d\n", i);
        taskyield();
    }
}

void
taskmain(int argc, char **argv)
{
    taskcreate(counttask1, NULL, 32768);
    taskcreate(counttask2, NULL, 32768);
}
