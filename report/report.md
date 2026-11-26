# OSLab5 实验报告

## 实验基本信息

| 项目       | 内容                                  |
| -------- | ----------------------------------- |
| **实验名称** | Lab 5: 用户环境与系统调用                    |
| **小组成员** | 2411570-何美璇、2410699-张欣妍、2411433-李烁菲 |
| **完成日期** | 2026-11-26                          |

### 小组分工

**练习分工**

| 成员          | 负责的模块                            | 额外任务                  |
| ----------- | -------------------------------- | --------------------- |
| 2410699-张欣妍 | 功能模块一：用户态系统调用功能                  | Challenge 思考题 1、分支任务（双重 gdb） |
| 2411433-李烁菲 | 功能模块二：创建加载第一个用户进程；功能模块四：退出/让出/终止 | —                     |
| 2411570-何美璇 | 功能模块三：复制用户进程                     | Challenge：COW 机制       |

**报告分工**

| 成员          | 负责的报告部分                                              |
| ----------- | ---------------------------------------------------- |
| 2410699-张欣妍 | 实验目的、实验环境、整体逻辑分析、功能模块一、Challenge 思考题 1、分支任务 gdb、测试汇总 |
| 2411433-李烁菲 | 功能模块二、功能模块四，相关测试截图                                    |
| 2411570-何美璇 | 功能模块三、COW Challenge，相关测试截图                            |

## 一、实验目的

1. 理解 RISC-V 的三级特权级（M/S/U）与用户态/内核态的切换过程，掌握 `ecall` 主动陷入、`sret` 降权返回这一对硬件机制。
2. 掌握系统调用框架的实现：用户库封装 → 异常入口 → 系统调用分发 → 内核语义落地 → 返回值写回用户寄存器，理解 `trapframe` 在其中的载体作用。
3. 了解第一个用户进程的创建与装载：用户程序如何被链接进内核镜像、`kernel_execve` 如何借助异常返回路径第一次进入 U 态、`load_icode` 如何按 ELF 建立用户地址空间。
4. 了解 `fork/exec/wait/exit/yield/kill` 在 ucore 中的实现方式，把“内核线程”推进到“用户进程的完整生命周期”。
5. 练习以“规格（spec）+ 提示词”的方式与 AI 协作开发操作系统核心代码，并对生成结果做编译、反汇编、自测与联调验证。

## 二、实验环境

**AI 编程工具**

| 成员          | AI 编程工具          | 底层模型                | 备注  |
| ----------- | ---------------- | ------------------- | --- |
| 2411570-何美璇 | Claude Code      | Claude Sonnet 5.5   |     |
| 2410699-张欣妍 | DeepSeek Harness | DeepSeek-V4.1-Flash |     |
| 2411433-李烁菲 | claude           | claude sonnet 5     |     |

**运行环境与工具链**

| 项目    | 内容                                                        |
| ----- | --------------------------------------------------------- |
| 运行环境  | WSL2 / Ubuntu 24.04.5 LTS                                 |
| 交叉编译器 | `riscv64-unknown-elf-gcc` 13.2.0                          |
| 交叉调试器 | `gdb-multiarch` 15.1                                      |
| 模拟器   | `qemu-system-riscv64` 4.1.1                               |
| 构建工具  | GNU Make 4.3、Python 3（`tools/inject.py`、`tools/grade.sh`） |

## 三、实验整体逻辑分析

### 3.1 本章节的逻辑主线

Lab4 结束时，系统里只有运行在 S 态的内核线程；Lab5 要完成的是“让用户程序真正在 U 态跑起来，并且能通过系统调用获得内核服务”。整章可以概括成一条从内核启动一直到用户程序系统调用返回的完整链路：

```text
kern_init
  → pmm_init / vmm_init
  → proc_init（idleproc、initproc）
  → cpu_idle → schedule → proc_run
  → init_main
  → user_main
  → kernel_execve
  → do_execve → load_icode（解析 ELF、建用户地址空间、填 trapframe）
  → __trapret → RESTORE_ALL → sret          ← 第一次从 S 态进入 U 态
  → 用户程序执行
  → syscall() 封装：a0=调用号、a1~a5=参数、ecall
  → __alltraps → SAVE_ALL                    ← 从 U 态陷入 S 态
  → trap → exception_handler（tf->epc += 4）
  → syscall()（读 tf->gpr.a0，参数基址 &tf->gpr.a1）
  → sys_exit / sys_fork / sys_wait / sys_exec / sys_yield / sys_kill / sys_getpid / sys_putc / sys_pgdir
  → do_exit / do_fork / do_wait / do_execve / do_yield / do_kill / print_pgdir
  → 返回值写回 tf->gpr.a0
  → __trapret → RESTORE_ALL → sret          ← 从 S 态返回 U 态
  → 用户程序继续执行
```

