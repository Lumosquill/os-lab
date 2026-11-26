# Lab 5 提示词（prompt.md）

## 2410699 张欣妍（成员 A）

### 1. 功能模块一主提示词（syscall.c + trap.c + pmm.c）

````markdown
[PROMPT]
任务：在 lab5 骨架中实现“用户态系统调用”功能模块，只改三处，必须修改真实文件：
(1) kern/syscall/syscall.c 的 “//lab5 function1 YOUR CODE” 处：实现 void syscall(void) 与 9 个 static sys_*（sys_exit/sys_fork/sys_wait/sys_exec/sys_yield/sys_kill/sys_getpid/sys_putc/sys_pgdir）；
(2) kern/trap/trap.c 的 “//lab5 function1 ... update exception_handler” 处：在 exception_handler 中增加 CAUSE_USER_ECALL 与用户态异常处理；
(3) kern/mm/pmm.c 文件末尾（get_pgtable_items 之后）新增 void print_pgdir(void)，并把注释里的 “YOUR CODE” 替换为 “2410699”。
操作要求：保留 syscalls[] 分发表、NUM_SYSCALLS、所有既有函数与注释；由于分发表在文件顶部、sys_* 定义在表后，必须在表前补 9 个 static 函数原型（否则 'sys_exit' undeclared）；文件是 CRLF 换行，保持原风格；不要修改 proc.c/vmm.c/sched.c/switch.S 等其他成员负责的文件与 “//lab5 function2/3/4” 标记处；不要删除既有逻辑；以工程真实代码为准，不引入无关功能。

[RELY]
1) 系统调用号（libs/unistd.h）：SYS_exit=1, SYS_fork=2, SYS_wait=3, SYS_exec=4, SYS_yield=10, SYS_kill=12, SYS_getpid=18, SYS_putc=30, SYS_pgdir=31。
2) 用户态封装（user/libs/syscall.c）：a0=调用号，a1~a5=参数，ecall 后从 a0 读返回值；用户库未提供 sys_exec 封装。
3) struct pushregs（kern/trap/trap.h）字段顺序：zero,ra,sp,gp,tp,t0,t1,t2,s0,s1,a0,a1,a2,a3,a4,a5,a6,a7,s2..s11,t3..t6；gpr.a1..a7 连续。
4) extern struct proc_struct *current;（proc.h）current->tf 指向本次陷入的 trapframe，current->pid 为进程号。
5) 模块二/四提供（签名以 proc.h 为准）：
   int do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf);
   int do_wait(int pid, int *code_store);   int do_execve(const char *name, size_t len, unsigned char *binary, size_t size);
   int do_exit(int error_code); int do_yield(void); int do_kill(int pid);
6) void cputchar(int c)（libs/stdio.h）；void print_pgdir(void) 已在 pmm.h 声明但 pmm.c 未实现，由本模块实现。
7) 错误码（libs/error.h）：E_INVAL=3, E_BAD_PROC=2, E_KILLED=9。
8) bool trap_in_kernel(struct trapframe *tf)（trap.c 已有）；libs/riscv.h：CAUSE_USER_ECALL=0x8、CAUSE_ILLEGAL_INSTRUCTION=0x2、CAUSE_BREAKPOINT=0x3、CAUSE_LOAD_PAGE_FAULT=0xd、CAUSE_STORE_PAGE_FAULT=0xf、CAUSE_FETCH_PAGE_FAULT=0xc、SSTATUS_SPP。
9) pmm.c 内已有 static 辅助：const char *perm2str(int perm)、int get_pgtable_items(size_t left,size_t right,size_t start,uintptr_t *table,size_t *left_store,size_t *right_store)；mmu.h：PDX1/PDX0/PTX/PGADDR/PDE_ADDR/PTE_*；pmm.h：KADDR/boot_pgdir_va。
10) struct mm_struct { ... pde_t *pgdir; ... }（vmm.h），mm->pgdir 是页表的内核虚拟地址；current->mm 可能为 NULL（内核线程）。

[GUARANTEE]
- 签名不可改：void syscall(void); static int sys_xxx(uint64_t arg[]); void print_pgdir(void)。
- sys_* 只做参数转换并调用 do_*；不要在里面直接改 PCB 字段（sys_getpid 读 current->pid 除外）。
- syscall：num = tf->gpr.a0；num >= NUM_SYSCALLS 或表项为 NULL → tf->gpr.a0 = -E_INVAL；否则 tf->gpr.a0 = syscalls[num]((uint64_t *)&tf->gpr.a1)。
- exception_handler：CAUSE_USER_ECALL → tf->epc += 4; syscall(); break; 用户态非法指令/缺页/访问错误 → print_trapframe(tf) 后 do_exit(-E_KILLED)（不返回）；用户态断点（ebreak）只前进 epc、不杀进程（gdb 用户态断点依赖它）；内核态异常保持原有 print/panic 行为；用户态 store page fault 处保留注释“[C 的 COW 接入点]”。
- print_pgdir：只读遍历、只 cprintf，不分配、不修改页表、不 panic；1GB 叶子页不再下钻。

[SPECIFICATION]
## syscall
Pre: current != NULL；current->tf 指向本次陷入的 trapframe；a0=调用号，a1..a5=参数。
Post: 有效号时 a0=被调 sys_* 的返回值；否则 a0=-E_INVAL；a1..a5/epc/status 不变。
Cases:
 Case 1 num>=NUM_SYSCALLS 或 syscalls[num]==NULL → -E_INVAL（不得越界访问）。
 Case 2 有效 → 调用并把 int 返回值写回 a0（负错误码按 64 位补码）。
 Case 3 被调函数不返回（sys_exit、sys_exec 成功）→ 不写回，随 trap 返回路径进入新上下文。
