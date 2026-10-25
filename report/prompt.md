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



### 2411433-李烁菲

## 提示词 A：生成 Buddy System 分配模块并接入内核

````markdown
[PROMPT]
**任务**：在 ucore 物理内存管理框架（kern/mm/）中新增 Buddy System（伙伴系统）连续物理页分配模块，并接入内核。涉及三个文件：
- 新建 `kern/mm/buddy_pmm.c`：实现 Buddy 分配器及其 `pmm_manager` 注册结构 `buddy_pmm_manager`；
- 新建 `kern/mm/buddy_pmm.h`：声明 `extern const struct pmm_manager buddy_pmm_manager;`，写法与现有 `best_fit_pmm.h`、`default_pmm.h` 一致；
- 修改 `kern/mm/pmm.c`：增加 `#include <buddy_pmm.h>`，并在 `init_pmm_manager()` 中把 `pmm_manager` 切换为 `&buddy_pmm_manager`。

**操作要求**：你必须在项目中创建和修改实际文件，而不是只展示代码。`pmm.c` 中现有代码一律保留（包括 default / best_fit 的 include），只改 `init_pmm_manager()` 里的赋值；不得修改 `default_pmm.c`、`best_fit_pmm.c`、`memlayout.h`、`pmm.h`、`page_init()`。完成后检查整个内核可以编译；若 Makefile 不会自动收集 `kern/mm/` 下的新文件，再做最小必要的修改。

**输出要求**：直接进行文件操作，使用下面 [RELY]、[GUARANTEE]、[SPECIFICATION] 中的信息，以工程真实代码为准，不要引入无关功能（锁、多核、NUMA、统计信息等）。

[RELY]
// ---- memlayout.h ----
struct Page {
    int ref;                        // 引用计数
    uint64_t flags;                 // 状态位
    unsigned int property;          // 空闲块头页上记录块大小
    list_entry_t page_link;         // 空闲链表节点
};
#define PG_reserved 0   // 1: 内核保留页，不得参与分配/释放
#define PG_property 1   // 1: 该页是一个空闲块的头页；0: 非头页，或该块已被分配
#define SetPageReserved(page) / ClearPageReserved(page) / PageReserved(page)
#define SetPageProperty(page) / ClearPageProperty(page) / PageProperty(page)
#define le2page(le, member)  to_struct((le), struct Page, member)

// ---- list.h ----
list_init(elm); list_add(listelm, elm);  /* 插到 listelm 之后 */
list_add_before(listelm, elm); list_del(listelm);
list_empty(list); list_next(listelm); list_prev(listelm);

// ---- pmm.h ----
struct pmm_manager {
    const char *name;
    void (*init)(void);
    void (*init_memmap)(struct Page *base, size_t n);
    struct Page *(*alloc_pages)(size_t n);
    void (*free_pages)(struct Page *base, size_t n);
    size_t (*nr_free_pages)(void);
    void (*check)(void);
};
extern struct Page *pages;      // Page 描述符数组，pages[i] 描述第 i 个被管理的物理页
extern size_t npage;            // 物理页号上界；数组元素个数为 npage - nbase
extern const size_t nbase;      // DRAM 起始页号
static inline ppn_t page2ppn(struct Page *page);
static inline uintptr_t page2pa(struct Page *page);
static inline void set_page_ref(struct Page *page, int val);
static inline int page_ref(struct Page *page);
// 另有 assert()、cprintf()、PGSIZE

// ---- 现有框架行为（pmm.c 的 page_init）----
// page_init() 只调用一次 init_memmap(pa2page(free_begin), n)，
// 交来的区间起点不保证对齐，n 也不保证是 2 的幂；该区间之外的页
// （内核镜像、OpenSBI、Page 数组自身所在页）保持 PG_reserved，不属于任何空闲块。
// 本实验中管理的页数不超过 2^15。