沿着这条链路，可以看到三个必须同时成立的“边界”：

1. **特权级边界**：用户程序不能直接调用内核函数，只能把请求放进约定寄存器后执行 `ecall`。硬件负责把 `sepc/scause/stvec` 填好、把特权级切到 S 态；软件负责越过 `ecall`（`epc += 4`）、分发、返回。
2. **地址空间边界**：用户进程有自己的页表（`mm_struct`、`vma_struct`），内核态访问用户指针必须先过 `user_mem_check`，不能拿用户传来的地址直接解引用。
3. **进程生命周期边界**：`fork` 复制执行环境，`exec` 替换映像，`exit` 回收资源并变僵尸，`wait` 由父进程完成最终回收，`kill` 只是置退出标志。四者共用 `alloc_proc/do_fork/do_wait/do_exit` 这套进程状态机。

因此本章不是四个孤立练习，而是“入口（模块一）→ 第一个进程（模块二）→ 复制（模块三）→ 收尾（模块四）”的一条闭环。

### 3.2 功能的逐步实现

本章按“先有入口、再有进程、然后复制、最后收尾”的顺序推进，这个顺序由依赖关系决定：

1. **功能模块一：用户态系统调用功能。** `exception_handler` 识别 `CAUSE_USER_ECALL` 并把控制权交给 `syscall()`，`syscall()` 从 `trapframe` 取号与参数并分发到 9 个 `sys_*`，返回值再写回 `a0`；同时 `print_pgdir` 为 `SYS_pgdir` 提供内核实现，`trapentry.S` 要能正确处理用户态/内核态两个方向的返回。没有这个入口，后面模块二/三/四写好的 `do_*` 根本不会被用户程序触发。
2. **功能模块二：创建加载第一个用户进程。** `alloc_proc` 补齐 `wait_state/cptr/yptr/optr` 等字段，`init_main` 创建 `user_main` 并等待其结束，`user_main` 通过 `KERNEL_EXECVE` 宏选中镜像中的用户程序，`kernel_execve` 手工构造 `trapframe` 并走 `__trapret/sret`，`do_execve/load_icode` 解析 ELF、建立用户地址空间、创建用户栈。没有这一步，系统里没有 U 态程序，模块一的系统调用无从验证。
3. **功能模块三：复制用户进程。** `do_fork` 要维护 `parent/cptr/yptr/optr` 与 `wait_state`，`copy_mm` 根据 `clone_flags` 决定共享还是复制 `mm`，`dup_mmap` 按 `vma` 复制区间，`copy_range` 按页复制内容并建立映射。`fork` 是 `exec/wait/exit` 能被“父子进程”场景验证的前提，`testbss/forktest/exit` 等用例都依赖它。
4. **功能模块四：其余系统调用。** `do_exit` 回收地址空间、置僵尸、唤醒父进程；`do_yield` 置 `need_resched`；`do_kill` 置 `PF_EXITING` 并在下次陷入返回时退出。这是生命周期的收尾，必须和 `do_wait` 的等待/唤醒逻辑严格对齐。

联调顺序同样是这个依赖：先保证模块一编译通过并能通过注入的 `[Test Syscall]`；等模块二让 `hello` 跑起来，才能看到真实的 `ecall/sret`；再用模块三让 `exit/forktest` 跑通；最后靠模块四让 `exit/spin/waitkill` 收敛。

## 四、实验内容与实现

### 功能模块一：用户态系统调用功能

**负责人：2410699-张欣妍**

#### 模块功能描述

**需要实现/修改的函数：**

```c
// kern/syscall/syscall.c
void syscall(void);
static int sys_exit(uint64_t arg[]);
static int sys_fork(uint64_t arg[]);
static int sys_wait(uint64_t arg[]);
static int sys_exec(uint64_t arg[]);
static int sys_yield(uint64_t arg[]);
static int sys_kill(uint64_t arg[]);
static int sys_getpid(uint64_t arg[]);
static int sys_putc(uint64_t arg[]);
static int sys_pgdir(uint64_t arg[]);

// kern/trap/trap.c
void exception_handler(struct trapframe *tf);

// kern/mm/pmm.c（pmm.h 已声明，未实现，不补会链接失败）
void print_pgdir(void);

// kern/process/proc.c（分工表分配的两处占位）
int do_yield(void);
int do_kill(int pid);
```