Requirements: 参数基址必须是 &tf->gpr.a1；不得用 a7 作调用号；不得修改分发表。

## sys_exit / sys_fork / sys_wait / sys_exec
exit: arg[0]=error_code，调 do_exit((int)arg[0])，不返回。
fork: tf=current->tf，返回 do_fork(0, tf->gpr.sp, tf)；子进程 a0=0 由 copy_thread 保证，父进程 a0=子 pid 由 syscall 写回。
wait: arg[0]=pid（-1 表示任一子进程），arg[1]=用户 int*；返回 do_wait(...)；非法 store 指针由 do_wait 返回 -E_INVAL，不得 panic。
exec: arg[0..3]=name,len,binary,size；返回 do_execve(...)；成功后不返回用户态。

## sys_yield / sys_kill / sys_getpid / sys_putc / sys_pgdir
yield: do_yield()。kill: do_kill((int)arg[0])，对 -1/不存在/idle/init 返回 -E_INVAL。
getpid: current->pid。putc: cputchar((int)arg[0]) 后返回 0。pgdir: print_pgdir() 后返回 0。

## exception_handler
Pre: tf 为当前陷阱帧，cause 最高位为 0（异常）。
Post: 用户 ecall 后 epc 指向 ecall 的下一条并完成系统调用；用户态其它异常使当前进程退出；内核态异常保留现场。
Cases:
 Case 1 CAUSE_USER_ECALL：tf->epc += 4（ecall 固定 4 字节；测试断言 0x1000→0x1004），调用 syscall()。
 Case 2 用户态非法指令/缺页/访问错误：print_trapframe(tf)，然后 do_exit(-E_KILLED)，不得返回；用户态断点（ebreak）例外：只打印并 tf->epc += insn_length(tf->epc)，不杀进程。
 Case 3 内核态异常：保持原有打印/panic，不得按用户态异常静默退出。
 Case 4 用户态 store page fault：在 do_exit 之前保留 COW 接入点注释（C 会插入 1-2 行），本模块不实现 COW。
Requirements: 不改 tf->epc 以外的 CSR；用户态异常分支不得 return。

## print_pgdir
Pre: 任意进程上下文可调用；current->mm 可能为 NULL。
Post: 只读打印当前地址空间的页表映射（GB 级叶子页 + 4KB 页区间与权限），含 BEGIN/END 行；系统状态不变。
Cases:
 Case 1 current->mm==NULL → 打印 boot_pgdir_va（只有 1GB 叶子页）。
 Case 2 一级表项为叶子（PTE_R|PTE_W|PTE_X 任一置位）→ 按 GB 区间打印，不下钻。
 Case 3 一级表项指向二级页表 → 逐项取三级页表，用 get_pgtable_items + perm2str 合并连续同权限页。
 Case 4 表项无效 → 跳过。
Requirements: 只用 KADDR 访问物理页；不得把叶子页当页表；输出行数可控（合并区间）。

验收：make 编译通过；make grade 的 [Test Syscall] PASS；hello/exit/pgdir 正常；faultread/faultreadkernel/testbss 无 "user panic at"。
````

### 2. proc.c 两处占位提示词（do_yield / do_kill）

````markdown
[PROMPT]
任务：只修改 kern/process/proc.c 的 “//lab5 function4 YOUR CODE” 处，实现 do_yield 与 do_kill（do_exit 由 B 负责，不要写）。
操作要求：保留 do_fork、KERNEL_EXECVE 宏、init_main、proc_init 等既有代码；不要动 374/506 行的 function2 标记与 vmm.c/sched.c；把 “YOUR CODE” 替换为 2410699。

[RELY]
current（proc.h）；idleproc/initproc 全局量；find_proc(int)；wakeup_proc（sched.h）；PF_EXITING、PROC_SLEEPING（proc.h）；E_INVAL（error.h）。

[GUARANTEE]
int do_yield(void)：置 current->need_resched = 1，返回 0，不直接调用 schedule()。
int do_kill(int pid)：find_proc(pid)；NULL/idleproc/initproc → -E_INVAL；否则置 PF_EXITING，若目标 SLEEPING 则 wakeup_proc，返回 0；不直接调用 do_exit。

[SPECIFICATION]
do_yield Pre: current 有效。Post: need_resched=1，返回 0，不切换上下文。
do_kill Pre: pid 为任意整数。Post: pid 不存在/-1/idle/init → -E_INVAL，无所改动；否则目标置 PF_EXITING，睡眠目标被唤醒，返回 0。
Cases: do_kill(-1) 必须返回 -E_INVAL（test_lab5_process_control 断言）；自旋中的目标由下一次时钟中断返回用户态前的 trap() 退出（spin 用例）。
验收：make 链接时 do_yield/do_kill 不再报 undefined；[Test Process Control] 中前两条断言通过。
````


---

## 2411433 李烁菲（成员 B）

> 待补充：功能模块二（`init_main/alloc_proc/kernel_thread/do_wait/user_main/kernel_execve/do_execve/load_icode`）与功能模块四（`do_exit`）的最终提示词、spec 与迭代记录；`do_yield/do_kill` 已由 A 实现，请复核。

---

## 2411570 何美璇（成员 C）

> 待补充：功能模块三（`do_fork/copy_mm/dup_mmap/copy_range`）与 Challenge（COW）的最终提示词、spec 与迭代记录。
