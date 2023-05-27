#include"co_struct.h"

// 1. 时间轮，处理定时任务
// 拆分任务，时间轮需要处理的任务有：
co_struct* GetCurrentCo();
void OnSubPollResponse(void *vArgs);
void TransferTo(void *co_item);

ull GetTimeStamp() {
    struct timeval now = {0};
    gettimeofday(&now, nullptr);
    return now.tv_sec * 1000 + now.tv_usec / 1000;
}


ull GetExpiredSlotId() {
    static ull id = 0;
    return ++id;
}

struct TimeWheelSlot {
    // 作为一个定时器，需要记录超时时间，以及超时后的回调函数？（sleep也很常用，用poll实现）
    ull expireTimeStamp;
    co_func expireCallback; // 调用上面的switchto 直接Switch to原来的coroutine
    co_func eventTriggerCallback;
    co_struct *co;
    int slotIndex; // 用于删除指定的slot
    ull id;
};
typedef std::vector<std::list<TimeWheelSlot>> Slots;

struct TimeWheel {
    int slotNum;
    int curSlot;      // 当前slot
    ull curTimeStamp; // 当前时间戳
    std::unique_ptr<Slots> pSlots;
};

// START TimeWheel相关的辅助函数
void AdvanceToClearMayBeExpiredSlots(TimeWheel *pTimingWheel, std::list<TimeWheelSlot> *pExpiredSlots, ull now) {
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
        std::list<TimeWheelSlot> &slot = (*pTimingWheel->pSlots)[curSlot];
        if (!slot.empty())
            pExpiredSlots->splice(pExpiredSlots->end(), slot); // list is slow, but it is simple
    }
    pTimingWheel->curSlot = curSlot;
    pTimingWheel->curTimeStamp = now;

}

bool AddToTimingWheel(TimeWheel *pTimingWheel, TimeWheelSlot *pSlotItem, ull now) {
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
    pSlotItem->slotIndex = slotIndex;
    return true;
}

void RemoveFromTimingWheel(TimeWheel *pTimingWheel, TimeWheelSlot *pSlotItem) {
    auto &targetList = pTimingWheel->pSlots->at(pSlotItem->slotIndex);
    auto iter = targetList.begin();
    while (iter != targetList.end()) {
        if (iter->id == pSlotItem->id) {
            targetList.erase(iter);
            break;
        }
        ++iter;
    }
}

void checkTimingWheel(TimeWheel *pTimingWheel) {
    printf("curSlot %d\n", pTimingWheel->curSlot);
    for (int i = 0; i < pTimingWheel->slotNum; ++i) {
        if (pTimingWheel->pSlots->at(i).size() > 0) {
            printf("slot %d has %lu items\n", i, pTimingWheel->pSlots->at(i).size());
        }
    }
}
// END TimeWheel相关的辅助函数


// 2. Epoll托管子poll请求
#define EPOLL_MAX_LENGTH 2048
struct EpollRequest {
    int epollFd;
    epoll_event *events;
};


struct PollRequest : public TimeWheelSlot {
    int envEpollFd;
    int nfds;
    pollfd *events;
    int nPrepared;     // 0
    bool hasSubReqTrg;
};

struct PollSubRequest {
    PollRequest *pTimedPoll; // the main request
    int idxInEvents;
};

void InitTWSlot(TimeWheelSlot *item, ull expiredTime) {
    item->co = GetCurrentCo();
    item->expireTimeStamp = expiredTime;
    item->expireCallback = TransferTo;
    item->id = GetExpiredSlotId();
    item->slotIndex = -1;
}

void InitPollRequestIn(PollRequest *cop, int epfd, pollfd *fds, int nfds, int timeout) {
    InitTWSlot(cop, GetTimeStamp() + timeout);
    cop->envEpollFd = epfd;
    cop->events = fds;
    cop->nfds = nfds;
    cop->nPrepared = 0;
    cop->hasSubReqTrg = false;
    cop->eventTriggerCallback = OnSubPollResponse;
}

