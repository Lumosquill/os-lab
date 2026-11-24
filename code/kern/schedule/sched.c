#include <list.h>
#include <sync.h>
#include <proc.h>
#include <sched.h>
#include <assert.h>

void wakeup_proc(struct proc_struct *proc)
{
    assert(proc->state != PROC_ZOMBIE);
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        if (proc->state != PROC_RUNNABLE)
        {
            proc->state = PROC_RUNNABLE;
            proc->wait_state = 0;
        }
        else
        {
            warn("wakeup runnable process.\n");
        }
    }
    local_intr_restore(intr_flag);
}

//LAB4 function3 2410699
// 成员C：schedule

void schedule(void)
{
    bool intr_flag;
    list_entry_t *le, *last;
    struct proc_struct *next = NULL;

    // 关中断，保护 current / proc_list / runs 的修改
    local_intr_save(intr_flag);
    {
        // 本次调度需求已被处理；必须在临界区内清零
        current->need_resched = 0;

        // current 是 idleproc 时从链表头开始；否则从 current 的下一个开始，实现轮转
        last = (current == idleproc) ? &proc_list : &(current->list_link);
        le = last;
        do
        {
            // 跳过链表头哨兵 &proc_list，只检查真正的 PCB 节点
            if ((le = list_next(le)) != &proc_list)
            {
                next = le2proc(le, list_link);
                // 只选 RUNNABLE；SLEEPING/ZOMBIE/UNINIT 都跳过
                if (next->state == PROC_RUNNABLE)
                {
                    break;
                }
            }
        } while (le != last);

        // 找不到就绪线程（或绕回 current 但它已不是 RUNNABLE）时，退回 idleproc 保底
        if (next == NULL || next->state != PROC_RUNNABLE)
        {
            next = idleproc;
        }

        // 统计被调度次数；选中即计数
        next->runs++;

        // 只有真正换了线程才切页表/上下文；next == current 直接返回
        if (next != current)
        {
            proc_run(next);
        }
    }
    // 恢复进入前的中断状态；被切走的线程以后恢复运行时才会执行到这里
    local_intr_restore(intr_flag);
}