[GUARANTEE]
必须在 buddy_pmm.c 中实现的接口与全局状态（名称固定，后续测试代码会直接引用）：
#define BUDDY_MAX_ORDER 15                          // 最大块 = 2^15 页
static list_entry_t buddy_free_list[BUDDY_MAX_ORDER + 1];  // 第 k 条链表挂大小为 2^k 页的空闲块
static size_t buddy_nr_free;                        // 空闲页总数

static void buddy_init(void);
static void buddy_init_memmap(struct Page *base, size_t n);
static struct Page *buddy_alloc_pages(size_t n);
static void buddy_free_pages(struct Page *base, size_t n);
static size_t buddy_nr_free_pages(void);
static void buddy_check(void);   // 先写成占位：只做"各链表块大小之和 == buddy_nr_free"的断言，之后会被测试提示词替换

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init, .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages, .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages, .check = buddy_check,
};
可以按需添加辅助函数（建议 static）、内部宏和静态变量，例如"向上取整到 2 的幂的阶数""计算伙伴页""按地址有序插入链表"。不得改变上面这些名称与含义。

[SPECIFICATION]
## 全局约定（后续测试依赖，必须遵守）
- 页下标 `idx = page - pages`。阶为 k 的块含 2^k 个连续页，其头页的 `idx` 必须是 2^k 的整数倍（按**全局下标**对齐，而不是相对于某次 init_memmap 的起点）。块 [idx, idx+2^k) 的伙伴是下标 `idx ^ (1<<k)` 处的同阶块。
- 空闲块只有头页的 `property` 非 0，值为**块包含的页数**（2^k），且头页置 `PG_property`；非头页 `property == 0` 且不置 `PG_property`。
- 每条 `buddy_free_list[k]` 内的块按头页地址**升序**排列，使分配结果确定可复现。
- 已分配出去的页：`flags == 0`（不是 reserved、也没有 property），`ref == 0`，头页 `property == 0`。
- 一次请求 n 页，实际占用 `2^ceil(log2 n)` 页（下称"取整后大小"）；`buddy_nr_free` 与 `nr_free_pages()` 按取整后大小增减。

## buddy_init
**Pre-Condition**：内核启动阶段、尚未交付任何空闲内存。
**Post-Condition**：所有 `buddy_free_list[0..BUDDY_MAX_ORDER]` 均为空链表，`buddy_nr_free == 0`。

## buddy_init_memmap
**Pre-Condition**：`n > 0`；`[base, base+n)` 内每一页均为 `PG_reserved`（用 assert 检查）。
**Post-Condition**：这 n 页被完整接管为空闲内存：每页 `flags/property/ref` 清零；区间被分解成若干个满足对齐规则的块挂入对应链表；`buddy_nr_free` 增加 n；n 页中没有任何页遗漏或重复归属。
  **Case 1（起点未对齐或 n 不是 2 的幂）**：从区间起点开始，每次取"满足起点按块大小对齐、且不超出区间末尾、且阶不超过 BUDDY_MAX_ORDER"的最大块，登记后前进，直到区间用完。
  **Case 2（起点已对齐且 n 恰为 2 的幂且不超过上限）**：整段成为一个块。
**Requirements**：不需要在此函数中做相邻块合并。

## buddy_alloc_pages
**Pre-Condition**：`n > 0`（assert）。
**Post-Condition**：成功时返回一个取整后大小的块的头页，该块已从空闲链表摘除，块内所有页满足"已分配"约定，`buddy_nr_free` 减少取整后大小；失败时返回 NULL 且不改变任何状态。
  **Case 1（取整后大小超过 2^BUDDY_MAX_ORDER，或超过 `buddy_nr_free`）**：返回 NULL。
  **Case 2（存在阶恰好合适的空闲块）**：取该链表中地址最小的块。
  **Case 3（只有更大阶的块）**：从阶数大于等于所需阶的最小的非空链表中取地址最小的块，反复对半拆分：低地址一半继续用于分配，高地址一半作为新的空闲块挂入低一阶链表，直到阶数等于所需阶；返回低地址那一块。
  **Case 4（没有足够大的单个块）**：返回 NULL（即使 `buddy_nr_free` 足够，碎片导致无法满足也属于此情况）。

