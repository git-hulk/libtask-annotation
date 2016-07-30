#include "taskimpl.h"

/*
 * locking
 */
static int
_qlock(QLock *l, int block)
{
    // 锁没人占用, 当前的协程拿到，好开心，赶快跑
	if(l->owner == nil){
		l->owner = taskrunning;
		return 1;
	}
    // 锁被别人占用, 如果不阻塞等待，那么直接返回
	if(!block)
		return 0;
    // 把需要等待的协程加入队列, unlock 的时候会把协程加入可运行队列，重新调度
	addtask(&l->waiting, taskrunning);
    // 状态设置为等待锁
	taskstate("qlock");
    // 出让 cpu
	taskswitch();
    // 重新调度的时候，如果发现锁拥有者，说明调度出问题了。
    // 因为 unlock 把等待协程加入可运行队列的前提是把锁所有权给这个等待的协程
	if(l->owner != taskrunning){
		fprint(2, "qlock: owner=%p self=%p oops\n", l->owner, taskrunning);
		abort();
	}
	return 1;
}

// 如果锁正在被其他的协程占用，会阻塞等待
void
qlock(QLock *l)
{
	_qlock(l, 1);
}

// 只是尝试获取锁, 如果被其他地方占用，就不等待返回
// 跟 try lock 类似
int
canqlock(QLock *l)
{
	return _qlock(l, 0);
}

// 释放锁
void
qunlock(QLock *l)
{
	Task *ready;
	
    // 如果锁不是自己，那你释放毛呢~~
    // 这个直接 abort, 好那个..
	if(l->owner == 0){
		fprint(2, "qunlock: owner=0\n");
		abort();
	}
    // 释放锁的时候，检查一下等待队列，如果有就拿到第一个，然后把他设置为锁的拥有者
    // 再把获得锁的协程设置为可运行状态
	if((l->owner = ready = l->waiting.head) != nil){
        // 从等待队列删除
		deltask(&l->waiting, ready);
        // 加入到可运行队列，等待调度
		taskready(ready);
	}
}

// 读写锁，这个和上面的互斥锁有什么区别，或者使用场景?
// 我是不会说的
static int
_rlock(RWLock *l, int block)
{
    // 检查当前是否有正在写入? 如果没有对读计数 +1
    // 读写锁的读锁是可以共享的，写是互斥的(包括和读互斥)
	if(l->writer == nil && l->wwaiting.head == nil){
		l->readers++;
		return 1;
	}

    // 同样判断是否等待
	if(!block)
		return 0;
    // 添加到等待队列
	addtask(&l->rwaiting, taskrunning);
    // 状态设置为等待读锁
	taskstate("rlock");
    // 切换到其他协程
	taskswitch();
	return 1;
}

// 阻塞的获取读锁
void
rlock(RWLock *l)
{
	_rlock(l, 1);
}

// 非阻塞的获取读锁, 拿不到就快跑
int
canrlock(RWLock *l)
{
	return _rlock(l, 0);
}

// 获取写锁
static int
_wlock(RWLock *l, int block)
{
    // 因为写和其他的读写操作是互斥的, 获取锁的前提是没有人正在读写
	if(l->writer == nil && l->readers == 0){
		l->writer = taskrunning;
		return 1;
	}
    // 叔叔我们不等了
	if(!block)
		return 0;
    // 把协程加入写等待队列
	addtask(&l->wwaiting, taskrunning);
    // 设置为等待写锁
	taskstate("wlock");
    // cpu 切换去干点别的吧
	taskswitch();
	return 1;
}

// 阻塞的等待写锁
void
wlock(RWLock *l)
{
	_wlock(l, 1);
}

// 非阻塞的等待写锁
int
canwlock(RWLock *l)
{
	return _wlock(l, 0);
}

// 读锁释放
void
runlock(RWLock *l)
{
	Task *t;

    // 对当前读的数量 -1， 如果发现没人占用了读锁, 就看看是否有等待写锁的
    // 有的话，就把这个等待的协程加入到可运行队列
	if(--l->readers == 0 && (t = l->wwaiting.head) != nil){
		deltask(&l->wwaiting, t);
		l->writer = t;
		taskready(t);
	}
}

// 写锁释放
void
wunlock(RWLock *l)
{
	Task *t;
	
    // 写锁又不是你拿的， 你释放个 jj
	if(l->writer == nil){
		fprint(2, "wunlock: not locked\n");
		abort();
	}
    // 把写锁拥有者清空
	l->writer = nil;
    // 这时候不可能有读, 因为读写是互斥的
	if(l->readers != 0){
		fprint(2, "wunlock: readers\n");
		abort();
	}
    // 把所有等待读锁的协程都变成可运行状态
	while((t = l->rwaiting.head) != nil){
		deltask(&l->rwaiting, t);
		l->readers++;
		taskready(t);
	}
    // 上面先判断上面是否有等待读锁，这里再来判断是否等待写的协程
    // 也就是说读优先
	if(l->readers == 0 && (t = l->wwaiting.head) != nil){
		deltask(&l->wwaiting, t);
		l->writer = t;
		taskready(t);
	}
}
