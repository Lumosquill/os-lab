# Lab 4 提示词（prompt.md）

2411433 李烁菲
```
[PROMPT]
任务：修改文件 kern/mm/pmm.c，实现“虚拟内存页操作”功能模块。该模块负责在现有 Sv39 三级页表框架上维护线性地址到物理页的映射：page_insert 把一个物理页挂到指定线性地址上（或更新已有映射），page_remove 撤销指定线性地址上的映射。模块必须保证页表项内容、物理页引用计数、TLB 状态三者始终一致。代码应放在 pmm.c 中标注 “//LAB4 function1 YOUR CODE” 的位置（get_pte/get_page 之后、tlb_invalidate 之前）。

操作要求：你必须在项目中修改实际文件，而不是仅展示代码。完成功能模块的实现，包括但不限于：两个公开函数、辅助函数、内部使用的宏。保留现有代码，只在上述标记位置补全，不要修改 get_pte、get_page、tlb_invalidate、check_pgdir、check_boot_pgdir 等已有函数，不要删除既有逻辑。文件使用 CRLF 换行，请保持原有换行风格。

输出要求：直接进行文件操作，使用下面 [RELY]、[GUARANTEE]、[SPECIFICATION] 部分的信息，以工程真实代码为准，不要引入无关功能。

[RELY]
// 权限位宏（libs/riscv.h）
PTE_V 0x001  // 有效
PTE_R 0x002  // 可读
PTE_W 0x004  // 可写
PTE_U 0x010  // 用户态可访问
// 错误码（libs/error.h）
E_NO_MEM 4   // 内存不足，函数应返回 -E_NO_MEM

// 类型
typedef uintptr_t pte_t;  typedef uintptr_t pde_t;
struct Page { int ref; ... };   // ref 为该物理页当前被引用（映射）的次数
extern struct Page *pages;       // 物理页描述符数组
extern pde_t *boot_pgdir_va;     // 内核启动页目录的虚拟地址

// 可用函数（kern/mm/pmm.h、pmm.c，签名以工程为准）
pte_t *get_pte(pde_t *pgdir, uintptr_t la, bool create);
    // 返回 la 对应的最后一级页表项指针。
    // create=0 且中间级页表不存在时返回 NULL；create=1 时会按需分配中间级页表页，
    // 分配失败返回 NULL。返回的 PTE 本身可能是无效的（未映射）。
    // 中间级页表页的引用计数由 get_pte 自己管理，本模块不要改动。
struct Page *alloc_page(void);
#define free_page(page) free_pages(page, 1)
struct Page *pte2page(pte_t pte);               // PTE 无效时会 panic，调用前必须确认 PTE_V
uintptr_t page2ppn(struct Page *page);          // 物理页号
pte_t pte_create(uintptr_t ppn, int type);      // 构造 PTE，结果已自动带有 PTE_V
int page_ref(struct Page *page);
void set_page_ref(struct Page *page, int val);
int page_ref_inc(struct Page *page);            // 引用计数 +1，返回新值
int page_ref_dec(struct Page *page);            // 引用计数 -1，返回新值
void tlb_invalidate(pde_t *pgdir, uintptr_t la); // 使 la 对应的 TLB 项失效（已在 pmm.c 中实现）
void flush_tlb(void);                            // 刷新整个 TLB

// 已有的相关调用方（供理解，不要修改）
// check_pgdir() / check_boot_pgdir() / tools/test_lab4.c 会调用 page_insert / page_remove，
// 并用断言检查引用计数、权限位、空闲页数是否正确。

[GUARANTEE]
必须实现的主要接口（声明已在 pmm.h 中，签名必须一致）：
int page_insert(pde_t *pgdir, struct Page *page, uintptr_t la, uint32_t perm);
void page_remove(pde_t *pgdir, uintptr_t la);

你可以根据需要添加：
- 辅助函数（必须设为 static，只在 pmm.c 内部使用，不得改变公开接口）
- 内部使用的宏定义

[SPECIFICATION]
## page_insert
Pre-Condition:
- pgdir 是有效的页目录虚拟地址；page 指向有效的物理页描述符；la 是任意线性地址；perm 是调用者希望设置的权限位（不含 PTE_V）。

Post-Condition:
- 成功时：la 稳定地映射到 page 对应的物理页，页表项的权限等于 perm 加上有效位；page 的引用计数正确反映“当前有多少个映射指向它”；la 对应的 TLB 旧翻译已失效；原先被 la 映射、现在不再被引用的物理页已被正确处理。
- 失败时：返回 -E_NO_MEM，页表、引用计数均未被修改，不留下半完成的映射。

  Case 1（无法定位页表项）:
  - 为 la 查找页表项时需要分配中间级页表页但分配失败。此时直接返回 -E_NO_MEM，不改变任何状态。

  Case 2（la 此前没有有效映射）:
  - 建立新映射：page 的引用计数增加一次，写入新的页表项，并使 la 的 TLB 失效，返回 0。

  Case 3（la 此前已映射到同一个物理页）:
  - 只需要更新权限位，page 的引用计数在整个调用前后保持不变，不能多加也不能释放该页；使 la 的 TLB 失效，返回 0。

  Case 4（la 此前映射到另一个物理页）:
  - 先撤销旧映射：旧物理页引用计数减一，若减到 0 则释放该物理页；再为 page 建立新映射，page 的引用计数加一；使 la 的 TLB 失效，返回 0。

Requirements:
- 增加新页引用计数的动作必须发生在处理旧映射之前，保证“重新映射到同一页”时该页不会被误释放。
- 不能简单覆盖旧页表项，否则会泄漏旧物理页的引用计数。
- 页表项一旦被修改，就必须使对应线性地址的 TLB 失效。

## page_remove
Pre-Condition:
- pgdir 是有效的页目录虚拟地址；la 是任意线性地址，该地址不一定存在映射。

Post-Condition:
- 调用结束后 la 不再有有效映射；原来被映射的物理页引用计数减一，减到 0 时被释放；la 对应的 TLB 项已失效。该函数是幂等的。

  Case 1（la 没有有效映射，包括中间级页表不存在）:
  - 什么都不做，安静返回，不得 panic，也不得为了删除而分配新的页表页。

  Case 2（la 存在有效映射）:
  - 物理页引用计数减一，若减到 0 则释放该页；清空页表项；使 la 的 TLB 失效。

Requirements:
- 不得操作或释放中间级页表页（其生命周期由 get_pte 及调用方管理）。
- “撤销一个已知页表项”的逻辑可抽成 static 辅助函数，供 page_insert 与 page_remove 共用。
- 实现后应能通过 check_pgdir、check_boot_pgdir 以及 tools/test_lab4.c 中 PMM 测试的全部断言（引用计数、权限位、空闲页数前后一致）。
```