**功能说明：**

本模块是用户态获得内核服务的唯一入口。用户程序在用户库里把系统调用号和参数放进 `a0`、`a1~a5`，执行 `ecall`；硬件把 `sepc` 指向 `ecall`、`scause` 置 `8`（`CAUSE_USER_ECALL`）、跳到 `stvec` 并切到 S 态；`trapentry.S` 保存现场后，`exception_handler` 把 `epc` 前进 4 字节并调用 `syscall()`；`syscall()` 从当前进程的 `trapframe` 取号、分发、把结果写回 `a0`；最后 `sret` 把控制权交还用户程序。`print_pgdir` 与 `do_yield/do_kill` 分别补上了 `SYS_pgdir` 和 `SYS_yield/SYS_kill` 的内核实现。

**1. `syscall()` 的分发逻辑**

```c
//lab5 function1 2410699
static int sys_exit(uint64_t arg[]) { return do_exit((int)arg[0]); }

static int sys_fork(uint64_t arg[]) {
    struct trapframe *tf = current->tf;              // 本次 ecall 的 trapframe
    return do_fork(0, (uintptr_t)tf->gpr.sp, tf);    // 子进程用父进程的用户栈
}

static int sys_wait(uint64_t arg[]) {
    return do_wait((int)arg[0], (int *)arg[1]);
}

static int sys_exec(uint64_t arg[]) {
    return do_execve((const char *)arg[0], (size_t)arg[1],
                     (unsigned char *)arg[2], (size_t)arg[3]);
}

static int sys_yield(uint64_t arg[]) { return do_yield(); }
static int sys_kill(uint64_t arg[])  { return do_kill((int)arg[0]); }
static int sys_getpid(uint64_t arg[]) { return current->pid; }

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
    uint64_t num = tf->gpr.a0;                       // 本实验：a0 = 调用号
    if (num < NUM_SYSCALLS && syscalls[num] != NULL) {
        // pushregs 里 a1..a7 连续，&a1 就是参数数组首地址（a1..a5 共 5 个）
        tf->gpr.a0 = (uintptr_t)syscalls[num]((uint64_t *)&tf->gpr.a1);
    } else {
        tf->gpr.a0 = (uintptr_t)-E_INVAL;            // 非法编号
    }
}
```

要点：

- 调用号读 `tf->gpr.a0`（不是 Linux 习惯的 `a7`），参数从 `a1` 开始，`&tf->gpr.a1` 直接当作 `uint64_t arg[]` 用，这是由 `struct pushregs` 的字段顺序保证的。
- 越界或表项为 `NULL` 一律返回 `-E_INVAL`，不能拿非法编号去索引函数指针数组。
- 返回值写回 `a0`；负错误码按 64 位补码写回，用户库按 `int` 读低 32 位即可。
- `sys_exit` 和成功的 `sys_exec` 都不返回，走的是进程上下文替换/退出路径，不会执行到写回语句。

**2. 九个 `sys_*` 的语义**

| 系统调用 | 编号 | 参数（a1 起） | 内核动作 | 返回 |
| --- | --- | --- | --- | --- |
| `sys_exit` | 1 | `error_code` | `do_exit`：回收 `mm`、置 `PROC_ZOMBIE`、唤醒父进程 | 不返回 |
| `sys_fork` | 2 | 无 | `do_fork(0, tf->gpr.sp, tf)`：复制当前进程 | 父进程得子 pid；子进程 `a0=0` |
| `sys_wait` | 3 | `pid`、`code_store` | `do_wait`：睡眠等待子进程、回收僵尸 | 0 / `-E_BAD_PROC` / `-E_INVAL` |
| `sys_exec` | 4 | `name,len,binary,size` | `do_execve`：替换当前进程映像 | 成功不返回 |
| `sys_yield` | 10 | 无 | `do_yield`：置 `need_resched` | 0 |
| `sys_kill` | 12 | `pid` | `do_kill`：置目标 `PF_EXITING` 并唤醒 | 0 / `-E_INVAL` |
| `sys_getpid` | 18 | 无 | 读 `current->pid` | pid |
| `sys_putc` | 30 | `c` | `cputchar`：走 SBI 输出字符 | 0 |
| `sys_pgdir` | 31 | 无 | `print_pgdir`：打印页表 | 0 |

