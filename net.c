#include "taskimpl.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/poll.h>

// 开启监听
int
netannounce(int istcp, char *server, int port)
{
	int fd, n, proto;
	struct sockaddr_in sa;
	socklen_t sn;
	uint32_t ip;

	taskstate("netannounce");
    // 协议是 tcp 还是 udp
	proto = istcp ? SOCK_STREAM : SOCK_DGRAM;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	if(server != nil && strcmp(server, "*") != 0){
        // 根据 server 来获取 ip，server 可以是 ip 或者域名
		if(netlookup(server, &ip) < 0){
			taskstate("netlookup failed");
			return -1;
		}
		memmove(&sa.sin_addr, &ip, 4);
	}
    // 端口转为网络字节序, 忘了是大端还是小端，懒，不想查
	sa.sin_port = htons(port);
    // 设置协议
	if((fd = socket(AF_INET, proto, 0)) < 0){
		taskstate("socket failed");
		return -1;
	}
	
	/* set reuse flag for tcp */
    // 设置 resue 选项, 当 time_wait 过多的时候，可以重用端口
	if(istcp && getsockopt(fd, SOL_SOCKET, SO_TYPE, (void*)&n, &sn) >= 0){
		n = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&n, sizeof n);
	}

    // 绑定监听的地址和端口到 fd
	if(bind(fd, (struct sockaddr*)&sa, sizeof sa) < 0){
		taskstate("bind failed");
		close(fd);
		return -1;
	}

    // 如果是 tcp 就开启监听
	if(proto == SOCK_STREAM)
		listen(fd, 16);

    // 设置为非阻塞
	fdnoblock(fd);
	taskstate("netannounce succeeded");
	return fd;
}

// 非阻塞的接收新连接
int
netaccept(int fd, char *server, int *port)
{
	int cfd, one;
	struct sockaddr_in sa;
	uchar *ip;
	socklen_t len;
	
    // 注册读事件, 出让 cpu 直到事件到来才会重新调度
	fdwait(fd, 'r');

	taskstate("netaccept");
	len = sizeof sa;
    // accept 新连接
	if((cfd = accept(fd, (void*)&sa, &len)) < 0){
		taskstate("accept failed");
		return -1;
	}
	if(server){
		ip = (uchar*)&sa.sin_addr;
		snprint(server, 16, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
	}
	if(port)
		*port = ntohs(sa.sin_port);
    // 连接 fd 设置为非阻塞
	fdnoblock(cfd);
	one = 1;
	setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof one);
	taskstate("netaccept succeeded");
	return cfd;
}

#define CLASS(p) ((*(unsigned char*)(p))>>6)
static int
parseip(char *name, uint32_t *ip)
{
	unsigned char addr[4];
	char *p;
	int i, x;

	p = name;
    // ipv4 解析
	for(i=0; i<4 && *p; i++){
		x = strtoul(p, &p, 0);
        // x.x.x.x ipv4 单个字段不会超过 255
		if(x < 0 || x >= 256)
			return -1;
        // 数值接着的应该为 .
		if(*p != '.' && *p != 0)
			return -1;
		if(*p == '.')
			p++;
		addr[i] = x;
	}

    // 根据第一个数值的高两位来确定 ip 地址的类型
    // 0 为 A 类, 1.0.0.1－126.255.255.254
    // 1 为 B 类, 128.1.0.1－191.255.255.254
    // 2 为 C 类, 192.0.1.1－223.255.255.254
    // 如果不足 4个段，那么低的段使用 0
	switch(CLASS(addr)){
	case 0:
	case 1:
		if(i == 3){
			addr[3] = addr[2];
			addr[2] = addr[1];
			addr[1] = 0;
		}else if(i == 2){
			addr[3] = addr[1];
			addr[2] = 0;
			addr[1] = 0;
		}else if(i != 4)
			return -1;
		break;
	case 2:
		if(i == 3){
			addr[3] = addr[2];
			addr[2] = 0;
		}else if(i != 4)
			return -1;
		break;
	}
	*ip = *(uint32_t*)addr;
	return 0;
}

int
netlookup(char *name, uint32_t *ip)
{
	struct hostent *he;

    // 解析是否为ip
	if(parseip(name, ip) >= 0)
		return 0;
	
	/* BUG - Name resolution blocks.  Need a non-blocking DNS. */
	taskstate("netlookup");
    // 这里的 DNS 解析会阻塞, 需要非阻塞的 DNS 解析
	if((he = gethostbyname(name)) != 0){
		*ip = *(uint32_t*)he->h_addr;
		taskstate("netlookup succeeded");
		return 0;
	}
	
    // 解析失败
	taskstate("netlookup failed");
	return -1;
}

int
netdial(int istcp, char *server, int port)
{
	int proto, fd, n;
	uint32_t ip;
	struct sockaddr_in sa;
	socklen_t sn;
	
    // 解析 ip
	if(netlookup(server, &ip) < 0)
		return -1;

	taskstate("netdial");
    // tcp 还是 udp
	proto = istcp ? SOCK_STREAM : SOCK_DGRAM;
	if((fd = socket(AF_INET, proto, 0)) < 0){
		taskstate("socket failed");
		return -1;
	}
    // 设置为非阻塞
	fdnoblock(fd);

	/* for udp */
    // udp 才设置的选项
	if(!istcp){
		n = 1;
		setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &n, sizeof n);
	}
	
	/* start connecting */
	memset(&sa, 0, sizeof sa);
	memmove(&sa.sin_addr, &ip, 4);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
    // 连接服务
	if(connect(fd, (struct sockaddr*)&sa, sizeof sa) < 0 && errno != EINPROGRESS){
		taskstate("connect failed");
		close(fd);
		return -1;
	}

	/* wait for finish */	
    // 等待连接成功, 变成可写即是连接成功
	fdwait(fd, 'w');
	sn = sizeof sa;
    // 获取远程ip和端口
	if(getpeername(fd, (struct sockaddr*)&sa, &sn) >= 0){
		taskstate("connect succeeded");
		return fd;
	}
	
	/* report error */
	sn = sizeof n;
    // 获取是否有连接错误
	getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*)&n, &sn);
	if(n == 0)
		n = ECONNREFUSED;
	close(fd);
	taskstate("connect failed");
	errno = n;
	return -1;
}

