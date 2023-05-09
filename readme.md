# LICO

## 参考libco中协程的实现分析

操作系统中的进程由 PCB，堆栈，代码区 构成，协程就是模拟这些部分（但是没有实现调度）
其中难点是那段x_86_64汇编代码，直接copy过来了，注释好了

协程的调用也是和函数调用类似，只不过是通过co_resume调用，并且被调用函数从上次co_yield()的位置继续执行

## hook read, recv, write, poll等系统调用

上面的部分仅能够实现懒计算等特性，libco在一些可能会阻塞的系统调用时自动yield让出cpu，这部分实现原理是什么？
libco通过epoll和时间轮管理事件，

## Test

g++ -g co_nest_test.cpp coctx_swap.S && ./a.out