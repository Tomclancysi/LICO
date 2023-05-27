/**
 * 写法参考了 https://github.com/Tencent/libco，但是仅保留其中重要的部分
*/
#pragma once
#include <cassert>
#include <cstring>
#include <list>
#include <memory>
#include <vector>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/poll.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <functional>

typedef void (*co_func)(void *);
typedef void* co_args;
typedef unsigned long long ull;

#define NODEBUG
#define CO_STACK_SIZE (1024*10)
#define MAX_SLOT_NUM_MS (1000*10)


static uint32_t PollEvent2Epoll( short events )
{
	uint32_t e = 0;
	if( events & POLLIN ) 	e |= EPOLLIN;
	if( events & POLLOUT )  e |= EPOLLOUT;
	if( events & POLLHUP ) 	e |= EPOLLHUP;
	if( events & POLLERR )	e |= EPOLLERR;
	if( events & POLLRDNORM ) e |= EPOLLRDNORM;
	if( events & POLLWRNORM ) e |= EPOLLWRNORM;
	return e;
}
static short EpollEvent2Poll( uint32_t events )
{
	short e = 0;
	if( events & EPOLLIN ) 	e |= POLLIN;
	if( events & EPOLLOUT ) e |= POLLOUT;
	if( events & EPOLLHUP ) e |= POLLHUP;
	if( events & EPOLLERR ) e |= POLLERR;
	if( events & EPOLLRDNORM ) e |= POLLRDNORM;
	if( events & EPOLLWRNORM ) e |= POLLWRNORM;
	return e;
}

struct CoEnvir;
struct coctx_t
{ // x_86_64汇编
	void *regs[ 14 ]; // 这个数组还牵扯到汇编以及内存布局，非常detail。一个struct内部每一项的位置是什么？
	[[maybe_unused]] size_t ss_size; // 参考libco的，但这两个作用不太清楚，应该共享栈才需要
	[[maybe_unused]] char *ss_sp;
};

struct co_stack
{
    char stack_buffer[CO_STACK_SIZE];
    size_t stack_size; // 这个值虽然没用到，但是可以把他看做哨兵，如果不存在对齐问题时，co_func返回地址会保存在这个值上。对齐时，保存在它前面
};


// 一个协程需要的数据
struct co_struct
{
    co_func func;
    co_args args;

    CoEnvir *env;
    coctx_t *ctx;
    co_stack *stack;

    bool isStarted;
    bool isEnded;

};

struct co_condition_variable {
    std::list<co_struct*> _waitingCoList;
    int wait(std::function<bool()> predicate);
    int wait_for(std::function<bool()> predicate, ull ms);
    void notify_one();
    void notify_all();
};


void co_create(co_struct **pico, co_func func, co_args args);

void co_release(co_struct *co);

void co_resume(co_struct *co);

void co_yield();

void co_sleep(ull ms);

int co_poll(struct pollfd fds[], nfds_t nfds, int timeout);

void co_eventloop(); // cpu大循环，类似JS的事件一样的机制

ull GetTimeStamp();

void co_global_init();

void co_global_release();
