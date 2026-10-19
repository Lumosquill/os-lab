# Lab 2 提示词（prompt.md）

2411570-何美璇：

````
[PROMPT]
**任务**：修改文件 kern/mm/pmm.c，在标有 "//lab2 function1" 的位置实现物理内存初始化功能（page_init）：
根据设备树提供的物理内存信息，建立可管理物理页的 Page 描述符布局，并把排除 OpenSBI、内核镜像和
描述符数组所占空间之后的完整空闲页面交给当前 pmm_manager 管理。该功能在 Sv39 分页已经开启之后执行；
pmm_init() 中已有对它的调用，因此它必须定义在 pmm_init() 之前。本任务只负责"探测、描述、移交"，
不负责设备树的解析、物理内存管理器的内部实现，也不负责管理器的选择与切换。

**操作要求**：你必须在项目中修改实际文件，而不是仅展示代码。
- 可以修改：仅限 pmm.c 中标记位置，以及该功能所需的 static 辅助函数、内部宏和模块内静态变量。
- 不得修改：pmm.c 中标记位置之外的现有代码（包括 pmm_init、init_pmm_manager 及其中对管理器的选择、
  init_memmap、pa2page 等已有函数）；pmm.h、memlayout.h、dtb.h 等头文件中已有的接口、结构体和宏定义；
  各物理内存管理器的实现文件；entry.S、Makefile、链接脚本，以及 tools/ 下的测试和评分文件。
- 保留文件中的现有代码和注释。

**输出要求**：直接在标记位置写入完整实现，使用下面 [RELY]、[GUARANTEE]、[SPECIFICATION] 的信息，
以工程真实代码为准，若与示例不一致以当前工程为准，不要引入无关功能。实现完成后必须满足：
1. 在 -Werror 下无警告通过编译；
2. 内核能正常启动，管理器自检 check_alloc_page() 通过（该自检在本函数移交的空闲页上分配和回收）；
3. [SPECIFICATION] 中 Post-Condition 描述的各项状态均成立（可由 tools/test_lab2.c 中的 Page Init 检查验证）；
4. 终端按如下格式输出物理内存范围（三个值依次为内存大小、起始地址、末尾地址，含）：
   physical memory map:
     memory: 0x%016lx, [0x%016lx, 0x%016lx].

[RELY]
// 宏定义
#define PGSIZE                  4096                      // mmu.h
#define PGSHIFT                 12
#define PPN(la)                 (((uintptr_t)(la)) >> PGSHIFT)   // 地址 -> 物理页号（天然向下取整）
#define DRAM_BASE               0x80000000                // libs/riscv.h，DRAM 起始物理地址
#define PHYSICAL_MEMORY_OFFSET  0xFFFFFFFF40000000        // 内核虚拟地址 = 物理地址 + 该偏移
#define PHYSICAL_MEMORY_END     0x88000000                // 框架可管理的物理内存上界（物理地址）
ROUNDUP(a, n) / ROUNDDOWN(a, n)                           // defs.h，按 n 向上/向下对齐
PADDR(kva)                                                // 内核虚拟地址 -> 物理地址，依赖 va_pa_offset；kva 非法时 panic

// 数据结构（memlayout.h）
struct Page {
    int ref;                        // 引用计数
    uint64_t flags;                 // 状态标志
    unsigned int property;          // 空闲块大小（由具体管理器使用）
    list_entry_t page_link;         // 空闲链表链接
};                                  // 一个 Page 描述一个物理页，sizeof 为 40 字节
#define PG_reserved 0               // flags 的第 0 位：1 表示该页为内核保留，不可被分配/释放
SetPageReserved(page) / PageReserved(page)

// 全局变量（pmm.c）
struct Page *pages;                 // Page 描述符数组，pages[i] 对应物理页号 nbase + i
size_t npage = 0;                   // 物理页号的上界
const size_t nbase = DRAM_BASE / PGSIZE;    // 描述符数组第 0 项对应的物理页号
uint64_t va_pa_offset;              // 虚拟地址与物理地址之间的偏移
extern char end[];                  // 链接脚本定义：内核镜像（含 BSS）结束处的虚拟地址

// 可用函数
uint64_t get_memory_base(void);     // 设备树解析出的物理内存起始地址；dtb_init() 已在 kern_init 中执行，未成功解析时为 0
uint64_t get_memory_size(void);     // 设备树解析出的物理内存大小；未成功解析时为 0
static void init_memmap(struct Page *base, size_t n);   // 把从 base 起的 n 个页交给 pmm_manager。
                                    // 契约：n > 0；这 n 个页此前必须处于"保留"状态（管理器会断言），
                                    // 接管后管理器负责把它们初始化为空闲页
