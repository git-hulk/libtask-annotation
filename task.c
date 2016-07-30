/* Copyright (c) 2005 Russ Cox, MIT; see COPYRIGHT */

#include "taskimpl.h"
#include <fcntl.h>
#include <stdio.h>

int	taskdebuglevel;
int	taskcount;
int	tasknswitch;
int	taskexitval;
Task	*taskrunning;

Context	taskschedcontext;
Tasklist	taskrunqueue; // 就绪队列

Task	**alltask;
int		nalltask;

static char *argv0;
static	void		contextswitch(Context *from, Context *to);

static void
taskdebug(char *fmt, ...)
{
	va_list arg;
	char buf[128];
	Task *t;
	char *p;
	static int fd = -1;

return;
	va_start(arg, fmt);
	vfprint(1, fmt, arg);
	va_end(arg);
return;

	if(fd < 0){
		p = strrchr(argv0, '/');
		if(p)
			p++;
		else
			p = argv0;
		snprint(buf, sizeof buf, "/tmp/%s.tlog", p);
		if((fd = open(buf, O_CREAT|O_WRONLY, 0666)) < 0)
			fd = open("/dev/null", O_WRONLY);
	}

	va_start(arg, fmt);
	vsnprint(buf, sizeof buf, fmt, arg);
	va_end(arg);
	t = taskrunning;
	if(t)
		fprint(fd, "%d.%d: %s\n", getpid(), t->id, buf);
	else
		fprint(fd, "%d._: %s\n", getpid(), buf);
}

static void
taskstart(uint y, uint x)
{
	Task *t;
	ulong z;

	z = x<<16;	/* hide undefined 32-bit shift from 32-bit compilers */
	z <<= 16;
	z |= y;
	t = (Task*)z;

//print("taskstart %p\n", t);
	t->startfn(t->startarg);
//print("taskexits %p\n", t);
	taskexit(0);
//print("not reacehd\n");
}

static int taskidgen;
 
static Task*
taskalloc(void (*fn)(void*), void *arg, uint stack)
{
	Task *t;
	sigset_t zero;
	uint x, y;
	ulong z;

	/* allocate the task and stack together */
    // 协程结构和栈空间在连续的内存，这个可以有效减少内存碎片
	t = malloc(sizeof *t+stack);
	if(t == nil){
		fprint(2, "taskalloc malloc: %r\n");
		abort();
	}

	memset(t, 0, sizeof *t);
    // 栈指针
	t->stk = (uchar*)(t+1);
    // 栈大小
	t->stksize = stack;
    // 协程id
	t->id = ++taskidgen;
    // 协程对应执行方法
	t->startfn = fn;
    // 参数
	t->startarg = arg;

	/* do a reasonable initialization */
	memset(&t->context.uc, 0, sizeof t->context.uc);
	sigemptyset(&zero);
	sigprocmask(SIG_BLOCK, &zero, &t->context.uc.uc_sigmask);

	/* must initialize with current context */
    // 获取当前上下文作为当前协程的上下文
	if(getcontext(&t->context.uc) < 0){
		fprint(2, "getcontext: %r\n");
		abort();
	}

	/* call makecontext to do the real work. */
	/* leave a few words open on both ends */
    // 在栈的开头和结束分别留了 8 和 64 bytes空间，不知道干嘛
	t->context.uc.uc_stack.ss_sp = t->stk+8;
	t->context.uc.uc_stack.ss_size = t->stksize-64;
#if defined(__sun__) && !defined(__MAKECONTEXT_V2_SOURCE)		/* sigh */
#warning "doing sun thing"
	/* can avoid this with __MAKECONTEXT_V2_SOURCE but only on SunOS 5.9 */
    // SunOS5.9 的栈是向低地址增长
	t->context.uc.uc_stack.ss_sp = 
		(char*)t->context.uc.uc_stack.ss_sp
		+t->context.uc.uc_stack.ss_size;
#endif
	/*
	 * All this magic is because you have to pass makecontext a
	 * function that takes some number of word-sized variables,
	 * and on 64-bit machines pointers are bigger than words.
	 */
//print("make %p\n", t);
    // 把 64bit 的指针 z 拆分成两个 32bit 的 x << 32 | y
    // 避免不兼容
	z = (ulong)t;
	y = z;
	z >>= 16;	/* hide undefined 32-bit shift from 32-bit compilers */
	x = z>>16;
    // 函数是干嘛见 man(3) makecontext 好么
    // 对用户线程的上下文信息进行修改, 包括指定新的栈地址等
	makecontext(&t->context.uc, (void(*)())taskstart, 2, y, x);

	return t;
}

