#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <task.h>
#include <stdlib.h>

enum
{
	STACK = 32768
};

char *server;
char *url;

void fetchtask(void*);

void
taskmain(int argc, char **argv)
{
	int i, n;
	
	if(argc != 4){
		fprintf(stderr, "usage: httpload n server url\n");
		taskexitall(1);
	}
    // 协程数目
	n = atoi(argv[1]);
    // 压测域名对应的 ip
	server = argv[2];
    // 压测的 url
	url = argv[3];

    // 这个例子应该是不完整的, 所以没生成二进制文件
    // 因为 fetchtask 一旦开起来就再也不出让 cpu 
    // 也就是其他的协程根本调度不了, 只会创建一个协程
	for(i=0; i<n; i++){
        // 创建压测协程
		taskcreate(fetchtask, 0, STACK);
        // 出让 cpu
		while(taskyield() > 1)
			;
		sleep(1);
	}
}

void
fetchtask(void *v)
{
	int fd, n;
	char buf[512];
	
	fprintf(stderr, "starting...\n");
    // 死循环不断连接服务和发送请求
	for(;;){
        // 连接 http 服务
		if((fd = netdial(TCP, server, 80)) < 0){
			fprintf(stderr, "dial %s: %s (%s)\n", server, strerror(errno), taskgetstate());
			continue;
		}
        // 发送数据并读取响应
		snprintf(buf, sizeof buf, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", url, server);
		fdwrite(fd, buf, strlen(buf));
		while((n = fdread(fd, buf, sizeof buf)) > 0)
			;
        // 关闭连接
		close(fd);
        // 打印一个 . 到屏幕
		write(1, ".", 1);
	}
}