struct Page *pa2page(uintptr_t pa); // 物理地址 -> 描述符，等价于 &pages[PPN(pa) - nbase]；越界时 panic
panic(fmt, ...)、cprintf(fmt, ...)、memset(s, c, n)

// 地址关系
// 物理布局（QEMU virt）：[0x80000000, 0x80200000) 为 OpenSBI；[0x80200000, PADDR(end)) 为内核镜像；
// 其后依次是（页对齐的）Page 描述符数组、可分配空闲页，直到设备树报告的内存上界。
// 描述符数组本身位于被它描述的物理内存之中，其虚拟起点是 end 向上按页对齐。

[GUARANTEE]
必须实现的接口：
```c
static void page_init(void);   // kern/mm/pmm.c；由 pmm_init() 调用，必须定义在 pmm_init() 之前
```
你可以根据需要添加：
- 辅助函数（建议设为 static，仅供本模块内部调用，不改变对外接口）
- 内部使用的宏定义
- 模块内部的静态变量

[SPECIFICATION]
## page_init
**Pre-Condition**:
- dtb_init() 已执行，设备树内存信息可通过查询接口获得；
- 分页已开启，内核可以通过虚拟地址访问内核镜像及其后的物理内存；
- pmm_manager 已选定；pages、npage 尚未初始化。

**Post-Condition**:
- 虚拟地址与物理地址的转换关系已建立，此后的地址转换结果正确；
- npage 表示设备树所报告的物理内存上界对应的物理页号；
- pages 是页对齐的 Page 描述符数组，位于内核镜像之后，为 [nbase, npage) 内的每个物理页各有一个描述符，
  各描述符的初始状态确定，不依赖内存的原有内容；
- 内核镜像（包括镜像末尾所在的页）、OpenSBI 以及描述符数组自身占用的页均保持"保留"状态，没有交给管理器；
- 管理器只接管一段连续、页对齐、完整的空闲页，起点在描述符数组之后，终点不超过框架可管理的上界；
  交给管理器的空闲页数大于 0，且不超过受管理的总页数；
- 已按输出要求输出物理内存范围。

  **Case 1（正常）**:
  - 设备树信息有效，且描述符数组之后到可管理上界之间至少有一个完整页：建立描述符布局，把这段完整空闲页
    一次性交给管理器，并输出物理内存范围。

  **Case 2（设备树信息无效）**:
  - 内存大小或基址缺失，或内存上界不高于描述符数组所假设的起始页号，或内存基址不符合框架对页号起点的假设：
    通过 panic 终止初始化。此时不向管理器注册任何空闲页，不对管理器的状态产生任何影响。

  **Case 3（内存超出可管理上界）**:
  - 设备树报告的内存上界高于框架可管理的上界：超出的部分不交给管理器；物理页号上界仍按设备树的真实上界给出，
    超出部分的描述符存在并保持"保留"状态。

  **Case 4（没有完整的空闲页）**:
  - 描述符数组之后到可管理上界之间不足一个完整页：不调用 init_memmap，函数正常返回，
    不对管理器的状态产生任何影响。

  **Case 5（内存上界未按页对齐）**:
  - 设备树报告的内存上界不是页的整数倍：最后不足一页的部分不交给管理器，其余的完整页都必须交出，
    不得遗漏最后一段完整页。

**Requirements**:
- 必须先建立虚实转换关系，再做任何依赖它的地址转换；
- 虚拟地址、物理地址、物理页号和 Page 描述符地址在代码和注释中必须区分清楚；
- 空闲区间的起点向上对齐、终点向下对齐，保证交给管理器的都是完整的页；
- 必须满足 init_memmap 的契约（n > 0，页此前处于保留状态）；
- 除建立上述全局状态和向管理器移交空闲页之外，不产生其他副作用。
````

2410699-张欣妍：

````
[PROMPT]
**任务**：修改两个文件，完成 Best-Fit（最佳适配）连续物理内存分配算法并接入内核。
1. 在 kern/mm/best_fit_pmm.c 中标有 "//lab2 function2-2" 的位置实现完整的 Best-Fit 分配器，
   包括内部状态、各管理函数，以及 const struct pmm_manager best_fit_pmm_manager 的注册结构
   （该结构已在 kern/mm/best_fit_pmm.h 中 extern 声明）。
   分配策略：从所有 property >= n 的空闲块中选择 property 最小的那个（最合适的块），
   而不是像 First-Fit 那样取链表中第一个够大的块。