- `fork` 的返回值由两部分组成：父进程的 `a0` 是 `syscall()` 写回的 `do_fork` 返回值（子 pid），子进程的 `a0=0` 是 `copy_thread` 复制 `trapframe` 时置的，A 不能在 `sys_fork` 里清 `a0`。
- `wait` 的 `code_store` 是用户指针，`do_wait` 内部要用 `copy_to_user`/`user_mem_check` 校验；非法指针（如 `0xC0000000`）应返回 `-E_INVAL` 而不是 panic，`badarg` 用例依赖这一点。
- `exec` 在本实验没有用户库封装：第一个用户程序由内核 `kernel_execve` 直接调 `do_execve`；`sys_exec` 仍按指导书实现，保证接口完整，其 `name` 指针的合法性检查放在 `do_execve` 里（`current->mm == NULL` 时 `user_mem_check` 退化为 `KERN_ACCESS`，内核指针也能通过）。

**3. `exception_handler` 的异常分流**

```c
//lab5 function1 2410699 exception_handler
void exception_handler(struct trapframe *tf) {
    switch (tf->cause) {
        case CAUSE_ILLEGAL_INSTRUCTION:
            cprintf("Illegal instruction at 0x%08x\n", tf->epc);
            if (trap_in_kernel(tf)) {
                tf->epc += insn_length(tf->epc);     // 内核态：越过出错指令
            } else {
                do_exit(-E_KILLED);                  // 用户态跑飞：杀进程
            }
            break;
        case CAUSE_BREAKPOINT:
            cprintf("Breakpoint at 0x%08x\n", tf->epc);
            // 不要杀进程：gdb 在用户程序里下断点靠 ebreak
            tf->epc += insn_length(tf->epc);
            break;
        case CAUSE_USER_ECALL:
            tf->epc += 4;                            // ecall 固定 4 字节
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
            /* [C 的 COW 接入点] 只对 STORE_PAGE_FAULT 有意义：
             * if (tf->cause == CAUSE_STORE_PAGE_FAULT && cow_pgfault(tf) == 0) break;
             */
            if (trap_in_kernel(tf)) {
                print_trapframe(tf);
                panic("unexpected trap in kernel.\n");
            }
            print_trapframe(tf);
            do_exit(-E_KILLED);                      // 用户态缺页：终止，不能返回
            break;
        default:
            if (trap_in_kernel(tf)) {
                print_trapframe(tf);
                break;                               // 保持 lab4 行为：只打印
            }
            print_trapframe(tf);
            do_exit(-E_KILLED);
            break;
    }
}
```

设计理由：

- `ecall` 必须 `+4`：RISC-V 硬件在 `ecall` 陷入时把 `sepc` 设成 `ecall` 自己的地址，不会自动跳过；不 `+4`，`sret` 会回到同一条 `ecall`，形成死循环。`ecall` 没有压缩形式，固定 4 字节，比用 `insn_length` 更直接。
- 用户态缺页必须终止进程：`faultread` 读地址 0、`faultreadkernel` 读 `0xfac00000` 都会触发 load page fault；如果只 `print_trapframe` 后返回，`sret` 会再次执行同一条出错指令，串口会无限刷 `trapframe`。`do_exit(-E_KILLED)` 让进程回收、父进程能 `wait` 到它，内核本身不受影响。
- 用户态断点不能杀进程：gdb 在用户程序里下断点、`ebreak` 调试都依赖断点异常；如果这里 `do_exit`，第 6 节的双重 gdb 调试会在第一个断点处把进程杀掉。所以断点分支只前进 `epc`。
- 给 C 留 COW 接入点：COW 在 `store page fault` 时可能合法地“拷贝一页后继续”，因此在这一组 case 的开头保留了带注释的插入点，C 只需要加 1~2 行 `cow_pgfault` 调用，不需要改 A 的分支结构。

**4. `print_pgdir()` 的实现**