## buddy_free_pages
**Pre-Condition**：`n > 0`；`base` 是此前 `buddy_alloc_pages(n)`（同一个 n）返回的地址；块内各页不是 reserved 也不是 property（assert 检查），且 `base` 满足取整后大小对应的对齐。
**Post-Condition**：该块回到空闲状态，`buddy_nr_free` 增加取整后大小；它与所有可合并的伙伴被合并成尽可能大的块，且最终链表中每个块仍满足全局约定。
  **Case 1（伙伴是同阶空闲块）**：判断条件为：伙伴头页下标在 pages 数组范围内、该页 `PageProperty` 为真、且其 `property` 等于当前块页数。满足则把伙伴从其链表摘除，两块合并为高一阶的块（头页取低地址者，被吸收的头页要清除 `PG_property` 并将 `property` 置 0），再继续尝试更高阶，直到无法合并或达到 BUDDY_MAX_ORDER。
  **Case 2（伙伴不空闲、阶不同，或相邻但不是伙伴）**：不合并，仅把当前块按地址升序挂入对应链表。
**Requirements**：地址相邻但不互为伙伴的块不得合并；与 `PG_reserved` 页相邻时不得越界合并。

## buddy_nr_free_pages
返回 `buddy_nr_free`。

## pmm.c 的修改
`init_pmm_manager()` 中改为选中 `buddy_pmm_manager`，其余逻辑（打印名称、调用 `init()`）保持不变。
````

---

## 提示词 B：为 Buddy System 生成测试函数

````markdown
[PROMPT]
**任务**：修改文件 `kern/mm/buddy_pmm.c`，为已实现的 Buddy System 分配器实现完整的测试函数 `buddy_check()`（替换其中的占位实现），并按需添加 static 的测试辅助函数。

**操作要求**：你必须在项目中修改实际文件，而不是只展示代码。只替换 `buddy_check` 并新增测试辅助函数，不得修改分配器本身的逻辑（`buddy_init / init_memmap / alloc_pages / free_pages` 等）。如果测试失败，先判断是测试写错还是分配器有 bug：测试写错就修测试；确认是分配器 bug 时，不要通过削弱断言来"让它通过"，而是在回复里明确指出 bug 位置和原因，并说明你的修复。

**输出要求**：直接进行文件操作，使用下面 [RELY]、[GUARANTEE]、[SPECIFICATION] 的信息，以工程真实代码为准。测试通过后内核启动日志应出现 `check_alloc_page() succeeded!`。

[RELY]
// ---- 被测模块的约定（来自 buddy_pmm.c，名称固定）----
#define BUDDY_MAX_ORDER 15
static list_entry_t buddy_free_list[BUDDY_MAX_ORDER + 1];   // 第 k 条链表挂 2^k 页的空闲块，块内按头页地址升序
static size_t buddy_nr_free;
// 页下标 idx = page - pages；阶 k 的块头页 idx 必为 2^k 的倍数；伙伴下标 = idx ^ (1<<k)
// 空闲块：仅头页 PageProperty 为真，头页 property == 块页数；非头页 property == 0
// 已分配页：flags == 0，ref == 0，头页 property == 0
// 请求 n 页实际占 2^ceil(log2 n) 页；nr_free_pages() 按取整后大小变化
// 分配：取不小于所需阶的最小非空链表的最低地址块，逐级对半拆分，低地址半块返回、高地址半块入低一阶链表
// 释放：free_pages(base, n) 的 n 必须与分配时相同

