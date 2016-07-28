/* Copyright (c) 2005 Russ Cox, MIT; see COPYRIGHT */

#include "taskimpl.h"

// 创建 channel, element 指定 channel 的元素个数, bufsize 指定元素占用字节数
Channel*
chancreate(int elemsize, int bufsize)
{
	Channel *c;

	c = malloc(sizeof *c+bufsize*elemsize);
	if(c == nil){
		fprint(2, "chancreate malloc: %r");
		exit(1);
	}
	memset(c, 0, sizeof *c);
	c->elemsize = elemsize; // 元素数目
	c->bufsize = bufsize; // 元素字节数
	c->nbuf = 0; // 当前使用字节数
    // 因为 malloc 申请的是一片内存
    // 所以 buf 的偏移就是加上 channel 的头部长度
    // 这里写法有点那什么, c + 1 因为 c 结构是 channel, 所以 + 1相当于 + sizeof(*c)
	c->buf = (uchar*)(c+1);
	return c;
}

/* bug - work out races */
// 释放 channel
void
chanfree(Channel *c)
{
	if(c == nil)
		return;
	free(c->name);
	free(c->arecv.a);
	free(c->asend.a);
	free(c);
}

// 添加 Alt 到数组
static void
addarray(Altarray *a, Alt *alt)
{
	if(a->n == a->m){
		a->m += 16;
		a->a = realloc(a->a, a->m*sizeof a->a[0]);
	}
	a->a[a->n++] = alt;
}

// 元素删除
static void
delarray(Altarray *a, int i)
{
	--a->n;
	a->a[i] = a->a[a->n];
}

/*
 * doesn't really work for things other than CHANSND and CHANRCV
 * but is only used as arg to chanarray, which can handle it
 */
// op 只能是 send, recv
#define otherop(op)	(CHANSND+CHANRCV-(op))

static Altarray*
chanarray(Channel *c, uint op)
{
	switch(op){
	default:
		return nil;
	case CHANSND:
		return &c->asend;
	case CHANRCV:
		return &c->arecv;
	}
}

// alt 能否操作
// 如果可以发送或者接收数据返回 1, 其他返回 0
static int
altcanexec(Alt *a)
{
	Altarray *ar;
	Channel *c;

    // 空操作
	if(a->op == CHANNOP)
		return 0;
	c = a->c;
	if(c->bufsize == 0){ // channel 没有分配空间
		ar = chanarray(c, otherop(a->op));
		return ar && ar->n;
	}else{
		switch(a->op){
		default:
			return 0;
		case CHANSND:
            // 如果 buf 还有空间, 认为可以写入, 返回 1
			return c->nbuf < c->bufsize;
		case CHANRCV:
            // 如果 buf 还有数据, 认为有数据可以读, 返回 1
			return c->nbuf > 0;
		}
	}
}

// 根据alt 的操作类型找到 alt 队列, 并入队
static void
altqueue(Alt *a)
{
	Altarray *ar;

	ar = chanarray(a->c, a->op);
	addarray(ar, a);
}

static void
altdequeue(Alt *a)
{
	int i;
	Altarray *ar;

    // 根据操作类型，获取队列
	ar = chanarray(a->c, a->op);
	if(ar == nil){
		fprint(2, "bad use of altdequeue op=%d\n", a->op);
		abort();
	}

    // 队列查找元素并删除
	for(i=0; i<ar->n; i++)
		if(ar->a[i] == a){
			delarray(ar, i);
			return;
		}
	fprint(2, "cannot find self in altdq\n");
	abort();
}

// 删除全部的 Alt
static void
altalldequeue(Alt *a)
{
	int i;

	for(i=0; a[i].op!=CHANEND && a[i].op!=CHANNOBLK; i++)
		if(a[i].op != CHANNOP)
			altdequeue(&a[i]);
}

// 把 src 拷贝到 dest, 如果 src = nil， dst 置 0
static void
amove(void *dst, void *src, uint n)
{
	if(dst){
		if(src == nil)
			memset(dst, 0, n);
		else
			memmove(dst, src, n);
	}
}

