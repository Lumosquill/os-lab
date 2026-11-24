#include <defs.h>
#include <assert.h>
#include <error.h>
#include <proc.h>
#include <trap.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <riscv.h>

extern void exception_handler(struct trapframe *tf);

static void lab5_pass(const char *name)
{
    cprintf("[Test %s] PASS\n", name);
}

void test_lab5_process_create(void)
{
    assert(current == idleproc);
    assert(idleproc != NULL && idleproc->pid == 0);
    assert(idleproc->state == PROC_RUNNABLE);
    assert(idleproc->mm == NULL && idleproc->tf == NULL);
    assert(initproc != NULL && initproc->pid == 1);
    assert(initproc->parent == idleproc);
    assert(initproc->state == PROC_RUNNABLE);
    assert(initproc->context.ra != 0 && initproc->context.sp != 0);
    lab5_pass("Process Create");
}

void test_lab5_syscall(void)
{
    struct trapframe tf;
    struct trapframe *old = current->tf;
    memset(&tf, 0, sizeof(tf));
    tf.cause = CAUSE_USER_ECALL;
    tf.epc = 0x1000;
    tf.gpr.a0 = SYS_getpid;
    current->tf = &tf;
    exception_handler(&tf);
    assert(tf.epc == 0x1004);
    assert((int)tf.gpr.a0 == current->pid);
    tf.gpr.a0 = 0;
    exception_handler(&tf);
    assert((int)tf.gpr.a0 == -E_INVAL);
    current->tf = old;
    lab5_pass("Syscall");
}

void test_lab5_process_control(void)
{
    current->need_resched = 0;
    assert(do_yield() == 0 && current->need_resched);
    current->need_resched = 0;

    assert(do_kill(-1) == -E_INVAL);
    assert(do_wait(-1, NULL) == -E_BAD_PROC);
    lab5_pass("Process Control");
}