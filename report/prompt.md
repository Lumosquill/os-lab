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