struct CoEnvir
{ // 协程执行的环境，包括epoll，当前协程，上次挂起的协程，协程的调度栈 (把协程想象成一个特殊的函数,这个函数被call的时候可以从中间执行)
	co_struct *pCallStack[ 128 ]; // 这个callstack属实有点小了
	int iCallStackSize;
    TimeWheel *pTimingWheel;
    EpollRequest *pEpollIns;
};
static CoEnvir GlobalEnv;

// 3. Condition Variable,用于同步多个协程，实现wait wait_for notify_one notify_all

int co_condition_variable::wait(std::function<bool()> predicate) {
    // wait 判断条件是否满足，不满足时候，yield给其他协程，等待notify
    while (!predicate()) {
        _waitingCoList.push_back(GetCurrentCo());
        co_yield();
    }
    return 0; // 0正常 -1超时
}
int co_condition_variable::wait_for(std::function<bool()> predicate, ull ms) {
    ull now = GetTimeStamp();
    TimeWheelSlot tws;
    InitTWSlot(&tws, now + ms);
    AddToTimingWheel(GlobalEnv.pTimingWheel, &tws, now);
    while (!predicate()) {
        _waitingCoList.push_back(GetCurrentCo());
        co_yield();
        if (GetTimeStamp() >= tws.expireTimeStamp) {
            break;
        }
    }
    RemoveFromTimingWheel(GlobalEnv.pTimingWheel, &tws);
    return predicate() ? 0 : -1;
}
void co_condition_variable::notify_one() {
    if (!_waitingCoList.empty()) {
        co_struct *co = _waitingCoList.front();
        _waitingCoList.pop_front();
        co_resume(co);
    }
}
void co_condition_variable::notify_all() {
    // 这有坑的需要遍历list，但是遍历过程中转让了控制权，期间list会被修改的。直接转移
    std::list<co_struct*> copy;
    copy.swap(_waitingCoList);
    for (auto co : copy) {
        co_resume(co);
    }
}

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

//-------------
// init data structure
void co_create(co_struct **pico, co_func func, co_args args) {
    co_struct *co = (co_struct*)calloc(1, sizeof(co_struct));
    co->func = func;
    co->args = args;

    co->env = &GlobalEnv;
    co->ctx = (coctx_t*)calloc(1, sizeof(coctx_t));
    co->stack = (co_stack*)calloc(1, sizeof(co_stack));

    *pico = co;
}

void co_release(co_struct *co) {
    free(co->ctx);
    free(co->stack);
    free(co);
}

void InitTimeWheel(TimeWheel **tw) {
    TimeWheel *t = new TimeWheel;
    t->slotNum = MAX_SLOT_NUM_MS;
    t->curSlot = 0;
    t->curTimeStamp = GetTimeStamp();
    t->pSlots.reset(new Slots(t->slotNum));
    *tw = t;
}
void ReleaseTimeWheel(TimeWheel *tw) {
    delete tw;
}

void co_global_init() {
    memset(&GlobalEnv, 0, sizeof(GlobalEnv));
    GlobalEnv.iCallStackSize = 1; // 默认有个全局env 也就是main
    co_create(&GlobalEnv.pCallStack[0], NULL, NULL);
    InitTimeWheel(&GlobalEnv.pTimingWheel);
    GlobalEnv.pEpollIns = (EpollRequest*)calloc(1, sizeof(EpollRequest));
    GlobalEnv.pEpollIns->epollFd = epoll_create(1);
    if (GlobalEnv.pEpollIns->epollFd < 0) {
        perror("epoll_create"); exit(1);
    }
    GlobalEnv.pEpollIns->events = (epoll_event*)malloc(sizeof(epoll_event) * EPOLL_MAX_LENGTH);
    if (GlobalEnv.pEpollIns->events == NULL) {
        perror("malloc fail"); exit(1);
    }
}