---

2410699 张欣妍

```
[PROMPT]
任务：只修改 kern/process/switch.S、kern/schedule/sched.c、kern/process/proc.c 中
“//LAB4 function3”标记处，实现 Lab4 功能模块三“进程调度”：switch_to、proc_run、
cpu_idle、schedule。保留所有已有代码和其他标记，不要修改 kern/mm/pmm.c、proc.c 的
function2、proc.h、sched.h、sync.h、trap/*、init.c。

[RELY]
1. struct context（kern/process/proc.h）按顺序为：
   uintptr_t ra, sp, s0, s1, ..., s11; 每个 8 字节，偏移 0,8,16,...,112。
2. 声明：
   void switch_to(struct context *from, struct context *to);  // 参数在 a0/a1
   void proc_run(struct proc_struct *proc);
   void schedule(void);
   void cpu_idle(void) __attribute__((noreturn));
3. 全局量：current、idleproc、proc_list 已在 proc.c 定义并在 proc.h 声明。
4. struct proc_struct 字段：state、pid、runs、kstack、need_resched、parent、mm、
   context、tf、pgdir、flags、name、list_link、hash_link。
5. 状态：PROC_RUNNABLE；le2proc(le, list_link)；list_next()；
   local_intr_save/local_intr_restore（sync.h）。
6. riscv.h：SATP_MODE_SV39=8、RISCV_PGSHIFT=12、write_csr/read_csr；
   pmm.h：flush_tlb()；pmm.c 的 enable_paging() 是 Sv39 satp 写法参照：
   write_csr(satp, 0x8000000000000000 | (boot_pgdir_pa >> RISCV_PGSHIFT))。
7. next->pgdir 是页表根节点的物理地址（不是虚拟地址）。

[GUARANTEE]
1. switch_to：保存 from 的 ra/sp/s0..s11 到 from->context 对应偏移；
   加载 to 的 ra/sp/s0..s11；最后 ret 返回到 to->context.ra。
   只保存 callee-saved，不保存 a0/a1/t0..t6。
2. proc_run：proc == current 时直接返回；否则关中断，current = proc，
   写 satp 为 Sv39 + (proc->pgdir >> 12)，flush_tlb()，
   switch_to(&prev->context, &next->context)，最后恢复中断。
3. cpu_idle：while (1) { if (current->need_resched) schedule(); }，不得返回。
4. schedule：关中断；current->need_resched = 0；
   current == idleproc 时从 proc_list 头开始，否则从 current->list_link 之后开始；
   找第一个 PROC_RUNNABLE；找不到则 next = idleproc；next->runs++；
   next != current 时 proc_run(next)；最后恢复中断。
5. 所有对 current/proc_list/need_resched 的修改都在 local_intr_save/restore 临界区内。
6. 不使用 lsatp()（Sv32）；不把 pgdir 当虚拟地址；不重复右移 12 位；
   写 satp 后必须刷 TLB。

[SPECIFICATION]
## switch_to
Pre: from、to 指向有效 context；from 是当前线程，to 是目标线程。
Post: from 的 ra/sp/s0..s11 已保存；CPU 寄存器已恢复为 to 的值；
      执行流返回到 to->context.ra；不修改内存中的其他字段。

## proc_run
Pre: proc 是有效 PCB；可能在中断开启或关闭的上下文调用。
Post: proc == current 时无副作用返回；否则 current == proc，
      satp 指向 proc->pgdir（Sv39），TLB 已刷新，
      当前 CPU 上下文已切换到 proc；返回时“当前线程”是 proc。

## cpu_idle
Pre: current == idleproc，proc_init 已把 idleproc->need_resched 置 1。
Post: 永不返回；每次 need_resched 为真时调用 schedule()。

## schedule
Pre: current 有效；proc_list 可能为空；调用者可能持有或不持有中断。
Post: 选中一个 PROC_RUNNABLE 线程；若没有则选 idleproc；
      current->need_resched 清零；next->runs 加一；
      next != current 时完成一次 proc_run；返回时中断状态恢复到进入前。
Cases:
  Case 1: current != idleproc，从 current 之后轮转，回到 current 停止。
  Case 2: current == idleproc，从 proc_list 头开始。
  Case 3: 没有 RUNNABLE，next = idleproc 保底。
  Case 4: next == current，不调用 proc_run，只增加 runs。
  Case 5: proc_list 为空，仍能安全返回/切到 idleproc。
验收：make 编译通过；make qemu 打印 initproc 三行；
      make grade 的 [Test Schedule] PASS，总分 30/30。
```

