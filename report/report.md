# OSlab3 实验报告

## 实验基本信息

| 项目       | 内容                                  |
| -------- | ----------------------------------- |
| **实验名称** | Lab 4: 进程管理                      |
| **小组成员** | 2411570-何美璇、2410699-张欣妍、2411433-李烁菲 |
| **完成日期** | 2026-11-                           |

### 小组分工

| 成员          | 负责的练习/模块                                    | 负责的实验报告模块                                                       |
| --------------   | ------------------------------------------- | --------------------------------------------------------------- |
| 2411433-李烁菲   | 功能模块一+challenge2 | 实验目的，逻辑主线，模块一和challenge2相关部分 |
| 2411570-何美璇 |  |  |
| 2410699-张欣妍 |  |  |



## 一、实验目的

1.了解虚拟内存管理的基本结构，掌握虚拟内存的组织与管理方式

2.了解内核线程创建/执行的管理过程

3.了解内核线程的切换和基本调度过程


## 二、实验环境

你们使用的 AI 工具

| 成员          | AI 编程工具          | 底层模型                | 备注  |
| ----------- | ---------------- | ------------------- | --- |
| 2411570-何美璇 | Claude Code      | Claude Sonnet 5.5   |     |
| 2410699-张欣妍 | DeepSeek Harness | DeepSeek-V4.1-Flash |     |
| 2411433-李烁菲 | claude           | claude sonnet 5     |     |

**运行环境与工具链**

| 项目 | 内容 |
| --- | --- |
| 运行环境 | WSL2 / Ubuntu 26.04 LTS |
| 交叉编译器 | `riscv64-unknown-elf-gcc` 15.1.0 |
| 交叉调试器 | `riscv64-unknown-elf-gdb` 16.3 |
| 模拟器 | `qemu-system-riscv64` 4.1.1 |
| 构建工具 | GNU Make 4.4.1 |



## 三、实验整体逻辑分析

### 3.1 本章节的逻辑主线

本实验的目标，是让内核从“已经具备物理内存管理和基础页表能力”，走到“能够创建、登记、调度线程并完成上下文切换”。整个启动与执行链可以概括为：

`kern_init → pmm_init → pic_init / idt_init → vmm_init → proc_init → cpu_idle → schedule → proc_run → switch_to`

沿着这条链，本实验可以分成三条相互衔接的主线：

1. **虚拟内存页操作（地址空间准备）。**
   `pmm_init` 建立物理内存管理和内核页表后，内核需要一套可靠的“增删映射”接口，即 `page_insert` 和 `page_remove`。它们决定一个线性地址最终对应哪一页物理内存，并保证页表项、物理页引用计数、TLB 三者一致。这是后面所有地址空间相关逻辑的基础。

2. **进程创建与初始化（线程从哪里来）。**
   `proc_init` 把当前执行现场包装成 `idleproc`，再通过 `kernel_thread → do_fork → copy_thread` 创建第一个真正的内核线程 `initproc`：分配 PCB、内核栈和 PID，登记到链表，并在新线程的内核栈上预先布置好首次运行所需的 `trapframe` 和 `context`。

3. **进程调度（线程如何运行）。**
   `cpu_idle` 在空闲时检测重调度标志并调用 `schedule` 选出下一个可运行线程，`proc_run` 切换 `current` 和页表，`switch_to` 完成寄存器上下文的保存与恢复。新线程第一次运行时，沿着 `forkret → forkrets → __trapret` 的链路进入 `kernel_thread_entry`，最终执行 `initproc` 的入口函数。


### 3.2 功能的逐步实现







## 四、实验内容与实现

### 功能模块一：虚拟内存页操作功能

**负责人**：2411433-李烁菲

#### 模块功能描述

