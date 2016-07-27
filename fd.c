#include "taskimpl.h"
#include <sys/poll.h>
#include <fcntl.h>

enum
{
	MAXFD = 1024
};

static struct pollfd pollfd[MAXFD];
static Task *polltask[MAXFD];
static int npollfd;
static int startedfdtask;
static Tasklist sleeping;
static int sleepingcounted;
static uvlong nsec(void);

void
fdtask(void *v)
{
	int i, ms;
	Task *t;
	uvlong now;
	
    // task 标记为系统任务
	tasksystem();
	taskname("fdtask");
	for(;;){
		/* let everyone else run */
        // 让党员先跑。哦，不，让其他 task 先跑
		while(taskyield() > 0)
			;
		/* we're the only one runnable - poll for i/o */
		errno = 0;
		taskstate("poll");
        // 设置 poll 等待时间
        // 如果sleep 队列没有 task 等待唤醒，那么这个 task 也会无限等待
        // 如果没有延时任务, 启动这个 task, 说明是有其他的 fd 读写？
		if((t=sleeping.head) == nil)
			ms = -1;
		else{
			/* sleep at most 5s */
			now = nsec();
			if(now >= t->alarmtime)
				ms = 0; // poll 设置为 0 为立即返回
			else if(now+5*1000*1000*1000LL >= t->alarmtime)
				ms = (t->alarmtime - now)/1000000;
			else
				ms = 5000;
		}
		if(poll(pollfd, npollfd, ms) < 0){
			if(errno == EINTR)
				continue;
			fprint(2, "poll: %s\n", strerror(errno));
			taskexitall(0);
		}

		/* wake up the guys who deserve it */
		for(i=0; i<npollfd; i++){
			while(i < npollfd && pollfd[i].revents){
				taskready(polltask[i]);
				--npollfd;
				pollfd[i] = pollfd[npollfd];
				polltask[i] = polltask[npollfd];
			}
		}
		
		now = nsec();
        // 检查是否有已经到了唤醒时间的任务,
        // 如果有, 从sleep 队列删除, 加入到就绪队列等待执行
		while((t=sleeping.head) && now >= t->alarmtime){
			deltask(&sleeping, t);
			if(!t->system && --sleepingcounted == 0)
				taskcount--;
			taskready(t);
		}
	}
}

uint
taskdelay(uint ms)
{
	uvlong when, now;
	Task *t;
	
    // 启动 fdtask, 作为系统任务
	if(!startedfdtask){
		startedfdtask = 1;
		taskcreate(fdtask, 0, 32768);
	}

	now = nsec();
    // 设置唤醒时间
	when = now+(uvlong)ms*1000000;
    // 根据唤醒时间来对 task 进行排序
    // 这里是为了找到最后一个唤醒时间比当前任务早的task
	for(t=sleeping.head; t!=nil && t->alarmtime < when; t=t->next)
		;

	if(t){
        // t != nil, 说明是有任务的唤醒时间比当前的任务的早
        // 把当前任务放到唤醒时间比它小的位置
		taskrunning->prev = t->prev;
		taskrunning->next = t;
	}else{
        // t == nil 说明遍历到了 sleep 任务的尾部，仍然没有需要唤醒的任务
        // 也就是当前任务的唤醒时间也是最大的，放到唤醒队列的尾部
		taskrunning->prev = sleeping.tail;
		taskrunning->next = nil;
	}
	
    // 设置该任务的唤醒时间
	t = taskrunning;
	t->alarmtime = when;
    // 上面只是把该任务的指针设置好
    // 因为这个是双向链表，这里要设置前一个 task 的指针指向这个 task
	if(t->prev)
		t->prev->next = t;
	else
		sleeping.head = t;
	if(t->next)
		t->next->prev = t;
	else
		sleeping.tail = t;

    // 唤醒队列长度计数
	if(!t->system && sleepingcounted++ == 0)
		taskcount++;
    // 切换到其他的 task
	taskswitch();

    // 如果又切回来，说明该任务又加到了就绪队列，然后被执行到
    // 也就是休眠的时间到了, 被唤醒
	return (nsec() - now)/1000000;
}

void
fdwait(int fd, int rw)
{
	int bits;

	if(!startedfdtask){
		startedfdtask = 1;
		taskcreate(fdtask, 0, 32768);
	}

	if(npollfd >= MAXFD){
		fprint(2, "too many poll file descriptors\n");
		abort();
	}
	
	taskstate("fdwait for %s", rw=='r' ? "read" : rw=='w' ? "write" : "error");
	bits = 0;
	switch(rw){
	case 'r':
		bits |= POLLIN;
		break;
	case 'w':
		bits |= POLLOUT;
		break;
	}

	polltask[npollfd] = taskrunning;
	pollfd[npollfd].fd = fd;
	pollfd[npollfd].events = bits;
	pollfd[npollfd].revents = 0;
	npollfd++;
	taskswitch();
}

/* Like fdread but always calls fdwait before reading. */
int
fdread1(int fd, void *buf, int n)
{
	int m;
	
	do
		fdwait(fd, 'r');
	while((m = read(fd, buf, n)) < 0 && errno == EAGAIN);
	return m;
}

int
fdread(int fd, void *buf, int n)
{
	int m;
	
	while((m=read(fd, buf, n)) < 0 && errno == EAGAIN)
		fdwait(fd, 'r');
	return m;
}

int
fdwrite(int fd, void *buf, int n)
{
	int m, tot;
	
	for(tot=0; tot<n; tot+=m){
		while((m=write(fd, (char*)buf+tot, n-tot)) < 0 && errno == EAGAIN)
			fdwait(fd, 'w');
		if(m < 0)
			return m;
		if(m == 0)
			break;
	}
	return tot;
}

int
fdnoblock(int fd)
{
	return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL)|O_NONBLOCK);
}

// 获取当前时间，单位是纳秒
static uvlong
nsec(void)
{
	struct timeval tv;

	if(gettimeofday(&tv, 0) < 0)
		return -1;
	return (uvlong)tv.tv_sec*1000*1000*1000 + tv.tv_usec*1000;
}