```c
//lab5 function1 2410699 print_pgdir
void print_pgdir(void) {
    cprintf("-------------------- BEGIN --------------------\n");
    pde_t *pgdir = (current->mm != NULL && current->mm->pgdir != NULL)
                       ? current->mm->pgdir : boot_pgdir_va;
    for (size_t i = 0; i < NPDEENTRY; i++) {
        if (!(pgdir[i] & PTE_V)) continue;
        if (pgdir[i] & (PTE_R | PTE_W | PTE_X)) {     // 1GB 叶子页（boot 页表第 511 项）
            cprintf("  |-- PDE(%s) %016lx-%016lx\n", perm2str(pgdir[i] & PTE_USER),
                    PGADDR(i, 0, 0, 0), PGADDR(i + 1, 0, 0, 0) - 1);
            continue;                                  // 绝不能当二级页表下钻
        }
        pde_t *pd0 = (pde_t *)KADDR(PDE_ADDR(pgdir[i]));
        for (size_t j = 0; j < NPDEENTRY; j++) {
            if (!(pd0[j] & PTE_V)) continue;
            pte_t *pt = (pte_t *)KADDR(PDE_ADDR(pd0[j]));
            size_t start = 0, left, right;
            int perm;
            while ((perm = get_pgtable_items(0, NPTEENTRY, start, pt, &left, &right)) != 0) {
                cprintf("  |-- PTE(%s) %016lx-%016lx\n", perm2str(perm),
                        PGADDR(i, j, left, 0), PGADDR(i, j, right, 0) - 1);
                start = right;
            }
        }
    }
    cprintf("-------------------- END --------------------\n");
}
```

要点：

- 用 `current->mm->pgdir`（内核虚拟地址）而不是 `current->pgdir`（物理地址，给 `satp` 用）；内核线程 `mm == NULL` 时退回 `boot_pgdir_va`。
- Sv39 允许 level-1 表项直接映射 1GB（boot 页表第 511 项就是 `0x80000000` 的 1GB 叶子页），这种表项 `R/W/X` 至少一位为 1；如果把它当二级页表下钻，会读到物理内存里的任意数据当 PTE，输出乱码甚至 panic。
- 4KB 页用 `get_pgtable_items` 按“连续同权限”合并区间，避免 512×512 全表打印把串口刷爆；`perm2str` 输出 `u/r/w` 权限，`PGADDR` 还原区间首尾地址。

**5. `trapentry.S` 双向返回核对**

| 场景 | 进入时 `sscratch` | `SAVE_ALL` 后 slot2（x2 槽） | `RESTORE_ALL` 的 SPP | 是否写 `sscratch` | `sret` 后 |
| --- | --- | --- | --- | --- | --- |
| 内核态陷入 | 0 | 陷入前的内核 `sp` | 1 | 否（保持 0） | 回 S 态，`sp=slot2` |
| 用户态陷入 | 内核栈顶 | 用户 `sp` | 0 | 是，`sp+36*8` = 内核栈顶 | 回 U 态，`sp=用户 sp` |
| 新进程首次经 `forkrets` | 0 | `a0` 指向的 tf 的 slot2 | 由 `load_icode` 置 0 | 是，`tf+288` = 内核栈顶 | 回 U 态，入口由 `tf->epc` 决定 |
| `kernel_execve` 成功返回 | 0（内核线程） | 同上 | 0 | 同上 | 回 U 态，执行用户入口 |
| 系统调用返回 | 用户态进入时已存内核栈顶 | 用户 `sp` | 0 | 是 | 回 U 态，`a0` 为返回值 |

核对结论：当前 `RESTORE_ALL` 已由 lab4 扩展为“按 `SSTATUS_SPP` 双向恢复”：返回用户态时把“弹出 trapframe 后的内核栈顶”写回 `sscratch`，返回内核态时保持 `sscratch=0`。`trapframe` 大小为 `32*8 + 4*8 = 288 = 36*REGBYTES`，与 `addi sp, sp, -36*REGBYTES`、`sp+36*REGBYTES` 一致。本模块只核对，不修改 `trapentry.S`。

**6. `do_yield` / `do_kill`（分工表分配的两处占位）**

```c
//lab5 function4 2410699（do_yield / do_kill；do_exit 由 B 接着写）
int do_yield(void) {
    current->need_resched = 1;
    return 0;
}

int do_kill(int pid) {
    struct proc_struct *proc = find_proc(pid);
    if (proc == NULL || proc == idleproc || proc == initproc) {
        return -E_INVAL;
    }
    proc->flags |= PF_EXITING;
    if (proc->state == PROC_SLEEPING) {
        wakeup_proc(proc);
    }
    return 0;
}
```

- `do_yield` 只置 `need_resched`，不直接调 `schedule()`：`trap()` 在返回用户态前会检查 `need_resched` 并调度，直接切栈会打乱系统调用的返回路径。
- `do_kill` 不直接 `do_exit`（杀的是别的进程），只置 `PF_EXITING` 并唤醒睡眠目标；目标进程下一次从内核返回用户态时，由 `trap()` 里的 `PF_EXITING` 分支退出。`spin` 用例（父进程杀自旋的子进程）依赖的正是这条路径。
- `test_lab5_process_control` 的断言 `do_kill(-1) == -E_INVAL` 由 `find_proc` 返回 `NULL` 保证；`idleproc/initproc` 不允许被杀。