// ---- 框架接口（pmm.h / memlayout.h / list.h）----
struct Page *alloc_pages(size_t n);  void free_pages(struct Page *base, size_t n);
size_t nr_free_pages(void);  #define alloc_page() alloc_pages(1)  #define free_page(p) free_pages(p, 1)
extern struct Page *pages;   page_ref(p)  PageProperty(p)  PageReserved(p)
list_entry_t 的 list_init / list_empty / list_next / list_prev，le2page(le, page_link)，assert()，cprintf()

[GUARANTEE]
必须实现：static void buddy_check(void);
建议新增的 static 辅助函数（可调整名称与数量）：
- 全局不变量检查函数（遍历所有 `buddy_free_list[k]` 校验约定）；
- 构造"受控小池"与恢复现场的函数；
- 记录已分配块并检查两两不重叠的小工具。

[SPECIFICATION]
## buddy_check
**Pre-Condition**：`pmm_init()` 已完成 `page_init()`，分配器中是内核真实的大量空闲内存。
**Post-Condition**：所有断言通过；函数返回后，所有 `buddy_free_list[]`、`buddy_nr_free`、每个页描述符的状态与调用前**逐一相同**，内核后续继续使用的空闲内存不受影响。

  **Case 0（不变量，贯穿始终）**：写一个不变量检查函数，在测试开始、结束以及每个场景之后调用。检查：每条链表里的每个块头页 `PageProperty` 为真、`property == 1<<k`、头页下标是 `1<<k` 的倍数、链表内地址严格升序；所有块 `property` 之和等于 `buddy_nr_free`，也等于 `nr_free_pages()`；块之间互不重叠。
  **Case 1（受控小池）**：为了让后面的场景有**确定**的预期，先构造一个已知形状的 16 页小池，具体做法由你设计，但必须满足两点。第一，小池运行期间链表中只有这 16 页（保存并清空全部链表与计数）。第二，小池必须与真实空闲块**隔离**：从真实分配器取出一个对齐的 32 页块，只把其中前 16 页当作小池放回，后 16 页保持已分配，这样释放时小池的上一级伙伴是"已分配"，绝不会与真实空闲块发生合并（否则会碰到被临时摘走的真实链表而破坏内核状态）。
  **Case 2（取整规则）**：在小池上请求 1、2、3、4、5 页，验证实际占用页数分别为 1、2、4、4、8（用 `nr_free_pages()` 的变化衡量）；返回页均不是 reserved、没有 property、`ref == 0`；返回地址满足取整后大小的对齐；释放时用相同 n，`nr_free_pages()` 完全恢复。
  **Case 3（拆分顺序确定）**：在 16 页小池上连续 `alloc_page()` 16 次，应依次得到 `p, p+1, ..., p+15`（pool 起点为 p，请你自行推导并断言），第 17 次返回 NULL，此时 `nr_free_pages() == 0`；各页互不相同。
  **Case 4（伙伴合并）**：取出 `p` 与 `p+1` 后以任意顺序释放，应合并回完整的 16 页单块（链表中只剩一个阶 4 的块，头页为 `p`，`property == 16`，被吸收页的 `PageProperty` 为假）。
  **Case 5（相邻但不是伙伴，不得合并）**：取出 `p..p+3` 共 4 个单页，只释放 `p+1` 和 `p+2`（地址相邻但不是伙伴）：阶 0 链表里应有两个独立的 1 页块，且 `alloc_pages(2)` 不得返回 `p+1`；再释放 `p` 和 `p+3` 之后，4 页才应级联合并为阶 2 的一个块，继续释放其余页应回到 16 页单块。
  **Case 6（释放顺序无关与多级级联）**：把 16 个单页全部取出，分别按正序、倒序、奇偶交错的顺序释放，每次最后都应回到完整的 16 页单块，每一步后不变量检查都通过。
  **Case 7（边界与失败路径）**：小池上 `alloc_pages(17)` 返回 NULL 且状态不变；`alloc_pages(16)` 成功，此时 `alloc_page()` 为 NULL，释放后恢复；`alloc_pages((1<<BUDDY_MAX_ORDER) + 1)` 返回 NULL；小池里取出 16 个单页后释放偶数下标的 8 个页：`nr_free_pages() == 8` 但 `alloc_pages(2)` 必须为 NULL（碎片），再释放剩余页之后 `alloc_pages(16)` 应成功。
  **Case 8（块互不重叠与数据状态）**：混合请求 1、3、2、5、1、4 页等多个块，所有返回块两两不重叠；写入每块的页内容（可通过 `page2pa` 与 `va_pa_offset` 转为虚拟地址后写入特征字节）后再读回验证无串扰（可选）；全部释放后回到完整的 16 页单块。
  **Case 9（恢复现场）**：先把小池整块取回（`alloc_pages(16)`，此时这 16 页重新回到"已分配"状态），再恢复保存的链表与计数，最后把当初取出的 32 页块 `free_pages` 回去，使其与真实空闲块正常合并；末尾再调用一次不变量检查，并核对 `nr_free_pages()` 与测试开始时相等。

