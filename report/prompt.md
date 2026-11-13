# Lab 3 提示词（prompt.md）

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