// 使用函数 fn 创建一个新的协程, 并添加到协程队列
int
taskcreate(void (*fn)(void*), void *arg, uint stack)
{
	int id;
	Task *t;

    // 申请协程结构的空间以及初始化
	t = taskalloc(fn, arg, stack);
    // 当前协程数目 +1
	taskcount++;
	id = t->id;
    // 存储所有协程的数组是否需要扩容
    // 为什么是 nalltask % 64 == 0 是扩容的条件? 
    // 因为每次增加64，所以每当能整除的时候说明，空间用完
	if(nalltask%64 == 0){
		alltask = realloc(alltask, (nalltask+64)*sizeof(alltask[0]));
		if(alltask == nil){
			fprint(2, "out of memory\n");
			abort();
		}
	}

    // 放到数组尾部
	t->alltaskslot = nalltask;
	alltask[nalltask++] = t;

    // 协程状态设置为就绪, 可以进行调度
	taskready(t);
	return id;
}

// 把当前的协程标识为系统协程
void
tasksystem(void)
{
	if(!taskrunning->system){
		taskrunning->system = 1;
		--taskcount;
	}
}

// 协程切换
void
taskswitch(void)
{
	needstack(0);
	contextswitch(&taskrunning->context, &taskschedcontext);
}

// 协程添加到就绪队列
void
taskready(Task *t)
{
	t->ready = 1;
	addtask(&taskrunqueue, t);
}

// 协程让出 cpu
int
taskyield(void)
{
	int n;
	
	n = tasknswitch;
    // 添加到就绪队列的尾部, 相当于让排在它后面的协程优先调度
	taskready(taskrunning);
    // 状态标识为 yield
	taskstate("yield");
    // 切换上下文
	taskswitch();
    // tasknswitch表示协程调度次数，出让到再次调用，两次相减就是出让的调度次数
	return tasknswitch - n - 1;
}

// 是否有就绪的协程可以调度
int
anyready(void)
{
	return taskrunqueue.head != nil;
}

// 退出所有协程
void
taskexitall(int val)
{
	exit(val);
}

// 退出当前执行的协程
void
taskexit(int val)
{
	taskexitval = val;
	taskrunning->exiting = 1;
	taskswitch();
}

// 上下文切换, 相当于协程切换
static void
contextswitch(Context *from, Context *to)
{
    // swapcontext功能见 man(3) swapcontext
	if(swapcontext(&from->uc, &to->uc) < 0){
		fprint(2, "swapcontext failed: %r\n");
		assert(0);
	}
}

static void
taskscheduler(void)
{
	int i;
	Task *t;

	taskdebug("scheduler enter");
	for(;;){
        // 当前除了系统协程外没有其他协程, 退出
		if(taskcount == 0)
			exit(taskexitval);

        // 处理的顺序是 FIFO
		t = taskrunqueue.head;
		if(t == nil){
			fprint(2, "no runnable tasks! %d tasks stalled\n", taskcount);
			exit(1);
		}
        // 从就绪队列里面去掉这个协程
		deltask(&taskrunqueue, t);
		t->ready = 0;

        // 把拿出来的就绪任务设置为当前正在执行的任务 
		taskrunning = t;
        // 协程调度计数 +1
		tasknswitch++;
		taskdebug("run %d (%s)", t->id, t->name);

        // 切换到刚拿到的协程, 调度的核心
        // 使用 taskschedcontext 保留老的堆栈状态, 把 t->context 设置为新的执行上下文
        // 后面通过 taskswitch 切换回调度协程
		contextswitch(&taskschedcontext, &t->context);
//print("back in scheduler\n");
		taskrunning = nil;

		if(t->exiting){ // 协程执行完退出
            // 系统协程不计数
			if(!t->system)
				taskcount--;

            // 把数组中最后一个任务放到要删除的任务的位置
            // 这里判断一下当前任务是不是已经在数据尾部会更加优雅?
			i = t->alltaskslot;
			alltask[i] = alltask[--nalltask];
			alltask[i]->alltaskslot = i;
            // 释放任务
			free(t);
		}
	}
}