void co_global_release() {
    for (int i = 0; i < GlobalEnv.iCallStackSize; ++i) {
        co_release(GlobalEnv.pCallStack[i]);
    }
    ReleaseTimeWheel(GlobalEnv.pTimingWheel);
    close(GlobalEnv.pEpollIns->epollFd);
    free(GlobalEnv.pEpollIns->events);
    free(GlobalEnv.pEpollIns);
}

void CoSwap(co_struct *curr, co_struct *next) {
    // 一个神奇的函数，直接就把当前的上下文切换到next的上下文
    coctx_swap(curr->ctx, next->ctx); // 此时函数已经到next中继续运行了，如何回到这个地址？
}

void CoFinish() { // 如果协程函数执行完毕，ret会返回到这个函数的地址上，从而swap到协程的父协程
    co_struct *curr = GlobalEnv.pCallStack[GlobalEnv.iCallStackSize-1];
    co_struct *next = GlobalEnv.pCallStack[GlobalEnv.iCallStackSize-2];
    // maybe need some garbage collection? maybe not
    GlobalEnv.iCallStackSize--;
    curr->isEnded = true;
    CoSwap(curr, next);
}

void co_resume(co_struct *co) {
    if (!co->isStarted) {
        // 协程初始化时，需要设置 栈，寄存器，返回地址
        co->ctx->regs[kRETAddr] = (char*)co->func;
        co->ctx->regs[kRDI] = (char*)co->args;
        // RBP RSP应该设置为正确的堆栈地址,RBP前面应该留一个返回地址的位置，函数执行完之后返回到这个函数里，没有显示yield
        char *stBase = (char*)( (u_int64_t)(&co->stack->stack_buffer[CO_STACK_SIZE-1]) & (-16LL) );
        *((u_int64_t*)stBase + 1) = (u_int64_t)CoFinish;
        co->ctx->regs[kRBP] = stBase;
        co->ctx->regs[kRSP] = stBase; // 这个函数的返回地址已经被安排了
        co->isStarted = true;
    }
    if (co->isEnded) {
        printf("Error co is ended\n");
        exit(-1);
    }
    co_struct *curr = GlobalEnv.pCallStack[GlobalEnv.iCallStackSize - 1];
    GlobalEnv.pCallStack[GlobalEnv.iCallStackSize++] = co;
    CoSwap( curr,co );
}

void co_yield() {
    // 直接yield到env，让到栈上的下一个协程
    co_struct *curr = GlobalEnv.pCallStack[GlobalEnv.iCallStackSize - 1];
    co_struct *next = GlobalEnv.pCallStack[GlobalEnv.iCallStackSize - 2];
    GlobalEnv.iCallStackSize--;
    CoSwap( curr,next );
}

// call back function
void TransferTo(void *co_item) {
    TimeWheelSlot *item = (TimeWheelSlot*)co_item;
    co_resume(item->co);
}

// START 事件，超时。
struct EventTriggerArgs {
    epoll_event ev;
    PollSubRequest *subreq;
    std::list<TimeWheelSlot> *mayTimeOut;
};

void OnSubPollResponse(void *vArgs) {
    // 当Poll请求中的部分满足条件了
    EventTriggerArgs *args = (EventTriggerArgs *)vArgs;
    PollRequest *pMainPool = args->subreq->pTimedPoll;
    // pollfd相应位置设置为ev的值
    pMainPool->nPrepared ++;
    pMainPool->events[args->subreq->idxInEvents].revents = EpollEvent2Poll(args->ev.events);

    if (!pMainPool->hasSubReqTrg) {
        pMainPool->hasSubReqTrg = true;
        (*args->mayTimeOut).push_back(*pMainPool);
        RemoveFromTimingWheel(GlobalEnv.pTimingWheel, pMainPool);
    }
}

co_struct* GetCurrentCo() {
    return GlobalEnv.pCallStack[GlobalEnv.iCallStackSize - 1];
}

