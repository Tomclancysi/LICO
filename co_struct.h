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

typedef void (*co_func)(void *);

typedef void* co_args;

#define NODEBUG 1
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

struct co_env;
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

    co_env *env;
    coctx_t *ctx;
    co_stack *stack;

    bool isStarted;
    bool isEnded;

};

struct co_epoll_ctl
{ // 通过hook各种可能阻塞的操作，用epoll管理，来实现协程的调度
    int iEpollFd;
    // 需要维护fd和协程的映射关系（还需要管理这个协程的生命周期吗？）
};

// 1. 时间轮，处理定时任务
// 拆分任务，时间轮需要处理的任务有：
typedef unsigned long long ull;

ull GetTimeStamp() {
    struct timeval now = {0};
    gettimeofday(&now, nullptr);
    return now.tv_sec * 1000 + now.tv_usec / 1000;
}

void co_switch_to(co_struct *co);
void co_onEventTrigger(void *vArgs);
co_struct* get_curr_co();
void co_switch_to(void *co_item);

struct co_slot_item {
    // 作为一个定时器，需要记录超时时间，以及超时后的回调函数？（sleep也很常用，用poll实现）
    ull expireTimeStamp;
    co_func expireCallback; // 调用上面的switchto 直接Switch to原来的coroutine
    co_func eventTriggerCallback;
    co_struct *co;
};

typedef std::vector<std::list<co_slot_item>> Slots;
struct co_timing_wheel {
    int slotNum;

    int curSlot;      // 当前slot
    ull curTimeStamp; // 当前时间戳

    std::unique_ptr<Slots> pSlots;
};

void AdvanceToClearMayBeExpiredSlots(co_timing_wheel *pTimingWheel, std::list<co_slot_item> *pExpiredSlots, ull now) {
    int curSlot = pTimingWheel->curSlot;
    int slotNum = pTimingWheel->slotNum;

    int advance = now - pTimingWheel->curTimeStamp; // advance 个slot都要被清除掉，串到pExpiredSlots上
    if (advance < 0) {
        return;
    }
    if (advance > slotNum) {
        advance = slotNum;
    }
    for (int i = 0; i < advance; ++i) {
        curSlot = (curSlot + 1) % slotNum;
        std::list<co_slot_item> &slot = (*pTimingWheel->pSlots)[curSlot];
        if (!slot.empty())
            pExpiredSlots->splice(pExpiredSlots->end(), slot); // list is slow, but it is simple
    }

    pTimingWheel->curSlot = curSlot;
    pTimingWheel->curTimeStamp = now;

}

bool AddToTimingWheel(co_timing_wheel *pTimingWheel, co_slot_item *pSlotItem, ull now) {
    int slotNum = pTimingWheel->slotNum;
    int curSlot = pTimingWheel->curSlot;
    ull curTimeStamp = pTimingWheel->curTimeStamp;

    if (!(curTimeStamp <= now && now <= pSlotItem->expireTimeStamp)) {
        return false;
    }

    int diff = pSlotItem->expireTimeStamp - curTimeStamp; // 从curTimestamp往后数diff个，放到相应的位置（有个问题：如果有个协程占据了很长时间，这个push是否会有延迟？
    if (diff <= 0) {
        return false;
    }
    int slotIndex = (curSlot + diff) % slotNum;
    (*pTimingWheel->pSlots)[slotIndex].push_back(*pSlotItem);
    return true;
}

void checkTimingWheel(co_timing_wheel *pTimingWheel) {
    printf("curSlot %d\n", pTimingWheel->curSlot);
    for (int i = 0; i < pTimingWheel->slotNum; ++i) {
        if (pTimingWheel->pSlots->at(i).size() > 0) {
            printf("slot %d has %lu items\n", i, pTimingWheel->pSlots->at(i).size());
        }
    }
}

// 2 epoll 托管子poll请求，高效的很
#define EPOLL_MAX_LENGTH 2048
struct epoll_req_wrapper {
    int epollFd;
    epoll_event *events;
};


struct co_timed_poll : public co_slot_item {
    int envEpollFd;

    int nfds;
    pollfd *events;

    int nPrepared;     // 0
    bool hasSubReqTrg;
};

struct co_poll_subreq {
    co_timed_poll *pTimedPoll; // the main request
    int idxInEvents;
};

void co_init_poll(co_timed_poll *cop, int epfd, pollfd *fds, int nfds, int timeout) {
    cop->co = get_curr_co();
    cop->envEpollFd = epfd;
    cop->events = fds;
    cop->nfds = nfds;
    cop->nPrepared = 0;
    cop->hasSubReqTrg = false;

    cop->eventTriggerCallback = co_onEventTrigger;
    cop->expireCallback = co_switch_to;
    cop->expireTimeStamp = GetTimeStamp() + timeout;
}