#### 最终提示词

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

proc.c 两处占位的提示词：

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

#### 实现迭代过程

本模块共经历 3 轮主要迭代。

##### 第一次迭代：函数体放在分发表之后导致编译失败

**遇到的问题：**

- `syscalls[]` 在文件顶部就引用了 `sys_exit` 等函数，而函数体按提示词写在了表后面。C 语言要求“先声明后使用”，编译报 `'sys_exit' undeclared`（一堆同类错误）。
- 第一版提示词只写了“实现 `syscall` 和 9 个 `sys_*`”，没有说明分发表的位置，AI 自然把函数体追加在标记处。

**问题解决策略：**

- 在 `static int (*syscalls[])(uint64_t arg[])` 之前补 9 行 `static` 函数原型，函数体仍放标记处；把“分发表在表顶部、必须补原型”写进 `[PROMPT]` 的“操作要求”。
- 同时补上 `<error.h>`（`-E_INVAL` 需要），并把 `syscalls[num] != NULL` 的边界检查写进 `[GUARANTEE]`。

##### 第二次迭代：构建环境把 `-Wint-conversion` 当 error

**遇到的问题：**

- `make` 在 `kern/mm/kmalloc.c:92` 报 `passing argument 1 of 'kva2page' makes pointer from integer without a cast [-Wint-conversion]`，整树编译中断。`kva` 是 `unsigned long`，`kva2page` 形参是 `void *`；GCC 14 起该警告默认就是 error。

**问题解决策略：**

- 按 lab4 在 `proc.c:320` 的同样修法，改成 `free_pages(kva2page((void *)kva), 1 << order);`，并把这条作为“框架构建兼容性修复”单独记录、单独提交，避免和 A 的功能代码混在一起。
- 没有用 `-Wno-int-conversion` 全局关闭：那会掩盖真正的指针类型错误，且会影响另外两位成员的构建。

##### 第三次迭代：用户态断点被当成“跑飞”杀掉

**遇到的问题：**

- 第一版 `exception_handler` 把用户态 `CAUSE_BREAKPOINT` 和非法指令一样处理成 `do_exit(-E_KILLED)`。这样第 6 节的双重 gdb 在 `user/libs/syscall.c` 下断点时，进程会在第一个断点处被杀掉，根本无法继续单步到 `ecall`。
- 同时 `print_pgdir` 第一版把 boot 页表第 511 项的 1GB 叶子页当成二级页表下钻，打印出乱码（读物理内存当 PTE）。

**问题解决策略：**

- 把断点分支改成“两种模式都只打印 + `tf->epc += insn_length(tf->epc)`”，并把这个约束写进 `[SPECIFICATION]` 的 `exception_handler` Case 2。
- `print_pgdir` 增加叶子页判断：`pgdir[i] & (PTE_R|PTE_W|PTE_X)` 为真就按 GB 区间打印并 `continue`，不再 `KADDR` 下钻；页表指针改用 `current->mm->pgdir`（虚拟地址）。

### Challenge 题目 1：说明该用户程序是何时被预先加载到内存中的？与我们常用操作系统的加载有何区别，原因是什么？

**负责人：2410699-张欣妍**

**（1）构建时：用户程序被链接进内核镜像**

`Makefile` 在内核链接阶段使用了：

```make
$(LD) $(LDFLAGS) -T tools/kernel.ld -o $@ $(KOBJS) --format=binary $(USER_BINS) --format=default
```

`--format=binary` 让链接器把 `obj/__user_xxx.out` 当成“二进制大数组”直接嵌进 `bin/kernel`，并为每个文件生成 `_binary_obj___user_xxx_out_start`、`_binary_obj___user_xxx_out_size` 等符号（分别表示数组首地址和长度）。`proc.c` 里的 `KERNEL_EXECVE(x)` 宏正是用这两个符号拿到用户程序的镜像地址和大小，交给 `kernel_execve`。也就是说，在编译链接阶段，用户程序的机器码就已经“在内存里”了——以字节数组的形式存在于内核镜像中。

**（2）启动时：随内核镜像进入物理内存**

QEMU 的启动命令是：

```text
qemu-system-riscv64 -machine virt -nographic -bios default \
    -device loader,file=bin/ucore.img,addr=0x80200000
```

`-device loader` 把整个 `ucore.img`（包含内核代码、数据以及嵌入的用户程序字节）搬到物理地址 `0x80200000`；OpenSBI 从 `0x80000000` 启动后跳到 `0x80200000` 执行 `kern_entry`。因此，内核还没开始执行时，用户程序的字节已经躺在物理内存里了。

