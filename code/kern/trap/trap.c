#include <defs.h>
#include <mmu.h>
#include <memlayout.h>
#include <clock.h>
#include <trap.h>
#include <riscv.h>
#include <stdio.h>
#include <assert.h>
#include <console.h>
#include <vmm.h>
#include <kdebug.h>
#include <unistd.h>
#include <syscall.h>
#include <error.h>
#include <sched.h>
#include <sync.h>
#include <sbi.h>

#define TICK_NUM 100

static void print_ticks()
{
    cprintf("%d ticks\n", TICK_NUM);
#ifdef DEBUG_GRADE
    cprintf("End of Test.\n");
    panic("EOT: kernel seems ok.");
#endif
}

/* idt_init - initialize IDT to each of the entry points in kern/trap/vectors.S */
void idt_init(void)
{
    extern void __alltraps(void);
    /* Set sscratch register to 0, indicating to exception vector that we are
     * presently executing in the kernel */
    write_csr(sscratch, 0);
    /* Set the exception vector address */
    write_csr(stvec, &__alltraps);
    /* Allow kernel to access user memory */
    set_csr(sstatus, SSTATUS_SUM);
}

/* trap_in_kernel - test if trap happened in kernel */
bool trap_in_kernel(struct trapframe *tf)
{
    return (tf->status & SSTATUS_SPP) != 0;
}

void print_trapframe(struct trapframe *tf)
{
    cprintf("trapframe at %p\n", tf);
    // cprintf("trapframe at 0x%x\n", tf);
    print_regs(&tf->gpr);
    cprintf("  status   0x%08x\n", tf->status);
    cprintf("  epc      0x%08x\n", tf->epc);
    cprintf("  tval 0x%08x\n", tf->tval);
    cprintf("  cause    0x%08x\n", tf->cause);
}

void print_regs(struct pushregs *gpr)
{
    cprintf("  zero     0x%08x\n", gpr->zero);
    cprintf("  ra       0x%08x\n", gpr->ra);
    cprintf("  sp       0x%08x\n", gpr->sp);
    cprintf("  gp       0x%08x\n", gpr->gp);
    cprintf("  tp       0x%08x\n", gpr->tp);
    cprintf("  t0       0x%08x\n", gpr->t0);
    cprintf("  t1       0x%08x\n", gpr->t1);
    cprintf("  t2       0x%08x\n", gpr->t2);
    cprintf("  s0       0x%08x\n", gpr->s0);
    cprintf("  s1       0x%08x\n", gpr->s1);
    cprintf("  a0       0x%08x\n", gpr->a0);
    cprintf("  a1       0x%08x\n", gpr->a1);
    cprintf("  a2       0x%08x\n", gpr->a2);
    cprintf("  a3       0x%08x\n", gpr->a3);
    cprintf("  a4       0x%08x\n", gpr->a4);
    cprintf("  a5       0x%08x\n", gpr->a5);
    cprintf("  a6       0x%08x\n", gpr->a6);
    cprintf("  a7       0x%08x\n", gpr->a7);
    cprintf("  s2       0x%08x\n", gpr->s2);
    cprintf("  s3       0x%08x\n", gpr->s3);
    cprintf("  s4       0x%08x\n", gpr->s4);
    cprintf("  s5       0x%08x\n", gpr->s5);
    cprintf("  s6       0x%08x\n", gpr->s6);
    cprintf("  s7       0x%08x\n", gpr->s7);
    cprintf("  s8       0x%08x\n", gpr->s8);
    cprintf("  s9       0x%08x\n", gpr->s9);
    cprintf("  s10      0x%08x\n", gpr->s10);
    cprintf("  s11      0x%08x\n", gpr->s11);
    cprintf("  t3       0x%08x\n", gpr->t3);
    cprintf("  t4       0x%08x\n", gpr->t4);
    cprintf("  t5       0x%08x\n", gpr->t5);
    cprintf("  t6       0x%08x\n", gpr->t6);
}

extern struct mm_struct *check_mm_struct;

//lab3 function2 2410699 & 2411570
// 返回触发异常的那条指令的字节数：低两位为 11 是 4 字节指令，否则是 2 字节压缩指令。
// 只读 16 位，避免读到指令之外的内存。
static size_t insn_length(uintptr_t addr) {
    uint16_t low = *(volatile uint16_t *)addr;
    return ((low & 0x3) == 0x3) ? 4 : 2;
}