struct co_env
{ // 协程执行的环境，包括epoll，当前协程，上次挂起的协程，协程的调度栈 (把协程想象成一个特殊的函数,这个函数被call的时候可以从中间执行)
	co_struct *pCallStack[ 128 ]; // 这个callstack属实有点小了
	int iCallStackSize;
    co_timing_wheel *pTimingWheel;
    epoll_req_wrapper *pEpollIns;
};


// 2. 协程调度器 处理读写等阻塞操作


// 3. CO conddition variable，协程之间的条件变量


static co_env glbEnv;

// #-------------------------------------------------------------------------------

//------------- Copied from libco context switch(ucontex)
// 64 bit
// low | regs[0]: r15 |
//    | regs[1]: r14 |
//    | regs[2]: r13 |
//    | regs[3]: r12 |
//    | regs[4]: r9  |
//    | regs[5]: r8  |
//    | regs[6]: rbp |
//    | regs[7]: rdi |
//    | regs[8]: rsi |
//    | regs[9]: ret |  //ret func addr
//    | regs[10]: rdx |
//    | regs[11]: rcx |
//    | regs[12]: rbx |
// hig | regs[13]: rsp |
enum {
  kRBP = 6, // 为什么libco没设置rbp
  kRDI = 7,
  kRSI = 8,
  kRETAddr = 9,
  kRSP = 13,
};

// 64 bit
extern "C" {
extern void coctx_swap(coctx_t*, coctx_t*) asm("coctx_swap"); // 寄存器操作:esp
};

// int coctx_make(coctx_t* ctx, co_func pfn/*回退函数*/, const void* s, const void* s1) {
//   char* sp = ctx->ss_sp + ctx->ss_size - sizeof(void*); // 第一次执行的时候,将contex设置了返回地址是一个yield函数
//   sp = (char*)((unsigned long)sp & -16LL); // 应该是为了对齐

//   memset(ctx->regs, 0, sizeof(ctx->regs));
//   void** ret_addr = (void**)(sp); // sp指向一个void*
//   *ret_addr = (void*)pfn;

//   ctx->regs[kRSP] = sp;

//   ctx->regs[kRETAddr] = (char*)pfn;

//   ctx->regs[kRDI] = (char*)s; // 返回地址那个函数对应的参数,汇编这边不太清楚
//   ctx->regs[kRSI] = (char*)s1; // 入参
//   return 0;
// }

//-------------

// init data structure
void co_struct_init(co_struct **pico, co_func func, co_args args) {
    co_struct *co = (co_struct*)calloc(1, sizeof(co_struct));
    co->func = func;
    co->args = args;

    co->env = &glbEnv;
    co->ctx = (coctx_t*)calloc(1, sizeof(coctx_t));
    co->stack = (co_stack*)calloc(1, sizeof(co_stack));

    *pico = co;
}

void co_struct_release(co_struct *co) {
    free(co->ctx);
    free(co->stack);
    free(co);
}

void co_timing_wheel_init(co_timing_wheel **tw) {
    co_timing_wheel *t = new co_timing_wheel;
    t->slotNum = MAX_SLOT_NUM_MS;
    t->curSlot = 0;
    t->curTimeStamp = GetTimeStamp();
    t->pSlots.reset(new Slots(t->slotNum));
    *tw = t;
}
void co_timing_wheel_release(co_timing_wheel *tw) {
    delete tw;
}

void co_global_init() {
    memset(&glbEnv, 0, sizeof(glbEnv));
    glbEnv.iCallStackSize = 1; // 默认有个全局env 也就是main
    co_struct_init(&glbEnv.pCallStack[0], NULL, NULL);
    co_timing_wheel_init(&glbEnv.pTimingWheel);
    glbEnv.pEpollIns = (epoll_req_wrapper*)calloc(1, sizeof(epoll_req_wrapper));
    glbEnv.pEpollIns->epollFd = epoll_create(1);
    if (glbEnv.pEpollIns->epollFd < 0) {
        perror("epoll_create"); exit(1);
    }
    glbEnv.pEpollIns->events = (epoll_event*)malloc(sizeof(epoll_event) * EPOLL_MAX_LENGTH);
    if (glbEnv.pEpollIns->events == NULL) {
        perror("malloc fail"); exit(1);
    }
}
void co_global_release() {
    for (int i = 0; i < glbEnv.iCallStackSize; ++i) {
        co_struct_release(glbEnv.pCallStack[i]);
    }
    co_timing_wheel_release(glbEnv.pTimingWheel);
    close(glbEnv.pEpollIns->epollFd);
    free(glbEnv.pEpollIns->events);
    free(glbEnv.pEpollIns);
}

