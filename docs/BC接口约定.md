# Lab4 B/C 接口约定（进程创建 × 进程调度）

> 目的：B 负责 `proc_init`/`do_fork`/`copy_thread` 等创建线，C 负责 `switch_to`/`proc_run`/`cpu_idle`/`schedule` 调度线；两人的代码在 `proc.c` 中共用，必须先把 ABI 对齐，避免联调返工。
>
> 版本：2026-11-13 初稿（C 张欣妍 2410699）
>
> 约定变更请直接改本文件并在群里说明；不要只改代码不改约定。

---

## 1. 文件与标记归属

| 文件 | B 改 | C 改 | 备注 |
|---|---|---|---|
| `kern/process/proc.c` | `//LAB4 function2` 标记处：`alloc_proc`、`proc_init`、`kernel_thread`、`do_fork`、`copy_thread`、`copy_mm` | `//LAB4 function3` 标记处：`proc_run`、`cpu_idle` | **共用文件，只改各自标记，不格式化对方代码** |
| `kern/process/switch.S` | 不改 | `switch_to` | 只有 C 改 |
| `kern/schedule/sched.c` | 不改 | `schedule` | `wakeup_proc` 已存在，不要动 |
| `kern/mm/pmm.c` | 不改 | 不改 | A 的 `page_insert`/`page_remove`，已完成 |
| `kern/process/proc.h`、`kern/schedule/sched.h`、`kern/sync/sync.h` | 不改 | 不改 | `struct context` 是 ABI，谁改谁负责全组返工 |

**学号标记**：B 写 `//LAB4 function2 2411570`（何美璇）；C 写 `//LAB4 function3 2410699`（张欣妍）。保留 `LAB4 functionX` 前缀，便于老师检查。

---

## 2. `struct context` 布局（不可改）

`kern/process/proc.h`：

```c
struct context {
    uintptr_t ra;
    uintptr_t sp;
    uintptr_t s0;
    uintptr_t s1;
    uintptr_t s2;
    uintptr_t s3;
    uintptr_t s4;
    uintptr_t s5;
    uintptr_t s6;
    uintptr_t s7;
    uintptr_t s8;
    uintptr_t s9;
    uintptr_t s10;
    uintptr_t s11;
};
```

| 偏移 | 字段 | 偏移 | 字段 |
|---|---|---|---|
| 0 | `ra` | 56 | `s5` |
| 8 | `sp` | 64 | `s6` |
| 16 | `s0` | 72 | `s7` |
| 24 | `s1` | 80 | `s8` |
| 32 | `s2` | 88 | `s9` |
| 40 | `s3` | 96 | `s10` |
| 48 | `s4` | 104 | `s11` |

- 每个字段 8 字节，`sizeof(struct context) == 112`。
- C 的 `switch.S` 按上表硬编码偏移；B 不要增删字段、不要调整顺序。
- 建议 B 在 `proc_init` 里加一行临时检查（联调后可删）：
  ```c
  static_assert(sizeof(struct context) == 14 * sizeof(uintptr_t));
  ```

---

## 3. `copy_thread` 与首次运行约定（B 实现，C 依赖）

B 的 `copy_thread(proc, esp, tf)` 必须满足：

1. 在 `proc->kstack + KSTACKSIZE` 往下留出 `sizeof(struct trapframe)`，令 `proc->tf` 指向它，并把模板 `*tf` 复制进去。
2. `proc->tf->gpr.a0 = 0;`（子线程返回值 0）。
3. `proc->tf->gpr.sp = (esp == 0) ? (uintptr_t)proc->tf : esp;`
4. `proc->tf->gpr.s0 = fn;`、`proc->tf->gpr.s1 = arg;`
5. `proc->tf->epc = (uintptr_t)kernel_thread_entry;`
6. `proc->tf->status = (read_csr(sstatus) | SSTATUS_SPP | SSTATUS_SPIE) & ~SSTATUS_SIE;`
7. `proc->context.ra = (uintptr_t)forkret;`
8. `proc->context.sp = (uintptr_t)proc->tf;`

这样 C 的 `switch_to` 恢复 `ra/sp` 后会执行：

```
switch_to ret
  -> forkret
  -> forkrets(current->tf)      // trapentry.S：move sp, a0; j __trapret
  -> __trapret
  -> sret                        // epc = kernel_thread_entry
  -> kernel_thread_entry         // move a0, s1; jalr s0
  -> fn(arg)
  -> do_exit
```

**B 最容易错的三处**：

| 错误 | 现象 |
|---|---|
| `context.sp` 写成 `0` 或写成 `kstack + KSTACKSIZE` | `switch_to` 后 `sp` 错，第一次运行崩溃 |
| `tf.gpr.s0/s1` 写成 `a0/a1` | `kernel_thread_entry` 跳到错误地址/参数错 |
| `tf.status` 忘记 `SPIE` | 新线程 `sret` 后中断未开；Schedule 可能仍能跑，但不符合 spec |

---

## 4. `pgdir` 必须是物理地址（B 实现，C 依赖）

- `alloc_proc` 里：`proc->pgdir = boot_pgdir_pa;`
- **不要**写 `boot_pgdir_va`（虚拟地址）。
- C 的 `proc_run` 会写：
  ```c
  write_csr(satp, 0x8000000000000000 | (next->pgdir >> RISCV_PGSHIFT));
  flush_tlb();
  ```
- 联调检查：gdb 里 `p/x current->pgdir` 应是 `0x8020....` 量级的**物理地址**，不是 `0xFFFFFFFFC0...`。

---

## 5. `idleproc` 不挂 `proc_list`（B 实现，C 依赖）