**Requirements**：
- 只使用 `alloc_pages / free_pages / nr_free_pages` 以及 buddy_pmm.c 内部的链表和计数变量，不依赖其它模块。
- 保存和恢复链表时，需要保存的是链表头本身（`list_entry_t` 数组的拷贝），并在恢复前保证链表头地址不变，不能新建副本替代原链表头。
- 每个 Case 的预期值写成显式 `assert`，失败时能从断言位置看出是哪个场景出错。
- 测试结束时打印一行总结信息（如 `buddy_check() succeeded!`）。
````



## 提示词 C：生成两层 SLUB 分配模块并接入内核

````markdown
[PROMPT]
**任务**：在 ucore 内核中实现一个简化版的两层 SLUB 内存对象分配器，使内核可以分配任意大小的内存。
- 第一层：以页为单位的物理页分配，直接使用现有 `alloc_pages()/free_pages()`，不得修改或替换 `pmm_manager`。
- 第二层：在第一层之上的任意大小分配 `kmalloc/kfree`——按大小分级的 cache，每个 cache 管理若干 slab（每个 slab 是一页，页首放头部、其后切成等大对象），小对象从 slab 中分配；大于最大分级的请求直接按页向第一层申请。

涉及文件：
- 新建 `kern/mm/slub.c`、`kern/mm/slub.h`：实现分配器；
- 修改 `kern/mm/memlayout.h`：仅新增两个页标志位及其宏（见 [RELY]）；
- 修改 `kern/mm/pmm.c`：在 `pmm_init()` 里 `check_alloc_page()` 之后，依次调用 `slub_init()` 和 `slub_check()`，并 include `<slub.h>`。

**操作要求**：你必须在项目中创建和修改实际文件，而不是只展示代码。除上述三处外不得改动其他现有文件和现有代码，不得修改任何物理内存管理器（default / best_fit / buddy）。完成后检查内核可以编译；若 Makefile 不会自动收集新文件，再做最小必要的修改。

**输出要求**：直接进行文件操作，使用下面 [RELY]、[GUARANTEE]、[SPECIFICATION] 的信息，以工程真实代码为准，不要引入无关功能（锁、多核、着色、对象构造/析构回调等）。`slub_check()` 先写成占位函数，只打印一行信息，后续会被测试提示词替换。

[RELY]
// ---- memlayout.h ----
struct Page { int ref; uint64_t flags; unsigned int property; list_entry_t page_link; };
#define PG_reserved 0
#define PG_property 1
// 需要你新增（仿照现有宏的写法）：
//   #define PG_slab  2   // 该页是一个 slab 页
//   #define PG_large 3   // 该页是大块直通分配的头页，其 Page 的 property 临时记录页数
//   SetPageSlab/ClearPageSlab/PageSlab 与 SetPageLarge/ClearPageLarge/PageLarge

