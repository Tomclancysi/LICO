/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing,
* software distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

.globl coctx_swap
#if !defined( __APPLE__ )
.type  coctx_swap, @function
#endif
coctx_swap:

#if defined(__i386__)
    movl 4(%esp), %eax
    movl %esp,  28(%eax)
    movl %ebp, 24(%eax)
    movl %esi, 20(%eax)
    movl %edi, 16(%eax)
    movl %edx, 12(%eax)
    movl %ecx, 8(%eax)
    movl %ebx, 4(%eax)


    movl 8(%esp), %eax
    movl 4(%eax), %ebx
    movl 8(%eax), %ecx
    movl 12(%eax), %edx
    movl 16(%eax), %edi
    movl 20(%eax), %esi
    movl 24(%eax), %ebp
    movl 28(%eax), %esp

	ret

#elif defined(__x86_64__)
	leaq (%rsp),%rax       ;rax保存rsp指向的地址
    movq %rax, 104(%rdi) ;rsp保存在reg[13],<前6个参数在rdi,rsi,rdx,rcx,r8,r9
    movq %rbx, 96(%rdi)  ;依次保存后面的
    movq %rcx, 88(%rdi)  ;
    movq %rdx, 80(%rdi)  ;
	  movq 0(%rax), %rax   ;将rsp栈顶指向的数据保存到rax中,栈顶保存了什么?ret返回位置!
	  movq %rax, 72(%rdi)  ;  然后把这个返回位置,保存到reg[9]中
    movq %rsi, 64(%rdi)  ;第二个操作数
	  movq %rdi, 56(%rdi)
    movq %rbp, 48(%rdi)  ;保存栈基地址寄存器
    movq %r8, 40(%rdi)
    movq %r9, 32(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 16(%rdi)
    movq %r14, 8(%rdi)
    movq %r15, (%rdi)
	  xorq %rax, %rax      ;rax置为空

    movq 48(%rsi), %rbp  ;rsi指向第二个操作数,也就是next协程,切换栈基地址reg
    movq 104(%rsi), %rsp ;栈顶指针恢复
    movq (%rsi), %r15    ;恢复其他寄存器,没啥特殊的
    movq 8(%rsi), %r14
    movq 16(%rsi), %r13
    movq 24(%rsi), %r12
    movq 32(%rsi), %r9
    movq 40(%rsi), %r8
    movq 56(%rsi), %rdi
    movq 80(%rsi), %rdx
    movq 88(%rsi), %rcx
    movq 96(%rsi), %rbx
		leaq 8(%rsp), %rsp   ;注意rsp前面还有个返回地址没有pop掉,现在+8就是pop这个
		pushq 72(%rsi)       ;返回地址替换成另一个地址,从而实现IP跳转

    movq 64(%rsi), %rsi
	ret
#endif