2. 在 kern/mm/pmm.c 的 init_pmm_manager() 中把 pmm_manager 由 &default_pmm_manager
   改为 &best_fit_pmm_manager，使内核运行和 check_alloc_page() 检查的都是 Best-Fit。
本任务只做“一种新的分配策略”，不改变 pmm_manager 这套机制，也不实现 Buddy System / SLUB。

**操作要求**：你必须在项目中修改实际文件，而不是仅展示代码。
- 可以修改：kern/mm/best_fit_pmm.c 的标记位置（标记之后、两个测试函数之前），
  以及文件末尾用于注册 best_fit_pmm_manager 的代码；kern/mm/pmm.c 中 init_pmm_manager()
  里那一条给 pmm_manager 赋值的语句及紧邻的一行注释。
- 不得修改：kern/mm/best_fit_pmm.c 中的 basic_check() 与 best_fit_check()（框架测试，逐字保留，
  连注释和 #ifdef ucore_test 也要保留）；kern/mm/default_pmm.c；pmm.h、best_fit_pmm.h、
  memlayout.h、list.h 等头文件；page_init()、entry.S、Makefile、链接脚本，以及 tools/ 下的评分文件。
- 保留文件中已有的代码与注释；把标记里的 "YOUR CODE" 换成我的学号 2410699。

**输出要求**：直接在标记位置写入完整实现。实现完成后必须满足：
1. 在 -Wall -Werror 下无告警通过编译（Makefile 用 -Wno-unused 关掉了"未使用变量/函数"这一类告警，其余告警都会被当成错误）；
2. 内核能正常启动，check_alloc_page() 中 best_fit_check() 的全部 assert 都能通过，
   最后打印 check_alloc_page() succeeded!；
3. make grade 输出 [Test 2] Best-Fit: PASS；
4. 终端打印 memory management: best_fit_pmm_manager。

[RELY]
// kern/mm/pmm.h —— 机制：内核只通过这组函数指针调用具体策略
struct pmm_manager {
    const char *name;
    void (*init)(void);
    void (*init_memmap)(struct Page *base, size_t n);
    struct Page *(*alloc_pages)(size_t n);      // 分配连续的 n 个页
    void (*free_pages)(struct Page *base, size_t n);
    size_t (*nr_free_pages)(void);
    void (*check)(void);
};
extern const struct pmm_manager *pmm_manager;
struct Page *alloc_pages(size_t n);  void free_pages(struct Page *base, size_t n);
size_t nr_free_pages(void);          // 以上三个是上层入口，内部转发给 pmm_manager
#define alloc_page()  alloc_pages(1)
#define free_page(page) free_pages(page, 1)

// kern/mm/memlayout.h —— 页描述符与空闲区
struct Page {
    int ref;                        // 引用计数
    uint64_t flags;                 // 位 0: PG_reserved；位 1: PG_property
    unsigned int property;          // 本节含义：只有空闲块的头页才非 0，值为该块的页数
    list_entry_t page_link;         // 挂到空闲链表上的链节点
};
SetPageProperty(p) / ClearPageProperty(p) / PageProperty(p)
SetPageReserved(p) / ClearPageReserved(p) / PageReserved(p)
#define le2page(le, member) to_struct((le), struct Page, member)   // 链节点 -> Page 首地址
typedef struct { list_entry_t free_list; unsigned int nr_free; } free_area_t;
set_page_ref(page, val) / page_ref(page)   // 引用计数：ref 为 0 表示该页当前没有使用者

// libs/list.h —— 双向链表，表头自身也当作一个节点
list_init(le) / list_empty(le) / list_next(le) / list_prev(le)
list_add(le, elm)            // 把 elm 插到 le 之后
list_add_before(le, elm)     // 把 elm 插到 le 之前
list_del(le)                 // 把 le 从链表中摘下（不重新初始化）

// 现成参考实现：kern/mm/default_pmm.c（First-Fit），可对照它的 init / init_memmap /
// free_pages / 空块合并写法；唯一需要改的是 alloc_pages 的选块策略。