// ---- pmm.h ----
struct Page *alloc_pages(size_t n);    // 第一层：失败返回 NULL
void free_pages(struct Page *base, size_t n);
size_t nr_free_pages(void);
#define alloc_page() alloc_pages(1)    #define free_page(page) free_pages(page, 1)
extern struct Page *pages;     extern uint64_t va_pa_offset;
static inline uintptr_t page2pa(struct Page *page);
static inline struct Page *pa2page(uintptr_t pa);   // 非法 pa 会 panic
#define PADDR(kva)   // 内核虚拟地址 -> 物理地址，kva < KERNBASE 时 panic
// 注意：框架中 KADDR 被注释掉了，Page -> 内核虚拟地址请用： (void *)(page2pa(page) + va_pa_offset)
// 另有 PGSIZE(4096)、ROUNDUP/ROUNDDOWN、memset（<string.h>）、assert()、panic()、cprintf()

// ---- list.h ----
list_init / list_add / list_add_before / list_del / list_empty / list_next / list_prev，le2page(le, member)

// ---- 约束 ----
// 物理页分配器可能是 First-Fit / Best-Fit / Buddy 中任意一种，
// SLUB 只能依赖 alloc_pages/free_pages 的公共语义：
// free_pages(base, n) 的 base 与 n 必须和分配时一致，且释放前该块各页的 flags 中不应有 reserved/property 位。

[GUARANTEE]
必须实现的接口（slub.h 中声明）：
void   slub_init(void);
void  *kmalloc(size_t size);
void   kfree(void *ptr);
size_t slub_class_size(size_t size);          // 给测试用的查询接口
size_t slub_objs_per_slab(size_t class_size); // 给测试用的查询接口
void   slub_check(void);                      // 先写占位

可以添加 static 辅助函数、内部结构体（如 `struct slab`、`struct kmem_cache`）和静态变量。cache 描述符必须是静态分配的，不得依赖 `kmalloc` 自身。

[SPECIFICATION]
## 设计约定（后续测试依赖，必须遵守）
- 小对象的大小分级固定为 16、32、64、128、256、512、1024、2048 字节共 8 级，每级一个 cache。请求 size 使用满足 `class >= size` 的最小一级。
- 一个 slab = 一个物理页。页首是 slab 头部（至少包含：所属 cache、空闲对象链表头、已分配对象数、链表节点），头部大小向上取整到 16 的倍数；其后从低到高紧密切成等大对象；每个空闲对象的前 8 字节用来存放链到下一个空闲对象的指针；所有对象不跨页，且至少 16 字节对齐。
- 每个 cache 维护 partial 链表（有空闲对象的 slab）和 full 链表（已满的 slab）。
- 空闲对象链表是 **LIFO**：刚释放的对象是该 slab 下次分配的第一个对象；新 slab 初始化时，空闲链表按对象地址**升序**排列。
- slab 的页描述符置 `PG_slab`。大块直通分配的头页描述符置 `PG_large`，并把 `property` 临时用作页数。

## slub_init
**Pre-Condition**：`pmm_init()` 已完成 `page_init()` 与物理内存检查，`alloc_pages` 可用。
**Post-Condition**：8 个 cache 全部初始化为"无 slab"状态，partial/full 链表为空；此函数不分配任何页。

## kmalloc
**Pre-Condition**：`slub_init()` 已调用。
**Post-Condition**：成功时返回内核虚拟地址指针，指向至少 size 字节可用且不与任何其他存活分配重叠的内存；失败返回 NULL。不要求对内存清零。
  **Case 1（size == 0）**：返回 NULL。
  **Case 2（size <= 2048）**：选中对应 cache。若 partial 链表非空，取其第一个 slab，从空闲对象链表头部弹出一个对象；若该 slab 因此没有空闲对象，把它从 partial 移到 full。若 partial 为空，则 `alloc_page()` 获得新页（失败返回 NULL），按约定初始化为 slab（置 `PG_slab`、建立空闲链表、已分配数置 0），挂入 partial，再按上述方式分配。
  **Case 3（size > 2048）**：按 `n = ROUNDUP(size, PGSIZE) / PGSIZE` 向第一层申请 n 页（失败返回 NULL），在头页描述符上置 `PG_large` 且 `property = n`，返回该块起始页对应的内核虚拟地址。需要防止 `size` 极大时上取整溢出，并对超过 `nr_free_pages()` 的请求直接返回 NULL。

