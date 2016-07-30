#include "taskimpl.h"

/*
 * sleep and wakeup
 */
// 类似条件变量的功能
// tasksleep 等待某一个资源，然后进入休眠状态
// 当条件满足的时候去调用 taskwakeup 来唤醒协程来
void
tasksleep(Rendez *r)
{
    // 把当前运行的协程假如等待队列
	addtask(&r->waiting, taskrunning);
    // 如果占用着锁就先释放
	if(r->l)
		qunlock(r->l);
	taskstate("sleep");
    // 释放 Cpu, 切换到调度协程
	taskswitch();
    // 如果有锁, 被唤醒的时候要去获取锁 
	if(r->l)
		qlock(r->l);
}

// 唤醒协程
static int
_taskwakeup(Rendez *r, int all)
{
	int i;
	Task *t;

	for(i=0;; i++){
        // 只唤醒一个?
		if(i==1 && !all)
			break;
        // 如果没有休眠的协程, 就退出
		if((t = r->waiting.head) == nil)
			break;
        // 从休眠队列删除, 加入就绪队列
		deltask(&r->waiting, t);
		taskready(t);
	}
	return i;
}

// 唤醒一个任务
int
taskwakeup(Rendez *r)
{
	return _taskwakeup(r, 0);
}

// 唤醒所有任务
int
taskwakeupall(Rendez *r)
{
	return _taskwakeup(r, 1);
}