// 框架测试的两个硬性约定（写在 best_fit_pmm.c 里，必须逐字保留、不能改）
// basic_check() 与 best_fit_check() 直接引用了 free_list 和 nr_free 这两个标识符，并且：
//   - 会遍历 free_list，要求每个块头页满足 PageProperty(p)，且各块 property 之和 == nr_free_pages()；
//   - 会通过 free_pages() 造出 [占用][空闲 2 页][占用][空闲 1 页] 这样的布局，
//     再断言 alloc_pages(4) == NULL、alloc_pages(1) 返回的是那个“1 页的空闲块”（地址 p0+4），
//     alloc_pages(2) 取到那个“2 页的空闲块”；
//   - 会把 free_list 与 nr_free 备份后清空、再恢复，并检查恢复后的链表与原来完全一致。
// 因此：空闲链表的头页必须满足“property = 块大小 且 PageProperty 置位”；
//       合并只能发生在物理地址相邻的空闲块之间；链表必须按物理地址升序，prev/next 才是相邻块。

// kern/mm/pmm.c
static void init_pmm_manager(void) {
    pmm_manager = &default_pmm_manager;      // ← 只改这一行（及紧邻注释）
    cprintf("memory management: %s\n", pmm_manager->name);
    pmm_manager->init();
}

[GUARANTEE]
必须提供的符号（best_fit_pmm.c 内部函数都用 static）：
```c
static void best_fit_init(void);
static void best_fit_init_memmap(struct Page *base, size_t n);
static struct Page *best_fit_alloc_pages(size_t n);
static void best_fit_free_pages(struct Page *base, size_t n);
static size_t best_fit_nr_free_pages(void);
const struct pmm_manager best_fit_pmm_manager;   // pmm.h 之外另需 extern 声明（best_fit_pmm.h 已有）
```
你可以根据需要添加内部静态变量与辅助函数，但不得改动两个测试函数与对外接口。

[SPECIFICATION]
## best_fit_init_memmap(base, n)
**Pre-Condition**: n > 0；[base, base+n) 这些页此前处于“保留”状态（best_fit_init_memmap 会断言 PageReserved）。
**Post-Condition**:
- 这 n 个页的 flags 与 property 被清零、ref 置 0；
- base 成为该空闲块的头页：base->property = n 且 PageProperty(base) 为真，其他页的 property 为 0；
- 这一整块以“一个节点”的形式挂在空闲链表上，链表仍按物理地址升序；
- nr_free 增加 n。

## best_fit_alloc_pages(n)
**Pre-Condition**: n > 0；空闲链表与 nr_free 处于一致状态（各块 property 之和 == nr_free）。
**Post-Condition**:
- n > nr_free 或无块满足 property >= n 时：返回 NULL，链表与 nr_free 不变；
- 否则返回一个空闲块的头页地址，块大小 >= n；被返回的块从链表中摘下，头页的 PageProperty 被清除、
  property 为 0（表示已被占用），nr_free 减少 n；
- 若被选中的块比 n 大，剩余部分 (page+n, property-n) 作为新的空闲块头页放回链表原来的位置，
  保持物理地址升序。

## best_fit_free_pages(base, n)
**Pre-Condition**: n > 0；[base, base+n) 是一段已分配出去的连续页（未保留、且头页 PageProperty 为假）。
**Post-Condition**:
- 这 n 个页的 flags 清零、ref 置 0，base 成为空闲块头页（property = n，PageProperty 置位）；
- 该块插入空闲链表并保持物理地址升序；若前一个空闲块（prev）恰好与本块物理相邻则合并，
  若后一个空闲块（next）也相邻则继续合并，合并后只保留一个头页的 property/PageProperty；
- nr_free 增加 n。

## 选块策略
- 只能是“所有 property >= n 的块里 property 最小的一个”，不能遇到第一个够大的块就 break；
- 空闲块大小相同时，取链表中靠前的那个，保证行为确定、可复现。

**Case 1（恰好适配）**: 存在 property == n 的块：直接整块摘下，不产生剩余分片。
**Case 2（过大）**: 最小的够用块 property > n：切出前 n 页返回，剩余部分作为新块留在链表原位置。
**Case 3（相邻合并）**: 释放时相邻空闲块必须合并成更大块，避免同一段内存被切成多个碎片。
**Case 4（空链表）**: 链表为空时 alloc 返回 NULL；free 后链表只含一个节点，表头前后指针自洽。

**Requirements**:
- 两个测试函数 basic_check() / best_fit_check() 不能修改，因此实现必须满足它们对 free_list、nr_free、
  property 与 PageProperty 的上述约定；
- 必须定义 free_list 与 nr_free 这两个标识符（沿用 First-Fit 的做法：一个 static free_area_t
  加两个同名宏），否则测试函数编译不过；
- 不允许改 pmm_manager 结构体，只能提供一个新的 const struct pmm_manager 实例；
- 不允许为了通过测试而修改、关闭或绕过 assert。
````
