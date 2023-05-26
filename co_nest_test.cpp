#include "co_struct.h"

#define MAX_NEST_LEVEL 127 // 128个协程，最后一个是main

void nest_func(co_args level) {
    int le = *(int*)level;
    if (le >= MAX_NEST_LEVEL) {
        printf("Max Nest Level Achieved\n");
        return ;
    }
    printf("level %d\n", le);

    struct co_struct *co = NULL;
    int nxt_level = le + 1;
    co_create(&co, nest_func, co_args(&nxt_level));
    co_resume(co);

    printf("level %d\n", le);
}


int main() {
    // 测试一下协程的嵌套层数
    co_global_init();

    int level = 0;
    nest_func(&level);

    co_global_release();
}