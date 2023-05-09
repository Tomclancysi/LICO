#include "co_struct.h"

/**
 * 可能的输出： func1每1秒输出1次，func2每2秒输出1次
 *
    XXXXXfunc1 time 504XXXXX
    ++++++++++++func2 time 504+++++++++++
    XXXXXfunc1 time 505XXXXX
    ++++++++++++func2 time 506+++++++++++
    XXXXXfunc1 time 506XXXXX
    XXXXXfunc1 time 507XXXXX
    ++++++++++++func2 time 508+++++++++++
    XXXXXfunc1 time 508XXXXX
    XXXXXfunc1 time 509XXXXX
    ++++++++++++func2 time 510+++++++++++
    XXXXXfunc1 time 510XXXXX
    XXXXXfunc1 time 511XXXXX
*/

void func1(void *) {
    while(true) {
        int second = (GetTimeStamp() / 1000) % 1000;
        printf("XXXXXfunc1 time %dXXXXX\n", second);
        co_sleep(1000);
    }
}

void func2(void *) {
    while(true) {
        int second = (GetTimeStamp() / 1000) % 1000;
        printf("++++++++++++func2 time %d+++++++++++\n", second);
        co_sleep(2000);
    }
}

int main() {
    // 测试一下协程的嵌套层数
    co_global_init();

    struct co_struct *co1 = NULL;
    co_struct_init(&co1, func1, NULL);
    struct co_struct *co2 = NULL;
    co_struct_init(&co2, func2, NULL);
    co_resume(co1);
    co_resume(co2);

    co_eventloop();

    co_global_release();
}