**（3）运行时：`load_icode` 把它“搬”进用户地址空间**

真正的“加载”发生在 `init_main → user_main → kernel_execve → do_execve → load_icode`：`load_icode` 把内存中的 ELF 头解析出来，按 `proghdr` 分配物理页、拷贝代码段/数据段/`bss`，用 `mm_map` 登记 `vma`，建好用户栈，填好 `trapframe` 的入口地址、用户栈顶和 `sstatus`，最后经 `__trapret/sret` 进入 U 态。这一步没有任何磁盘 I/O，本质是“从内核镜像的一块内存拷贝到用户地址空间的另一块内存”。

**与常用操作系统的区别：**

| 方面 | ucore lab5 | 常用 OS（Linux） |
| --- | --- | --- |
| 可执行文件存放 | 无文件系统，链接进内核镜像 | 存放在磁盘文件系统（ext4 等） |
| 进入内存的时机 | 构建时嵌入 + 启动时随内核镜像进物理内存 | `execve` 时才通过 VFS 从磁盘读入 |
| 链接方式 | 静态链接，固定地址 `0x800020`（`tools/user.ld`） | 支持 PIE/ASLR、动态链接器、共享库 |
| 装载粒度 | `load_icode` 一次性映射全部段 | 按需分页，首次访问触发 page fault |
| 程序数量 | 一次启动一个测试程序（`-DTEST=xxx`） | 任意多程序，按路径 `exec` |
| 共享库 | 无 | `.so` 共享映射、`ld.so` 重定位 |

**原因：**

Lab5 的重点是特权级切换、系统调用和进程复制，此时还没有文件系统（后续实验才做），也没有按需分页；如果用“磁盘 + 文件系统 + 按需调页”的完整加载路径，会引入大量与本章目标无关的复杂度。链接进内核镜像让构建链最简单、镜像自包含、实验可复现；`--format=binary` 还能直接给出起止符号，方便 `kernel_execve` 取镜像。代价是内核体积变大、不能运行任意程序、没有 ASLR，而且调试用户程序时要手动 `add-symbol-file`（见第 6 节）——因为内核镜像里只有用户程序的裸字节，没有它的 ELF 符号表。

### 分支任务：gdb 调试系统调用以及返回

**负责人：2410699-张欣妍**

> 说明：本节的操作流程、QEMU 源码分析和观察要点已经写完；具体的寄存器数值、调用栈与截图依赖“B/C 合入后 `hello` 能跑起来”，属最后收尾任务，实测后补入标记为【待实测】的位置。




## 五、测试与验证

### 5.1 已完成（成员 A，阶段性）

1. **编译**：A 的三个文件 `-fsyntax-only` 通过；全量 `make` 编译阶段通过，链接错误从 5 个符号降到 3 个（仅剩 B 的 `do_exit/do_wait/do_execve`）。
2. **注入测试（临时 stub 环境）**：`[Test Process Create] PASS`、`[Test Syscall] PASS`；`test_lab5_process_control` 在第 57 行 `do_wait` 断言处失败，证明其前面的 `do_yield`、`do_kill` 断言均已通过。原始串口输出已在“功能模块一 → 验证结果”引用。
3. **`trapentry.S` 核对**：5 场景表全部成立，未修改该文件。

### 5.2 待完成（依赖 B/C）

| 测试项 | 命令 | 预期 |
| --- | --- | --- |
| 第一个用户程序 | `make build-hello && make qemu` | `Hello world!!.` / `I am process 2.` / `hello pass.` |
| `ecall` 观测 | 第 6 节双重 gdb | 6.4 表与截图 |
| `fork/wait/exit` | `make build-exit && make qemu` | `exit pass.` / `waitpid ... ok.` |
| 用户态异常 | `make build-faultread && make qemu` | 无 `user panic at` |
| 页表打印 | `make build-pgdir && make qemu` | `print_pgdir` 输出 + `pgdir pass.` |
| 全量评分 | `make grade` | 13 个程序，`Total Score: 135/135`（3×10 + 7×10 + 2×10 + 15） |

**截图清单（待补，放入 `report/images/`）**