/*
 * Actually move the data around.  There are up to three
 * players: the sender, the receiver, and the channel itself.
 * If the channel is unbuffered or the buffer is empty,
 * data goes from sender to receiver.  If the channel is full,
 * the receiver removes some from the channel and the sender
 * gets to put some in.
 */
// 用来接收或者发送的时候，进行数据拷贝
// 把变量拷贝到接收的缓冲区, 获取放到发送的缓冲区
static void
altcopy(Alt *s, Alt *r)
{
	Alt *t;
	Channel *c;
	uchar *cp;

	/*
	 * Work out who is sender and who is receiver
	 */
	if(s == nil && r == nil)
		return;
	assert(s != nil);
    // 获取源的 channel
	c = s->c;
    // 如果 s 是接收方，调换 r(recv) 和 s(send)
    // 下面就可以认为 r是 recv, s 是 send
	if(s->op == CHANRCV){
		t = s;
		s = r;
		r = t;
	}
	assert(s==nil || s->op == CHANSND);
	assert(r==nil || r->op == CHANRCV);

	/*
	 * Channel is empty (or unbuffered) - copy directly.
	 */
    // channel 是非缓冲channel(元素不是数组), 直接拷贝
	if(s && r && c->nbuf == 0){
		amove(r->v, s->v, c->elemsize);
		return;
	}

	/*
	 * Otherwise it's always okay to receive and then send.
	 */
	if(r){
        // 如果是接收, 那么就是从 buffer 读取数据
        // cp 读取位置
		cp = c->buf + c->off*c->elemsize;
        // 拷贝元素到接收者的变量 
		amove(r->v, cp, c->elemsize);
        // 缓冲区可读元素减掉一
		--c->nbuf;
        // 如果 off 到buffer末尾, 说明发送结束，从头开始
		if(++c->off == c->bufsize)
			c->off = 0;
	}
	if(s){
        // 计算接收缓冲区的位置
		cp = c->buf + (c->off+c->nbuf)%c->bufsize*c->elemsize;
        // 发送的变量拷贝到 channel的 buffer
		amove(cp, s->v, c->elemsize);
        // 缓冲区可读元素加 1
		++c->nbuf;
	}
}

static void
altexec(Alt *a)
{
	int i;
	Altarray *ar;
	Alt *other;
	Channel *c;

	c = a->c;
    // 获取出相反操作的队列， 如果 a 是 recv, 就拿出 send 队列
    // 这个是阻塞发送或者接收的场景使用, 优先处理阻塞的任务
    // 这里为什么选择相反操作的队列可以明白吧? 
    // 你发送数据的时候，这时候当然要唤醒等待读的协程了
	ar = chanarray(c, otherop(a->op));
	if(ar && ar->n){
        // 随机选择一个 Alt
		i = rand()%ar->n;
		other = ar->a[i];
        // 拷贝数据
		altcopy(a, other);
        // 删除任务已经完成的相关操作
		altalldequeue(other->xalt);
        // 假设有 a0, a1, a2，初始化的时候 a0, a1, a2 的 xalt 都指向了 a0
        // 这时候如果到 a2 阻塞了， 那么 other 就为 a2
        // other->xalt[0] 就为 a0，然后把 a0->xalt 设置 other(a2)
		other->xalt[0].xalt = other;
        // 把阻塞住的任务重新设置为就绪状态
		taskready(other->task);
	}else
		altcopy(a, nil); // 同步的时候使用, 直接写入buffer, 或者读取 buffer 
}