## kfree
**Pre-Condition**：`ptr` 为 NULL，或者是此前 `kmalloc` 返回且尚未释放的指针。
**Post-Condition**：对象/大块被归还；其他存活分配的内容不受影响。
  **Case 1（ptr == NULL）**：不做任何事。
  **Case 2（ptr 属于 slab）**：通过 `PADDR` 和 `pa2page` 找到所在页描述符，判定 `PG_slab` 为真后，在页首取得 slab 头部。校验指针落在头部之后、对象区之内，且相对对象区起点按对象大小对齐，否则 panic。把对象压回空闲链表头部，已分配数减 1。若 slab 之前在 full 链表，则移回 partial。若已分配数降为 0，把 slab 从链表中摘除，清除 `PG_slab`，用 `free_page` 立即还给第一层（简化策略：空 slab 一律立刻回收）。
  **Case 3（ptr 是大块直通）**：`ptr` 对应的页描述符带 `PG_large`，且 `ptr` 必须页对齐。取出 `property` 作为页数 n，清除 `PG_large` 并将 `property` 置 0，再 `free_pages(head, n)`。
  **Case 4（其他）**：页描述符既不是 slab 也不是大块头页（含指向大块内部非头页的指针），视为非法指针，panic。

## slub_class_size
`size == 0` 或 `size > 2048` 时返回 0；否则返回实际使用的分级大小（16~2048）。

## slub_objs_per_slab
对合法的分级大小，返回单个 slab 能容纳的对象数（页大小减去向上取整后的头部，再整除对象大小）；非法分级返回 0。

## slub_check
占位：打印 `slub_check() placeholder` 一行。

**Requirements**：全程单线程，无需加锁；所有内部结构使用静态内存或 slab 页本身，不得递归调用 `kmalloc`。
````

---

## 提示词 D：为 SLUB 分配器生成测试函数

````markdown
[PROMPT]
**任务**：修改文件 `kern/mm/slub.c`，为已实现的两层 SLUB 分配器实现完整的测试函数 `slub_check()`（替换占位实现），并按需添加 static 测试辅助函数。

**操作要求**：你必须在项目中修改实际文件，而不是只展示代码。只替换 `slub_check` 并新增测试辅助函数，不得修改分配器自身逻辑。如果测试失败，先判断是测试写错还是分配器有 bug：测试写错就修测试；确认是分配器 bug 时，不要通过削弱断言来"让它通过"，而是在回复中指出 bug 位置和原因并说明修复。

**输出要求**：直接进行文件操作，使用下面 [RELY]、[GUARANTEE]、[SPECIFICATION] 的信息，以工程真实代码为准。测试通过后内核启动日志应出现 `slub_check() succeeded!`。

[RELY]
// ---- 被测接口（slub.h）----
void *kmalloc(size_t size);   void kfree(void *ptr);
size_t slub_class_size(size_t size);          // 0 表示 size==0 或大块直通；否则为 16..2048 的分级大小
size_t slub_objs_per_slab(size_t class_size);
// ---- 对外可观察的约定 ----
// 小对象分级：16/32/64/128/256/512/1024/2048；size 取满足 class>=size 的最小一级
// 一个 slab 恰占一页；对象不跨页，至少 16 字节对齐；新 slab 空闲对象按地址升序；
// 释放采用 LIFO：刚释放的对象是同 slab 下次分配的第一个
// 空 slab 立即归还第一层；大块（size>2048）按 ROUNDUP(size,PGSIZE)/PGSIZE 页向 alloc_pages 申请
// kmalloc(0) 返回 NULL；kfree(NULL) 无动作；kmalloc 不清零
// 单页分配会使 nr_free_pages() 恰好减 1（页分配器对 1 页不会多占）
// ---- 框架接口 ----
size_t nr_free_pages(void);  PGSIZE  memset  assert()  cprintf()  extern uint64_t va_pa_offset  KERNBASE