**需要实现的函数：**
```c
static inline void page_remove_pte(pde_t *pgdir, uintptr_t la, pte_t *ptep)
{
    if (*ptep & PTE_V)
    {
        struct Page *page = pte2page(*ptep);
        if (page_ref_dec(page) == 0)
        {
            free_page(page);
        }
        *ptep = 0;
        tlb_invalidate(pgdir, la);
    }
}

int page_insert(pde_t *pgdir, struct Page *page, uintptr_t la, uint32_t perm)
{
    pte_t *ptep = get_pte(pgdir, la, 1);
    if (ptep == NULL)
    {
        return -E_NO_MEM;
    }
    page_ref_inc(page);              // 先加引用，防止旧映射就是同一页时被误释放
    if (*ptep & PTE_V)
    {
        struct Page *old = pte2page(*ptep);
        if (old == page)
        {
            page_ref_dec(page);      // 同一物理页：只更新权限，抵消多加的引用
        }
        else
        {
            page_remove_pte(pgdir, la, ptep);   // 别的物理页：先撤销旧映射
        }
    }
    *ptep = pte_create(page2ppn(page), PTE_V | perm);
    tlb_invalidate(pgdir, la);
    return 0;
}

void page_remove(pde_t *pgdir, uintptr_t la)
{
    pte_t *ptep = get_pte(pgdir, la, 0);   // create=0，删除时不分配页表页
    if (ptep != NULL)
    {
        page_remove_pte(pgdir, la, ptep);
    }
}

```

**功能说明：**

1.int page_insert(pde_t *pgdir, struct Page *page, uintptr_t la, uint32_t perm)

在页表中建立线性地址 la 到物理页 page 的映射，权限为 perm。

如果 la 已映射到同一物理页，只更新权限位，引用计数不变。
如果 la 已映射到另一物理页，先撤销旧映射（旧页引用计数减一，减到 0 则释放），再建立新映射。
成功返回 0；为 la 分配中间级页表页失败时返回 -E_NO_MEM，且不修改任何状态。
页表项更新后会使 la 对应的 TLB 项失效。

2.void page_remove(pde_t *pgdir, uintptr_t la)     撤销 la 上的映射。

如果有映射：对应物理页引用计数减一，减到 0 则释放，同时清空页表项并使 TLB 失效。
如果没有映射：安静返回，不报错，所以重复删除同一地址也是安全的。
查找页表项时不会分配新的页表页。

3.辅助函数 static void page_remove_pte(pde_t *pgdir, uintptr_t la, pte_t *ptep)

撤销一个已经定位好的页表项，上面两个函数共用它。






#### 最终提示词
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




#### 实现迭代过程

本模块的实现经历了 1 次迭代，过程如下：

##### 第一次迭代

**遇到的问题：**

第一版提示词只列出了函数名清单，规格只有两句话概括，没有拆分 Case。根据它写出的代码有以下问题：

1.同一物理页重复插入时引用计数错乱：只要旧 PTE 有效就先撤销旧映射，导致原地址已映射到同一页时，该页先被减到 0 释放，再被重新映射。

2.`page_remove` 用 `create=1` 调用 `get_pte`：对未映射地址会白白分配页表页，造成内存泄漏，空闲页数断言失败。

3.未判断 PTE 有效性就调用 `pte2page`：删除未映射地址时直接 panic。

4.分配失败用 `assert` 处理，而不是返回 `-E_NO_MEM`；覆盖旧映射的路径还漏了 TLB 刷新。

**问题解决策略：**

[RELY] 补全了接口行为：`get_pte` 的 `create` 语义、`pte2page` 遇到无效 PTE 会 panic、引用计数的加减与释放约定。

[SPECIFICATION] 把 `page_insert` 拆成 4 个 Case（无法定位页表项、原地址无映射、同一物理页、另一物理页），`page_remove` 拆成 2 个 Case（无映射、有映射），并在 Requirements 中要求先加新页引用再处理旧映射、改页表必刷 TLB、失败不留半完成状态。

##### **最终结果：**
经过 1 次迭代，最终实现的 `page_insert` 与 `page_remove` 满足提示词中的全部约束，能够通过 PMM 相关测试。




### 功能模块二：

**负责人：** 

#### 模块功能描述

**需要实现的函数：**

```c


```

**功能说明：**



#### 最终提示词

以下是经过迭代优化后，最终成功实现该功能的提示词：

````

````

#### 实现迭代过程

<font color="">本模块的实现经历了 1 次迭代，过程如下：</font>

##### 第一次迭代

**遇到的问题：**



**问题解决策略：**



**最终结果：**



**关键改进点总结：**




### 功能模块三：

**负责人：** 

#### 模块功能描述

**需要实现的函数：**

```c


```

**功能说明：**





#### 最终提示词

以下是经过迭代优化后，最终成功实现该功能的提示词：