#define dbgalt 0
int
chanalt(Alt *a)
{
    int i, j, ncan, n, canblock;
    Channel *c;
    Task *t;

    // 检查栈空间
    needstack(512);
    // 检查 alt 数组, 直到遇到 op = CHANEND 或者 CHANNOBLK 停止
    for(i=0; a[i].op != CHANEND && a[i].op != CHANNOBLK; i++)
        ;
    n = i;
    // 判断最后一个 op 是否为 CHANEND, 是的话，操作就为 block
    canblock = a[i].op == CHANEND;

    // t 设置为当前执行的协程
    t = taskrunning;
    for(i=0; i<n; i++){
        a[i].task = t;
        a[i].xalt = a;
    }
    if(dbgalt) print("alt ");
    ncan = 0;
    for(i=0; i<n; i++){
        c = a[i].c;
        if(dbgalt) print(" %c:", "esrnb"[a[i].op]);
        if(dbgalt) { if(c->name) print("%s", c->name); else print("%p", c); }
        // 检查并计算可以执行的 Alt 数目
        if(altcanexec(&a[i])){ // 判断是否可以接收或者发送
            if(dbgalt) print("*");
            ncan++;
        }
    }

    if(ncan){
        j = rand()%ncan;
        // n 个可执行的 Alt 随机执行一条，然后返回
        for(i=0; i<n; i++){
            if(altcanexec(&a[i])){
                // j == 0 那条才会被执行, 那么有且只有一条被执行
                if(j-- == 0){
                    if(dbgalt){
                        c = a[i].c;
                        print(" => %c:", "esrnb"[a[i].op]);
                        if(c->name) print("%s", c->name); else print("%p", c);
                        print("\n");
                    }
                    // 执行
                    altexec(&a[i]);
                    return i;
                }
            }
        }
    }
    if(dbgalt)print("\n");

    // 不能阻塞就返回 -1
    if(!canblock)
        return -1;

    // 可以阻塞, 就加入异步队列, 等待可以写或者读再唤醒
    for(i=0; i<n; i++){
        if(a[i].op != CHANNOP)
            // 加入发送或者接收队列
            altqueue(&a[i]);
    }

    // 异步，先切换到其他的任务
    taskswitch();

    /*
     * the guy who ran the op took care of dequeueing us
     * and then set a[0].alt to the one that was executed.
     */
    // 再唤醒的时候会把 a[0].xalt 设置为最后一条执行的 alt
    // 两个相减就是执行的数目
    return a[0].xalt - a;
}

static int
_chanop(Channel *c, int op, void *p, int canblock)
{
	Alt a[2];

	a[0].c = c; // Alt 对应的 channel
	a[0].op = op; // 操作
	a[0].v = p; // Alt 对应的数据
	a[1].op = canblock ? CHANEND : CHANNOBLK; // 是否允许阻塞
	if(chanalt(a) < 0)
		return -1;
	return 1;
}

// 允许阻塞的发送
int
chansend(Channel *c, void *v)
{
	return _chanop(c, CHANSND, v, 1);
}

// 不允许阻塞的发送
int
channbsend(Channel *c, void *v)
{
	return _chanop(c, CHANSND, v, 0);
}

// 允许阻塞接收
int
chanrecv(Channel *c, void *v)
{
	return _chanop(c, CHANRCV, v, 1);
}

// 不允许阻塞接收
int
channbrecv(Channel *c, void *v)
{
	return _chanop(c, CHANRCV, v, 0);
}

int
chansendp(Channel *c, void *v)
{
	return _chanop(c, CHANSND, (void*)&v, 1);
}

void*
chanrecvp(Channel *c)
{
	void *v;

	_chanop(c, CHANRCV, (void*)&v, 1);
	return v;
}

int
channbsendp(Channel *c, void *v)
{
	return _chanop(c, CHANSND, (void*)&v, 0);
}

void*
channbrecvp(Channel *c)
{
	void *v;

	_chanop(c, CHANRCV, (void*)&v, 0);
	return v;
}

int
chansendul(Channel *c, ulong val)
{
	return _chanop(c, CHANSND, &val, 1);
}

ulong
chanrecvul(Channel *c)
{
	ulong val;

	_chanop(c, CHANRCV, &val, 1);
	return val;
}

int
channbsendul(Channel *c, ulong val)
{
	return _chanop(c, CHANSND, &val, 0);
}

ulong
channbrecvul(Channel *c)
{
	ulong val;

	_chanop(c, CHANRCV, &val, 0);
	return val;
}