void**
taskdata(void)
{
	return &taskrunning->udata;
}

/*
 * debugging
 */
void
taskname(char *fmt, ...)
{
	va_list arg;
	Task *t;

	t = taskrunning;
	va_start(arg, fmt);
	vsnprint(t->name, sizeof t->name, fmt, arg);
	va_end(arg);
}

char*
taskgetname(void)
{
	return taskrunning->name;
}

// 设置当前协程状态
void
taskstate(char *fmt, ...)
{
	va_list arg;
	Task *t;

	t = taskrunning;
	va_start(arg, fmt);
	vsnprint(t->state, sizeof t->name, fmt, arg);
	va_end(arg);
}

// 获取当前协程状态
char*
taskgetstate(void)
{
	return taskrunning->state;
}

// 检查栈空间是否足够 n bytes
void
needstack(int n)
{
	Task *t;

	t = taskrunning;

    // 实现检查的技巧比较 hacker
    // 通过创建临时变量t, 这时候t一定是在栈顶
    // 栈顶一定大于栈底
	if((char*)&t <= (char*)t->stk
	|| (char*)&t - (char*)t->stk < 256+n){
		fprint(2, "task stack overflow: &t=%p tstk=%p n=%d\n", &t, t->stk, 256+n);
		abort();
	}
}

// 打印协程信息, 收到退出信号的时候会调用
static void
taskinfo(int s)
{
	int i;
	Task *t;
	char *extra;

	fprint(2, "task list:\n");
	for(i=0; i<nalltask; i++){
		t = alltask[i];
		if(t == taskrunning) // 嗯, 因为是单线程, 所以正在运行一定也只有一个
			extra = " (running)";
		else if(t->ready)
			extra = " (ready)";
		else
			extra = "";
		fprint(2, "%6d%c %-20s %s%s\n", 
			t->id, t->system ? 's' : ' ', 
			t->name, t->state, extra);
	}
}

/*
 * startup
 */

static int taskargc;
static char **taskargv;
int mainstacksize;

// 主协程
static void
taskmainstart(void *v)
{
	taskname("taskmain");
	taskmain(taskargc, taskargv);
}

int
main(int argc, char **argv)
{
	struct sigaction sa, osa;

	memset(&sa, 0, sizeof sa);

    // 设置 quit 信号时，对应处理函数为 taskinfo
    // 这个函数会在收到退出信号时, 打印当前的所有协程信息
	sa.sa_handler = taskinfo;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGQUIT, &sa, &osa);

#ifdef SIGINFO
	sigaction(SIGINFO, &sa, &osa);
#endif

	argv0 = argv[0];
	taskargc = argc;
	taskargv = argv;

    // 设置默认的最小栈大小
	if(mainstacksize == 0)
		mainstacksize = 256*1024;

    // 创建一个主协程
	taskcreate(taskmainstart, nil, mainstacksize);
    // 开始协程调度
	taskscheduler();

	fprint(2, "taskscheduler returned in main!\n");
	abort();
	return 0;
}

/*
 * hooray for linked lists
 */
void
addtask(Tasklist *l, Task *t)
{
	if(l->tail){
		l->tail->next = t;
		t->prev = l->tail;
	}else{
		l->head = t;
		t->prev = nil;
	}
	l->tail = t;
	t->next = nil;
}

void
deltask(Tasklist *l, Task *t)
{
	if(t->prev)
		t->prev->next = t->next;
	else
		l->head = t->next;
	if(t->next)
		t->next->prev = t->prev;
	else
		l->tail = t->prev;
}

// 协程id
unsigned int
taskid(void)
{
	return taskrunning->id;
}