````markdown


````

#### 实现迭代过程

本模块的实现经历了 2 次迭代，过程如下：

##### 第一次迭代

**遇到的问题：**



**问题解决策略：**



##### 第二次迭代

**遇到的问题：**


**问题解决策略：**



**最终结果：**



**关键改进点总结：**



#### 验证结果


### challenge题目1


### challenge题目2

**1.get_pte() 函数中有两段形式类似的代码，结合 sv32、sv39、sv48 的异同，解释这两段代码为什么如此相像。**

get_pte() 在 Sv39 下要走三级页表：先用线性地址的最高一段索引（PDX1）在根页表里找表项，再用中间一段索引（PDX0）在第二级页表里找表项，最后用 PTX 在第三级页表里取出真正的页表项。前两步做的事情完全一样：看这个表项有没有效，没有效就分配一页、清零、把它的物理页号写进上一级表项，然后取出这一页的地址继续往下走。两段代码的唯一区别，只是用的索引和所在的那一级页表不同。

之所以会这样，是因为多级页表每一层的结构都是相同的：每一级页表都是一个 4KB 的页，里面是一排页表项，每个表项都由有效位、权限位和物理页号组成，要么指向下一级页表，要么指向最终的物理页。结构一样，处理它的代码自然也一样。

再看 Sv32、Sv39、Sv48 三种模式，它们的页大小和页表项格式基本一致，区别只在层数和每级索引的位数：Sv32 是两级，Sv39 是三级，Sv48 是四级。所以"检查并分配"这段代码需要重复的次数，总是总层数减一：Sv32 只需要一段，Sv39 需要两段，Sv48 就需要三段。这就是 get_pte() 里两段代码如此相像的原因，它们是在同一种结构的不同层次上做同一件事。

**2.目前 get_pte() 函数将页表项查找和页表页分配合并在一个函数里，你认为这种写法好吗？有没有必要把两个功能拆开？**

我认为这种写法有一些问题，应该拆开。

合在一起的好处是用起来简单：调用者只要通过 create 参数就能决定是"只查"还是"查不到就建"，而且只需要沿着页表走一遍，效率也不错。对于教学用的小内核，代码短、容易读懂。

但它的问题也很明显。首先，一个函数同时承担了"查询"和"修改页表结构"两种职责，像 page_remove 这种只想查询的调用，也带着分配的逻辑，一旦误传了 create=1，就会产生白白分配页表页的副作用。其次，失败时只返回 NULL，调用者分不清是"本来就没有映射"还是"想创建但内存不足"。再次，每多一级页表就要多复制一段分配代码，将来要改分配细节，或者换成 Sv48，很容易漏改某一段。最后，分配逻辑藏在查找里面，不方便单独测试，以后要加锁或者做失败回滚也不好改。

所以我的看法是有必要拆开：可以拆成一个只负责查找、没有副作用的函数，和一个只负责"为某个无效表项分配并挂接下一级页表"的函数，再由 get_pte() 把两者组合起来，走到哪一级断了就在那一级补上。这样只读的调用不会意外分配内存，错误也能区分清楚，重复的分配代码只用写一份，换分页模式时改动也小。当然拆开会多一些函数调用，要注意不要因此多遍历一次页表。不过对本实验这样规模的内核来说，不拆也能正常工作，所以这更多是代码质量和实现简单之间的取舍，而不是对错问题。


## 五、测试与验证

**测试截图：**


**编译与运行：**



**测试结果：**


## 六、实验总结与收获

### 对操作系统的理解

#### 1. 本实验知识点与 OS 原理知识点的对照



#### 2. OS 原理中重要、但本实验没有对应上的知识点



### AI 协作开发的经验

**成员A（李烁菲2411433）**：
在与AI协作的过程中，定义需求规格和约束条件很重要。在初始阶段，由于指令较为笼统，AI 生成的代码在处理物理页引用计数和 TLB 刷新时出现了逻辑漏洞，导致测试无法通过。我于是我转变了协作方式：我将功能需求拆解为具体的 Case（例如：映射建立、冲突处理、权限更新等），并明确了每一步的状态约束。这种“先定义规格，再由 AI 实现”的模式，提升了开发的准确性。




**成员B（何美璇2411570）**：




**成员C（张欣妍 2410699）**：