- `proc_init` 里不要把 `idleproc` `list_add` 进 `proc_list`；`idleproc` 是调度器的保底，不是普通就绪线程。
- `proc_init` 必须先 `list_init(&proc_list)`、初始化 `hash_list`，再设置 `current = idleproc`、`idleproc->need_resched = 1`，最后 `kernel_thread(init_main, ...)`。
- `do_fork` 负责把新线程挂入 `proc_list` 和 `hash_list`，设置 `parent`、`pid`、`state = PROC_RUNNABLE`。
- 标准 ucore 的 `proc_init` 只初始化 `idleproc` 的 `pid/state/kstack/need_resched/name`，然后 `current = idleproc`，再 `kernel_thread(init_main, ...)`。
- 如果 B 把 `idleproc` 挂进链表：C 的 `schedule` 从 `initproc` 轮转时可能选中 `idleproc`；lab4 没有定时器设置 `need_resched`，`cpu_idle` 会空转，`[Test Schedule]` 挂起。
- 联调检查：
  ```bash
  grep -n "list_add(&proc_list" kern/process/proc.c
  ```
  只应看到 `do_fork` 里那一处（给新线程挂链表），不应看到 `idleproc` 的 `list_add`。

---

## 6. `switch_to` ABI（C 实现）

```c
void switch_to(struct context *from, struct context *to);
```

- 参数：`a0 = from`，`a1 = to`。
- 只保存 **callee-saved**：`ra`、`sp`、`s0`~`s11`；不保存 `a0/a1/t0`~`t6`。
- 保存到 `from->context` 的对应偏移；从 `to->context` 恢复；最后 `ret` 到 `to->context.ra`。
- 不写 `satp`、不刷 TLB（这些是 `proc_run` 的职责）。

C 的 `switch.S` 已按第 2 节偏移实现，B 不需要改。

---

## 7. `proc_run` / `schedule` / `cpu_idle` 职责（C 实现，B 只需知道）

### `proc_run(proc)`

- `proc == current` 直接返回。
- 否则关中断 → `current = proc` → 写 Sv39 `satp`（`proc->pgdir >> 12`）→ `flush_tlb()` → `switch_to(&prev->context, &next->context)` → 恢复中断。

### `schedule()`

- 关中断；`current->need_resched = 0`。
- `current == idleproc` 时从 `proc_list` 头找；否则从 `current` 的下一个找（轮转）。
- 找第一个 `PROC_RUNNABLE`；找不到退回 `idleproc`。
- `next->runs++`；`next != current` 时 `proc_run(next)`。
- 恢复中断。

### `cpu_idle()`

```c
while (1) {
    if (current->need_resched) {
        schedule();
    }
}
```

- `proc_init` 必须把 `idleproc->need_resched` 初始化为 1，`kern_init` 最后调用 `cpu_idle()` 才会让出 CPU 给 `initproc`。

---

## 8. 联调顺序与验收

1. B 先完成 `alloc_proc/proc_init/kernel_thread/do_fork/copy_thread/copy_mm`，保证 `make` 能链接。
2. B 单独验证 `[Test Process Create] PASS`：
   ```bash
   make grade
   ```
   即使 Schedule 还没过，先看 Process Create。
3. C 完成 `switch_to/proc_run/cpu_idle/schedule`，单文件编译 + `objdump` 核对偏移。
4. B/C 一起跑：
   ```bash
   make clean && make && make qemu
   ```
   期望 initproc 三行 + `process exit!!.`。
5. 跑 `make grade`，期望：
   ```
   check PMM: OK
   check Process Create: OK
   check Schedule: OK
   Total Score: 30/30
   ```

---

## 9. 工程与提交约定

- **只用 `Makefile`**，不要用 `CMakeLists.txt`（它是过期文件，引用了不存在的 `ide.c`/`swap.c`/`rand.c`）。
- `Makefile`、`tools/grade.sh`、`tools/inject.py` 必须是 **LF**；在 WSL/Ubuntu 里先 `dos2unix`。
- `kmalloc.c:92` 在 GCC 15 下需要显式转换：`kva2page((void *)kva)`（已修）。
- 分支：`lab4`；目录：`code/` + `report/`；本文档放 `docs/`（最终提交前如果老师严格要求只保留两个文件夹，可移到 `report/` 或删除）。
- 提交信息建议：
  - B：`feat(lab4): implement process creation (2411570)`
  - C：`feat(lab4): implement switch_to/proc_run/cpu_idle/schedule (2410699)`
  - 合：`test(lab4): pass PMM/Process Create/Schedule 30/30`

---

## 10. B 确认清单（请在群里回复或在本文件打勾）

- [ ] `struct context` 布局不改，`sizeof == 112`
- [ ] `copy_thread` 的 `context.ra = forkret`、`context.sp = proc->tf`
- [ ] `tf.gpr.s0 = fn`、`tf.gpr.s1 = arg`、`tf.epc = kernel_thread_entry`、`tf.gpr.a0 = 0`
- [ ] `tf.status` 置 `SPP/SPIE`、清 `SIE`
- [ ] `alloc_proc` 的 `proc->pgdir = boot_pgdir_pa`（物理地址）
- [ ] `proc_init` 不把 `idleproc` 挂进 `proc_list`
- [ ] `proc.c` 只改 function2 标记，不碰 function3
- [ ] `Makefile`/`grade.sh`/`inject.py` 已转 LF
- [ ] 同意按第 8 节顺序联调

---

## 11. 变更记录

| 日期 | 修改人 | 内容 |
|---|---|---|
| 2026-11-13 | C 张欣妍 | 初稿：context 布局、copy_thread、pgdir、idleproc、switch_to ABI、联调顺序 |