| 文件名 | 内容 | 状态 |
| --- | --- | --- |
| `lab5_a_test_syscall.png` | 串口 `[Test Syscall] PASS`（stub 自测） | 待补 |
| `lab5_a_qemu_hello.png` | `hello` 运行输出 | 待补 |
| `lab5_a_faultread.png` | `faultread` 被终止、无 `user panic at` | 待补 |
| `lab5_a_pgdir.png` | `pgdir` 程序与 `print_pgdir` 输出 | 待补 |
| `lab5_a_grade.png` | `make grade` 全绿 + 总分 | 待补 |
| `lab5_a_gdb_ecall.png` | ucore gdb 停在 `ecall` 前 | 待补 |
| `lab5_a_gdb_qemu_ecall.png` | QEMU gdb 命中 `helper_ecall`/`riscv_cpu_do_internrupt` | 待补 |
| `lab5_a_gdb_qemu_sret.png` | QEMU gdb 命中 `helper_sret` | 待补 |

## 六、实验总结与收获

### 对操作系统的理解

#### 1. 本实验知识点与 OS 原理知识点的对照

| 实验中的知识点 | 对应 OS 原理中的知识点 | 含义、关系与差异 |
| --- | --- | --- |
| `ecall` + `CAUSE_USER_ECALL` + `sret` | 系统调用 / 陷入与返回 | 实验用 RISC-V 的 `ecall` 主动陷入、`sepc` 保存返回地址，软件 `+4` 越过 `ecall`；原理中的 `int 0x80`/`syscall` 是同类机制，硬件细节不同 |
| `trapframe` | 中断/异常现场、进程上下文 | 实验把 32 个通用寄存器 + `status/epc/tval/cause` 压在内核栈；原理中的“进程上下文”还包括页表、打开文件等 |
| `user_mem_check` | 用户指针校验 / 地址空间隔离 | 实验用 `vma` + `USER_ACCESS` 检查；原理中由 MMU + 内核 `copy_from_user` 共同保证 |
| `print_pgdir` 遍历 Sv39 | 多级页表 / 地址翻译 | 实验三级页表、1GB 叶子页特判；原理中还有 TLB、页表缓存、缺页处理 |
| 用户程序链接进内核 | 程序装载（loader） | 实验是“内存到内存”的 `load_icode`；原理中 loader 从文件系统读 ELF、建立 `mm_struct`、按需调页 |
| `sstatus.SPP/SPIE/SIE`、`sscratch` | 特权级切换 / 内核栈 | 实验用 `sscratch` 一条指令完成用户栈/内核栈交换；原理中 x86 用 TSS/内核栈，机制不同但目标一致 |

#### 2. OS 原理中重要、但本实验没有对应上的知识点

1. **文件系统与可执行文件加载**：本实验的用户程序是链接进内核镜像的，没有 VFS、磁盘 I/O、`execve` 路径、动态链接与共享库。
2. **按需分页与页面置换**：`load_icode` 一次性映射全部页，没有缺页按需调页、页面置换、swap（COW 也只在 Challenge 中模拟）。
3. **进程间通信与同步**：没有管道、共享内存、信号、信号量、死锁处理；本实验只有 `PF_EXITING`、`wait_state` 这种最基础的等待/唤醒。
4. **多核与调度策略**：单核、简单轮转；没有 per-CPU 队列、优先级、CFS、实时调度。
5. **安全与隔离的完整实现**：有 U/S 特权级和 `user_mem_check`，但没有 ASLR、栈保护、能力机制、审计等。

### AI 协作开发的经验

**成员 A（张欣妍 2410699）：**

第一版提示词只写了“实现 syscall 和 9 个 sys_\*”，AI 就把函数体追加在标记处，结果因为分发表在文件顶部、C 要求先声明后使用，直接报 `'sys_exit' undeclared`。后来我把“分发表在表前、必须补 9 个原型”“参数基址是 `&gpr.a1` 而不是 `a0`”“`ecall` 固定 `+4`、测试断言 `0x1004`”这些工程事实写进 `[RELY]`/`[GUARANTEE]`，生成结果一次就过。

此外，本次实验过程中还体会到：规格要写到"可验证"的程度。比如“用户态缺页要杀进程”如果只写“处理用户态异常”，AI 可能只打印现场就返回，现象是同一条指令无限 fault；写成“`print_trapframe` 后 `do_exit(-E_KILLED)`，不得返回，验收标准是 `faultread` 输出无 `user panic at`”，就把行为和测试对上了。再比如“断点不能杀进程”这条，是我在准备 gdb 任务时才意识到——如果按“用户态异常一律杀进程”实现，gdb 在用户程序里下的第一个断点就会把进程杀掉，整个分支任务做不了。这类跨任务的隐含约束，是提示词里最值得写、也最容易被忽略的部分。



