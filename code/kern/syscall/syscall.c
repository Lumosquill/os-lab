#include <unistd.h>
#include <proc.h>
#include <syscall.h>
#include <trap.h>
#include <stdio.h>
#include <pmm.h>
#include <assert.h>
#include <error.h>

// lab5 2410699
static int sys_exit(uint64_t arg[]);
static int sys_fork(uint64_t arg[]);
static int sys_wait(uint64_t arg[]);
static int sys_exec(uint64_t arg[]);
static int sys_yield(uint64_t arg[]);
static int sys_kill(uint64_t arg[]);
static int sys_getpid(uint64_t arg[]);
static int sys_putc(uint64_t arg[]);
static int sys_pgdir(uint64_t arg[]);
//

static int (*syscalls[])(uint64_t arg[]) = {
    [SYS_exit]              sys_exit,
    [SYS_fork]              sys_fork,
    [SYS_wait]              sys_wait,
    [SYS_exec]              sys_exec,
    [SYS_yield]             sys_yield,
    [SYS_kill]              sys_kill,
    [SYS_getpid]            sys_getpid,
    [SYS_putc]              sys_putc,
    [SYS_pgdir]             sys_pgdir,
};

#define NUM_SYSCALLS        ((sizeof(syscalls)) / (sizeof(syscalls[0])))

//lab5 function1 2410699
static int sys_exit(uint64_t arg[]) {
    return do_exit((int)arg[0]);
}

static int sys_fork(uint64_t arg[]) {
    // 本次 ecall 的 trapframe
    struct trapframe *tf = current->tf;
    // 子进程用父进程的用户栈
    return do_fork(0, (uintptr_t)tf->gpr.sp, tf); 
}

static int sys_wait(uint64_t arg[]) {
    return do_wait((int)arg[0], (int *)arg[1]);
}

static int sys_exec(uint64_t arg[]) {
    return do_execve((const char *)arg[0], (size_t)arg[1],
                     (unsigned char *)arg[2], (size_t)arg[3]);
}

static int sys_yield(uint64_t arg[]) {
    return do_yield();
}

static int sys_kill(uint64_t arg[]) {
    return do_kill((int)arg[0]);
}

static int sys_getpid(uint64_t arg[]) {
    return current->pid;
}

static int sys_putc(uint64_t arg[]) {
    cputchar((int)arg[0]);
    return 0;
}

static int sys_pgdir(uint64_t arg[]) {
    print_pgdir();
    return 0;
}

void syscall(void) {
    struct trapframe *tf = current->tf;
    // 本实验：a0 = 调用号
    uint64_t num = tf->gpr.a0;
    if (num < NUM_SYSCALLS && syscalls[num] != NULL) 
    {
        // pushregs 里 a1..a7 连续，&a1 就是参数数组首地址（a1..a5 共 5 个）
        tf->gpr.a0 = (uintptr_t)syscalls[num]((uint64_t *)&tf->gpr.a1);
    } 
    else 
    {
        // 测试断言：非法号返回 -E_INVAL
        tf->gpr.a0 = (uintptr_t)-E_INVAL;        
    }
}