void co_swap(co_struct *curr, co_struct *next) {
    // 一个神奇的函数，直接就把当前的上下文切换到next的上下文

    coctx_swap(curr->ctx, next->ctx); // 此时函数已经到next中继续运行了，如何回到这个地址？

}

void co_finish() { // 如果协程函数执行完毕，ret会返回到这个函数的地址上，从而swap到协程的父协程
    co_struct *curr = glbEnv.pCallStack[glbEnv.iCallStackSize-1];
    co_struct *next = glbEnv.pCallStack[glbEnv.iCallStackSize-2];
    // maybe need some garbage collection? maybe not
    glbEnv.iCallStackSize--;
    curr->isEnded = true;

    co_swap(curr, next);
}

void co_resume(co_struct *co) {
    if (!co->isStarted) {
        // 协程初始化时，需要设置 栈，寄存器，返回地址
        co->ctx->regs[kRETAddr] = (char*)co->func;
        co->ctx->regs[kRDI] = (char*)co->args;

        // RBP RSP应该设置为正确的堆栈地址,RBP前面应该留一个返回地址的位置，函数执行完之后返回到这个函数里，没有显示yield
        char *stBase = (char*)( (u_int64_t)(&co->stack->stack_buffer[CO_STACK_SIZE-1]) & (-16LL) );
        *((u_int64_t*)stBase + 1) = (u_int64_t)co_finish;
        co->ctx->regs[kRBP] = stBase;
        co->ctx->regs[kRSP] = stBase; // 这个函数的返回地址已经被安排了

        co->isStarted = true;
    }
    if (co->isEnded) {
        printf("Error co is ended\n");
        exit(-1);
    }

    co_struct *curr = glbEnv.pCallStack[glbEnv.iCallStackSize - 1];
    glbEnv.pCallStack[glbEnv.iCallStackSize++] = co;

    co_swap( curr,co );
}

void co_yield() {
    // 直接yield到env，让到栈上的下一个协程
    co_struct *curr = glbEnv.pCallStack[glbEnv.iCallStackSize - 1];
    co_struct *next = glbEnv.pCallStack[glbEnv.iCallStackSize - 2];
    glbEnv.iCallStackSize--;

    co_swap( curr,next );
}

// call back function
void co_switch_to(void *co_item) {
    co_slot_item *item = (co_slot_item*)co_item;
    co_resume(item->co);
}

struct EventTriggerArgs {
    epoll_event ev;
    co_poll_subreq *subreq;
    std::list<co_slot_item> *mayTimeOut;
};

void co_onEventTrigger(void *vArgs) {
    EventTriggerArgs *args = (EventTriggerArgs *)vArgs;
    co_timed_poll *pMainPool = args->subreq->pTimedPoll;
    // pollfd相应位置设置为ev的值
    pMainPool->nPrepared ++;
    pMainPool->events[args->subreq->idxInEvents].revents = args->ev.events;

    if (!pMainPool->hasSubReqTrg) {
        pMainPool->hasSubReqTrg = true;
        (*args->mayTimeOut).push_back(*pMainPool);
    }
}


//

co_struct* get_curr_co() {
    return glbEnv.pCallStack[glbEnv.iCallStackSize - 1];
}

void co_sleep(ull ms) {
    // 往时间轮中添加一个定时器，把当前协程挂起(struct最好用仅包含指针的struct，这样拷贝起来也轻量)
    // 如果是main协程，那么就直接sleep，不需要挂起
    co_slot_item item;
    auto now = GetTimeStamp();

    item.co = get_curr_co();
    item.expireCallback = co_switch_to;
    item.expireTimeStamp = now + ms;
    if(!AddToTimingWheel(glbEnv.pTimingWheel, &item, now)) { // 期望它timeout的时候返回
        printf("Error add to timing wheel\n");
        return;
    } else {
#ifdef DEBUG
        printf("Add to timing wheel\n");
#endif
    }
    co_yield();
}