[GUARANTEE]
必须实现：
void slub_check(void);

建议新增的 static 辅助函数：
- 用特征字节填充一块内存与校验；
- 简单的确定性伪随机数生成（如线性同余），不依赖外部库；
- 一个记录"指针、大小、填充值"的静态表，用于批量分配、释放与逐项校验。

[SPECIFICATION]
## slub_check
**Pre-Condition**：`slub_init()` 已调用，内核处于启动阶段。
**Post-Condition**：所有断言通过；函数返回时所有分配均已释放，`nr_free_pages()` 与测试开始时**完全相等**。

  **Case 1（分级选择）**：对 1、15、16、17、32、33、…、1024、1025、2048 这些边界调用 `slub_class_size`，验证分级正确；`0` 与 `2049` 返回 0。对每个分级检查 `slub_objs_per_slab` 大于 0 且乘以分级大小不超过 `PGSIZE`。
  **Case 2（基本分配与内容保持）**：对每个分级各分配若干对象，每个对象整段填充不同特征字节，**全部分配完之后**再逐个读回校验，以发现对象重叠、头部被覆盖等错误；校验指针非 NULL、至少 16 字节对齐、处于内核虚拟地址范围（不小于 `KERNBASE`）、对象不跨越页边界。
  **Case 3（slab 增长与回收的页账目）**：针对某个分级（至少测 16、256、2048 三种），记录当前 `nr_free_pages()`，分配 `slub_objs_per_slab(c)` 个对象，应恰好占用 1 页；再多分配 1 个应占用第 2 页；释放全部对象后 `nr_free_pages()` 必须恢复为初值（验证空 slab 被及时归还）。再测分配 `3 * objs_per_slab` 个对象占用 3 页的情形。
  **Case 4（LIFO 复用与 full/partial 状态流转）**：先分配 `objs_per_slab` 个对象填满一页（slab 进入 full），释放其中某个对象 `x`（slab 回到 partial），再分配同级对象应得到地址 `x`；对释放后再分配的对象做内容读写不影响其余对象。
  **Case 5（不同释放顺序）**：批量分配后分别用正序、倒序、奇偶交错的顺序释放，之后重新分配同数量对象应都能成功，释放完毕页账目归零。
  **Case 6（大块直通）**：分配 2049、4096、4097、`3 * PGSIZE`、`5 * PGSIZE + 1` 字节，验证指针页对齐、整段可写且数据可读回，释放后 `nr_free_pages()` 恢复；大块与小对象交错分配、释放互不影响。
  **Case 7（失败路径与无效参数）**：`kmalloc(0) == NULL`；`kfree(NULL)` 不崩溃；请求大于 `nr_free_pages() * PGSIZE` 的大块返回 NULL 并且不改变 `nr_free_pages()`；特别大的 size（如 `(size_t)-1`、`(size_t)-1 - 100`）返回 NULL 而不是溢出成功。
  **Case 8（混合随机压力）**：用确定性伪随机数产生数百次混合操作（随机选取分配或释放；大小随机取自 1 到约 3 个页），每个存活分配记录指针、大小和填充值；每次释放前校验内容完整；操作结束后释放全部存活对象，并检查 `nr_free_pages()` 恢复初值。

**Requirements**：
- 测试只使用公开接口和上面 [RELY] 列出的查询接口，不读取 slab 内部结构，这样对不同的内部实现都有效。
- 每个 Case 的预期值写成显式 `assert`，便于从断言位置判断出错的场景。
- 保存的指针表使用静态数组，不能用 `kmalloc` 自身存放测试记录。
- 结束时打印 `slub_check() succeeded!`。
````