// 成员C 实现：中断处理
void interrupt_handler(struct trapframe *tf) {
    // scause 的最高位为 1 表示中断，低 63 位才是原因编码。
    // 必须先剥掉最高位，否则真实的中断值 0x8000000000000005 无法与 IRQ_S_TIMER 匹配。
    intptr_t cause = (intptr_t)((tf->cause << 1) >> 1);

    switch (cause) {
        case IRQ_S_TIMER:
            // OpenSBI 一次只能设定一个时钟事件，必须每拍重新续期，否则只会收到一次中断
            clock_set_next_event();
            ticks++;
            // lab5：lab3 里这里是打印 "100 ticks" 并在若干次后关机。
            // lab5 的评分构建带 -DDEBUG_GRADE，print_ticks 会直接 panic，而且进程需要被时钟抢占，
            // 所以改为每 TICK_NUM 个 tick 置位当前进程的 need_resched，由 trap() 返回前的 schedule() 处理。
            if (ticks % TICK_NUM == 0) {
                assert(current);
                current->need_resched = 1;
            }
            break;
        case IRQ_S_SOFT:
            cprintf("Supervisor software interrupt\n");
            break;
        default:
            // 未知中断只做提示；print_trapframe 是异常的兜底手段，不能在这里用
            cprintf("Unknown interrupt: cause %d\n", (int)cause);
            break;
    }
}

//lab5 function1 2410699 exception_handler
void exception_handler(struct trapframe *tf) {
    switch (tf->cause) 
    {
        case CAUSE_ILLEGAL_INSTRUCTION:
            cprintf("Illegal instruction at 0x%08x\n", tf->epc);
            if (trap_in_kernel(tf)) 
            {
                // 内核态：越过出错指令
                tf->epc += insn_length(tf->epc);     
            } 
            else 
            {
                // 用户态跑飞：杀进程
                do_exit(-E_KILLED);                  
            }
            break;
        case CAUSE_BREAKPOINT:
            cprintf("Breakpoint at 0x%08x\n", tf->epc);
            // gdb 在用户程序里下断点靠 ebreak，用户态一杀进程，分支任务的 gdb 调试直接失效
            tf->epc += insn_length(tf->epc);
            break;
        case CAUSE_USER_ECALL:
            // ecall 固定 4 字节；断言 0x1000→0x1004
            tf->epc += 4;                            
            syscall();
            break;
        case CAUSE_MISALIGNED_FETCH:
        case CAUSE_FETCH_ACCESS:
        case CAUSE_MISALIGNED_LOAD:
        case CAUSE_LOAD_ACCESS:
        case CAUSE_MISALIGNED_STORE:
        case CAUSE_STORE_ACCESS:
        case CAUSE_FETCH_PAGE_FAULT:
        case CAUSE_LOAD_PAGE_FAULT:
        case CAUSE_STORE_PAGE_FAULT:
            // [C 的 COW 接入点] 只对 STORE_PAGE_FAULT 有意义，C 在这里加 1-2 行：
            if (trap_in_kernel(tf)) 
            {
                print_trapframe(tf);
                panic("unexpected trap in kernel.\n");
            }
            print_trapframe(tf);
            // 用户态缺页：终止，不能返回
            do_exit(-E_KILLED);                      
            break;
        default:
            if (trap_in_kernel(tf)) 
            {
                // 保持 lab4 行为：只打印
                print_trapframe(tf);
                break;                               
            }
            print_trapframe(tf);
            do_exit(-E_KILLED);
            break;
    }
}


static inline void trap_dispatch(struct trapframe *tf)
{
    if ((intptr_t)tf->cause < 0)
    {
        // interrupts
        interrupt_handler(tf);
    }
    else
    {
        // exceptions
        exception_handler(tf);
    }
}

/* *
 * trap - handles or dispatches an exception/interrupt. if and when trap() returns,
 * the code in kern/trap/trapentry.S restores the old CPU state saved in the
 * trapframe and then uses the iret instruction to return from the exception.
 * */
void trap(struct trapframe *tf)
{
    // dispatch based on what type of trap occurred
    //    cputs("some trap");
    if (current == NULL)
    {
        trap_dispatch(tf);
    }
    else
    {
        struct trapframe *otf = current->tf;
        current->tf = tf;

        bool in_kernel = trap_in_kernel(tf);

        trap_dispatch(tf);

        current->tf = otf;
        if (!in_kernel)
        {
            if (current->flags & PF_EXITING)
            {
                do_exit(-E_KILLED);
            }
            if (current->need_resched)
            {
                schedule();
            }
        }
    }
}