int co_blocked_poll(struct pollfd fds[], nfds_t nfds, int timeout) {
    // 只处理需要阻塞的情况，把这些poll请求放到全局的Epoll对象上
    if (timeout <= 0) {
        return 0;
    }
    int epfd = glbEnv.pEpollIns->epollFd;
    co_timed_poll pollReq; // 总管这些poll请求
    co_init_poll(&pollReq, epfd, fds, nfds, timeout);
    co_poll_subreq *subreqs = (co_poll_subreq*)malloc(sizeof(co_poll_subreq) * nfds);
    for (int i = 0; i < nfds; ++i) {
        pollfd &pf = fds[i];
        subreqs[i].idxInEvents = i;
        subreqs[i].pTimedPoll = &pollReq;
        epoll_event transEv;
        transEv.events = PollEvent2Epoll(pf.events);
        transEv.data.ptr = subreqs + i; // 放subreq
        epoll_ctl(epfd, EPOLL_CTL_ADD, pf.fd, &transEv);
    }
    AddToTimingWheel(glbEnv.pTimingWheel, &pollReq, GetTimeStamp() + timeout);
    co_yield(); // 让epoll等待好了在转回来

    // 从epoll中删除这些fd，（结果在哪回填？）
    for (int i = 0; i < nfds; ++i) {
        pollfd &pf = fds[i];
        int flag = epoll_ctl(epfd, EPOLL_CTL_DEL, pf.fd, NULL);
        if (flag == -1) {
            printf("Error epoll_ctl, errno = %s\n", strerror(errno));
        }
    }
    free(subreqs);
    return pollReq.nPrepared;
}

void co_eventloop() {
    // 事件循环
    while (true) {
        // 1. 执行定时器

        std::list<co_slot_item> mayExpired;
        // 为什么说这个是mayExpired？因为时间轮算法中，如果等待时间是一个时间轮的周期，那么就会被放到下一个时间轮中，所以这里可能会有一些误差
        // checkTimingWheel(glbEnv.pTimingWheel);
        // poll(NULL, 0, 3000); // 3s
        auto ep = glbEnv.pEpollIns;
        int nTrig = epoll_wait(glbEnv.pEpollIns->epollFd, glbEnv.pEpollIns->events, EPOLL_MAX_LENGTH, 1);
        if (nTrig > 0) {
            auto evs = glbEnv.pEpollIns->events;
            for (int i = 0; i < nTrig; ++i) {
                co_poll_subreq *req = (co_poll_subreq *)evs[i].data.ptr;
                co_timed_poll *mainReq = req->pTimedPoll;
                // 规定epoll_event结果中的data指针指向一个sub poll request object, ctl注册时记得传参
                if (mainReq->eventTriggerCallback) {
                    // const epoll_event &ev, co_poll_subreq *subreq, std::list<co_slot_item>& mayTo
                    EventTriggerArgs args {
                            evs[i],
                            req,
                            &mayExpired
                    };
                    mainReq->eventTriggerCallback(&args);
                } else {
                    // 如果没有event trigger callback，那么就直接把结果放到mayExpired中
                    mayExpired.push_back(*mainReq);
                }
            }
        } else if (nTrig < 0) {
            printf("Error epoll wait with %d, errno = %s\n", nTrig, strerror(errno));
            exit(-1);
        }

        ull now = GetTimeStamp();
        AdvanceToClearMayBeExpiredSlots(glbEnv.pTimingWheel, &mayExpired, now);

#ifdef DEBUG
        checkTimingWheel(glbEnv.pTimingWheel);
        if (mayExpired.size() == 0) {
            // 没有超时的，那么就等待一下
            printf("\nwait\n");
            poll(NULL, 0, 3000);
            continue;
        }
#endif
        for (auto &item : mayExpired) {
            // 由于有sub event，需要特殊处理一下

        }

        for (auto &item : mayExpired) {
            if (item.expireTimeStamp <= now) {
                // 超时之后执行一个函数,这个函数是写死还是灵活?
                item.expireCallback((void*)&item); // 超时了应该返回原来的那个协程？
            } else {
                // 这个没有真的过期，重新插入
                AddToTimingWheel(glbEnv.pTimingWheel, &item, now);
            }
        }
        // 2. 执行事件,这需要用Epoll高效的等待多种阻塞事件
        //    这部分逻辑：如果要执行一个阻塞的函数（like Poll），我们重新定义这个函数
        //                   用我们得全局Epoll管理操作涉及的文件描述符（linux很多东西都能包装成fd，定时器啥的）让Epoll去等待
        //               处理两种返回情况？1. 超时，添加到时间轮里了，它会执行回调函数
        //                          2. 有事件发生（这种情况有点复杂，一次Poll会监听多个fd，这么多个只要有一个准备好了，就去删除1的超时回调

        // 3. 执行协程
    }
}