---

2411570 何美璇

```
[PROMPT]
任务：修改文件 kern/process/proc.c，实现“进程创建及初始化”功能模块。该模块负责让内核从只有一条执行流，变成能够创建并登记多个内核线程：把当前执行现场包装成第 0 个内核线程 idleproc，再通过 kernel_thread 和 do_fork 创建第 1 个内核线程 initproc，并在新线程自己的内核栈上布置好它第一次被调度时需要的 trapframe 和 context，使它能够沿着 switch_to、forkret、forkrets、__trapret、kernel_thread_entry 这条路径开始执行。代码放在 proc.c 中标注 “//LAB4 function2 YOUR CODE” 的位置（init_main 之后、“//LAB4 function3 YOUR CODE” 之前）。

操作要求：你必须在项目中修改实际文件，而不是仅展示代码。完成功能模块的实现，包括但不限于：六个函数主体、辅助函数、内部使用的宏。保留现有代码，只在上述标记位置补全，不要修改 get_pid、hash_proc、find_proc、setup_kstack、put_kstack、forkret、init_main、do_exit 等已有函数，也不要修改 function3 标记处以及 sched.c、switch.S 等其他文件。

输出要求：直接进行文件操作，使用下面 [RELY]、[GUARANTEE]、[SPECIFICATION] 部分的信息，以工程真实代码为准，不要引入无关功能。

[RELY]
// 进程状态与结构（kern/process/proc.h）
enum proc_state { PROC_UNINIT = 0, PROC_SLEEPING, PROC_RUNNABLE, PROC_ZOMBIE };
struct context { uintptr_t ra, sp, s0, s1, s2, ..., s11; };   // 切换时保存的 14 个寄存器
struct proc_struct {
    enum proc_state state;  int pid;  int runs;  uintptr_t kstack;  volatile bool need_resched;
    struct proc_struct *parent;  struct mm_struct *mm;  struct context context;
    struct trapframe *tf;  uintptr_t pgdir;  uint32_t flags;  char name[PROC_NAME_LEN + 1];
    list_entry_t list_link;  list_entry_t hash_link;
};
#define MAX_PROCESS 4096        // 系统允许的最大进程数
// pgdir 保存页目录的物理地址，进程切换时会被写入 satp

// trapframe（kern/trap/trap.h）
struct trapframe { struct pushregs gpr; uintptr_t status; uintptr_t epc; uintptr_t badvaddr; uintptr_t cause; };
// gpr 中包含 sp、a0、s0、s1 等通用寄存器。sret 返回时，epc 是返回地址，status 决定返回后的特权级和中断使能。

// sstatus 位与 CSR 访问（libs/riscv.h）
SSTATUS_SIE 0x2    // 当前是否允许中断
SSTATUS_SPIE 0x20  // sret 之后 SIE 的取值
SSTATUS_SPP 0x100  // sret 之后的特权级：1 表示内核态（S 模式），0 表示用户态
read_csr(sstatus)

// 全局变量与常量（proc.c / pmm.h / memlayout.h）
list_entry_t proc_list;                          // 所有进程的链表
static list_entry_t hash_list[HASH_LIST_SIZE];   // 按 pid 的哈希表
struct proc_struct *idleproc, *initproc, *current;
static int nr_process;                           // 当前进程数
extern uintptr_t boot_pgdir_pa;                  // 内核页表的物理地址
extern char bootstack[];                         // 启动阶段使用的栈
#define KSTACKSIZE (KSTACKPAGE * PGSIZE)         // 内核栈大小，KSTACKPAGE 为 2
#define CLONE_VM 0x00000100                      // 表示共享地址空间
// 错误码（libs/error.h）
E_NO_MEM 4        // 返回 -E_NO_MEM
E_NO_FREE_PROC 5  // 返回 -E_NO_FREE_PROC

// 可用函数（签名以工程为准）
void *kmalloc(size_t n);  void kfree(void *p);  void *memset(void *s, char c, size_t n);
static int get_pid(void);                        // 返回当前未被占用的 pid
static void hash_proc(struct proc_struct *proc); // 按 pid 插入哈希表
struct proc_struct *find_proc(int pid);
static int setup_kstack(struct proc_struct *proc);   // 成功返回 0，失败返回 -E_NO_MEM，成功时设置 proc->kstack
static void put_kstack(struct proc_struct *proc);    // 释放 proc 的内核栈
char *set_proc_name(struct proc_struct *proc, const char *name);
void wakeup_proc(struct proc_struct *proc);          // 要求 state 不是 ZOMBIE 和 RUNNABLE，把它设为 RUNNABLE
void list_init(list_entry_t *elm);  void list_add(list_entry_t *listelm, list_entry_t *elm);  // 加在 listelm 之后
local_intr_save(x); local_intr_restore(x);       // 关中断/恢复中断，x 是 bool 变量
static void forkret(void);                       // 已定义：新线程第一次被 switch_to 切换进来后到达这里，调用 forkrets(current->tf)
void kernel_thread_entry(void);                  // 汇编（process/entry.S）：调用 s0(s1)，返回后调用 do_exit
static int init_main(void *arg);                 // 已定义：initproc 的入口函数，打印 pid、名字和参数

// 已有的相关调用方（供理解，不要修改）
// init.c 在 kern_init 中调用 proc_init。
// tools/test_lab4.c 的 Process Create 测试在 proc_init 结束后检查：
//   current == idleproc；idleproc 的 pid 为 0、state 为 RUNNABLE、kstack 等于 bootstack、
//   parent/mm/tf 为 NULL、runs 和 flags 为 0、context.ra 为 0；
//   initproc 的 pid 为 1、state 为 RUNNABLE、parent 等于 idleproc、mm 为 NULL、context.ra 和 context.sp 不为 0。
// Schedule 测试会再调用 kernel_thread 创建两个线程，并让三个线程轮流运行。

[GUARANTEE]
必须实现的主要接口（声明已在 proc.h 中，签名必须一致）：
static struct proc_struct *alloc_proc(void);
void proc_init(void);
int kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags);
int do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf);
static void copy_thread(struct proc_struct *proc, uintptr_t esp, struct trapframe *tf);
static int copy_mm(uint32_t clone_flags, struct proc_struct *proc);

你可以根据需要添加：
- 辅助函数（必须设为 static，只在 proc.c 内部使用，不得改变公开接口）
- 内部使用的宏定义

[SPECIFICATION]
## alloc_proc
Pre-Condition:
- 内核堆分配器 kmalloc 已经初始化。

Post-Condition:
- 成功时返回一个处于“刚创建、尚未运行”状态的 PCB，之后任何代码读取其中没有被显式赋值的字段，都只会得到安全的空值；失败时返回 NULL。

  Case 1（内存不足）:
  - kmalloc 失败，返回 NULL，不改变任何其他状态。

  Case 2（成功）:
  - 状态为未初始化态，pid 为 -1 表示尚未分配，页目录基址 pgdir 为内核页表的物理地址，其余字段（运行次数、内核栈、need_resched、父进程、mm、context、tf、flags、name、两个链表节点）全部为 0 或空。

Requirements:
- pgdir 必须是物理地址而不是虚拟地址：内核线程共用内核页表，进程切换时这个值会被写入 satp。
- 不能留下未初始化的随机值：Process Create 测试会检查 idleproc 的 context.ra 为 0、tf 为 NULL、flags 为 0。

## copy_mm
Pre-Condition:
- 调用者是内核线程，current 的 mm 为 NULL。

Post-Condition:
- 返回 0，不创建任何新的地址空间，proc 的 mm 保持为 NULL。

Requirements:
- 当 current 的 mm 不为 NULL 时，说明这不是内核线程的创建，应当用断言暴露出来，而不是悄悄忽略。
- 保持函数形态完整，便于后续实验为用户进程扩展。

## copy_thread
Pre-Condition:
- proc 已经拥有有效的内核栈；tf 指向调用者准备好的 trapframe；esp 为 0 表示新线程是内核线程。

Post-Condition:
- proc 的 trapframe 位于它自己内核栈的最高处，内容是传入 trapframe 的拷贝，并且子线程的返回值为 0。proc 的 context 设置为：第一次被 switch_to 切换进来时进入 forkret，并且 forkret 运行时使用的栈位于 trapframe 之下，不会覆盖 trapframe。

  Case 1（esp 为 0，内核线程）:
  - trapframe 中的栈指针设为该 trapframe 自己所在的地址，线程之后在这个位置之下使用栈。传入的 trapframe 是由 kernel_thread 清零构造的，如果不设置这一项，线程第一次运行时栈指针为 0。

  Case 2（esp 不为 0）:
  - trapframe 中的栈指针使用调用者给定的 esp。

Requirements:
- trapframe 必须完整地放在 kstack 到 kstack + KSTACKSIZE 的范围之内。
- trapframe 中的 epc、status、s0、s1 必须原样保留，它们决定线程第一次运行时从哪里开始、以什么状态运行。

## do_fork
Pre-Condition:
- current 是调用 do_fork 的线程；tf 指向要复制给子线程的 trapframe；stack 为 0 表示内核线程。

Post-Condition:
- 成功时新线程已经创建并登记：它的 pid 在现有进程中唯一，父进程为 current，已加入进程链表和按 pid 的哈希表，进程总数加 1，状态为就绪，函数返回它的 pid。失败时返回负的错误码，并且不留下已经分配却没有释放的资源。

  Case 1（进程数已达上限）:
  - 返回 -E_NO_FREE_PROC，不分配任何资源。

  Case 2（PCB 分配失败）:
  - 返回 -E_NO_MEM，没有需要释放的资源。

  Case 3（内核栈分配失败）:
  - 释放已经分配的 PCB，返回 -E_NO_MEM。

  Case 4（copy_mm 失败）:
  - 释放已经分配的内核栈和 PCB，返回 -E_NO_MEM。

  Case 5（成功）:
  - 按上面的 Post-Condition 完成登记，返回新线程的 pid。

Requirements:
- 父进程必须显式设置为 current：alloc_proc 得到的 parent 为空，Process Create 测试会检查 initproc 的 parent 是 idleproc。
- pid 分配、插入哈希表、插入进程链表、进程数加 1 必须作为一个整体完成，期间不能被中断打断。
- 状态从未初始化变为就绪，必须在线程完全登记之后，通过现有的 wakeup_proc 完成。
- 失败路径按资源分配的相反顺序释放。

## kernel_thread
Pre-Condition:
- fn 是线程入口函数，arg 是传给它的参数；current 指向调用者。

Post-Condition:
- 返回新线程的 pid 或 do_fork 返回的负错误码。新线程第一次运行时在内核态执行 fn(arg)，运行时中断是打开的，fn 返回后进入 do_exit。

  Case 1（do_fork 失败）:
  - 原样返回错误码。

  Case 2（成功）:
  - 返回新线程的 pid。

Requirements:
- 临时 trapframe 先整体清零，再设置：线程函数和参数通过 kernel_thread_entry 约定使用的两个寄存器传递；epc 指向 kernel_thread_entry。
- status 在当前 sstatus 的基础上要满足三点：sret 之后回到内核态（置 SPP）；sret 之后打开中断（置 SPIE）；恢复寄存器的过程中不接受中断（清 SIE）。漏掉 SPP，线程会回到用户态并在取第一条指令时异常；漏掉 SPIE，线程会在关中断状态下运行，三个测试仍然能通过，因此必须由规格保证。
- 调用 do_fork 时 clone_flags 要加上 CLONE_VM，stack 参数为 0。

## proc_init
Pre-Condition:
- 内存管理已经初始化，kmalloc 可用；此时 current 为空。

Post-Condition:
- 进程链表和哈希表已初始化为空表；idleproc 是当前执行现场对应的第 0 个线程，current 指向它；initproc 是第 1 个线程，名字为 init，入口为 init_main，参数为 "Hello world!!"，已经就绪等待调度。

  Case 1（idleproc 分配失败）:
  - 调用 panic 终止初始化。

  Case 2（initproc 创建失败，返回的 pid 不大于 0）:
  - 调用 panic 终止初始化。

  Case 3（成功）:
  - idleproc 的 pid 为 0，状态为就绪，内核栈为启动时使用的 bootstack，need_resched 为 1，名字为 idle，不加入 proc_list（它只作为调度器找不到其他线程时的保底选择），进程计数包含它。initproc 的 pid 为 1，父进程为 idleproc，名字为 init。

Requirements:
- current 必须在调用 kernel_thread 之前就指向 idleproc：do_fork 会把 current 记为父进程，copy_mm 也会读取 current。
- 实现后应能通过 tools/test_lab4.c 中 Process Create 测试以及 Schedule 测试的全部断言。
```
