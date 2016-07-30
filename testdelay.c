#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <task.h>

enum { STACK = 32768 };

Channel *c;

void
delaytask(void *v)
{
    // sleep v ms后唤醒
	taskdelay((int)v);
	printf("awake after %d ms\n", (int)v);
    // 写入channel, 后面会有recv channel 来计数
	chansendul(c, 0);
}

void
taskmain(int argc, char **argv)
{
	int i, n;
	
    // 创建长度为 0 大小的 channel，用来做同步
	c = chancreate(sizeof(unsigned long), 0);

	n = 0;
	for(i=1; i<argc; i++){
		n++;
		printf("x");
        // 创建一个延时任务
		taskcreate(delaytask, (void*)atoi(argv[i]), STACK);
	}

	/* wait for n tasks to finish */
    // 等待三个延时任务都结束，这里才会结束
    // 因为无 buffer 的 channel, 如果没有数据可读会阻塞
	for(i=0; i<n; i++){
		printf("y");
		chanrecvul(c);
	}
	taskexitall(0);
}