void co_sleep(ull ms) {
    // 往时间轮中添加一个定时器，把当前协程挂起(struct最好用仅包含指针的struct，这样拷贝起来也轻量)
    // 如果是main协程，那么就直接sleep，不需要挂起
    TimeWheelSlot item;
    auto now = GetTimeStamp();
    item.co = GetCurrentCo();
    item.expireCallback = TransferTo;
    item.expireTimeStamp = now + ms;
    if(!AddToTimingWheel(GlobalEnv.pTimingWheel, &item, now)) { // 期望它timeout的时候返回
        printf("Error add to timing wheel\n");
        return;
    } else {
        // maybe log is needed
    }
    co_yield();
}

int co_poll(struct pollfd fds[], nfds_t nfds, int timeout) {
    // 只处理需要阻塞的情况，把这些poll请求放到全局的Epoll对象上
    if (timeout <= 0) {
        return 0;
    }
    // 实际上是吧poll请求用epoll实现了一遍 手动清除revents调用前
    int epfd = GlobalEnv.pEpollIns->epollFd;
    PollRequest pollReq; // 总管这些poll请求
    InitPollRequestIn(&pollReq, epfd, fds, nfds, timeout);
    PollSubRequest *subreqs = (PollSubRequest*)malloc(sizeof(PollSubRequest) * nfds);
    for (int i = 0; i < nfds; ++i) {
        fds[i].revents = 0;
        subreqs[i].idxInEvents = i;
        subreqs[i].pTimedPoll = &pollReq;
        epoll_event transEv;
        pollfd &pf = fds[i];
        transEv.events = PollEvent2Epoll(pf.events);
        transEv.data.ptr = subreqs + i; // 放subreq
        epoll_ctl(epfd, EPOLL_CTL_ADD, pf.fd, &transEv);
    }
    AddToTimingWheel(GlobalEnv.pTimingWheel, &pollReq, GetTimeStamp());
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
        std::list<TimeWheelSlot> mayExpired;
        // 为什么说这个是mayExpired？因为时间轮算法中，如果等待时间是一个时间轮的周期，那么就会被放到下一个时间轮中，所以这里可能会有一些误差
        // checkTimingWheel(GlobalEnv.pTimingWheel);
        // poll(NULL, 0, 3000); // 3s
        auto ep = GlobalEnv.pEpollIns;
        int nTrig = epoll_wait(GlobalEnv.pEpollIns->epollFd, GlobalEnv.pEpollIns->events, EPOLL_MAX_LENGTH, 1);
        if (nTrig > 0) {
            epoll_event *epoll_events = GlobalEnv.pEpollIns->events;
            for (int i = 0; i < nTrig; ++i) {
                PollSubRequest *subreq = (PollSubRequest *)epoll_events[i].data.ptr;
                PollRequest *mainReq = subreq->pTimedPoll;
                // 规定epoll_event结果中的data指针指向一个sub poll request object, ctl注册时记得传参
                if (mainReq->eventTriggerCallback) {
                    // const epoll_event &ev, PollSubRequest *subreq, std::list<TimeWheelSlot>& mayTo
                    EventTriggerArgs args {
                            epoll_events[i],
                            subreq,
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
        // 找出当前超时的东西
        ull now = GetTimeStamp();
        AdvanceToClearMayBeExpiredSlots(GlobalEnv.pTimingWheel, &mayExpired, now);

#ifdef DEBUG
        checkTimingWheel(GlobalEnv.pTimingWheel);
        if (mayExpired.size() == 0) {
            // 没有超时的，那么就等待一下
            printf("\nwait\n");
            poll(NULL, 0, 500);
            continue;
        }
#endif

        for (auto &item : mayExpired) {
            if (item.expireTimeStamp <= now || item.eventTriggerCallback != NULL) {
                // 超时之后执行一个函数,这个函数是写死还是灵活?
                item.expireCallback((void*)&item); // 超时了应该返回原来的那个协程？
            } else {
                // 这个没有真的过期，重新插入
                AddToTimingWheel(GlobalEnv.pTimingWheel, &item, now);
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