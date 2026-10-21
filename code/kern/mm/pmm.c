#include <default_pmm.h>
#include <best_fit_pmm.h>
#include <buddy_pmm.h>
#include <slub.h>
#include <defs.h>
#include <error.h>
#include <memlayout.h>
#include <mmu.h>
#include <pmm.h>
#include <sbi.h>
#include <stdio.h>
#include <string.h>
#include <riscv.h>
#include <dtb.h>

// virtual address of physical page array
struct Page *pages;
// amount of physical memory (in pages)
size_t npage = 0;
// the kernel image is mapped at VA=KERNBASE and PA=info.base
uint64_t va_pa_offset;
// memory starts at 0x80000000 in RISC-V
// DRAM_BASE defined in riscv.h as 0x80000000
const size_t nbase = DRAM_BASE / PGSIZE;

// virtual address of boot-time page directory
uintptr_t *satp_virtual = NULL;
// physical address of boot-time page directory
uintptr_t satp_physical;

// physical memory management
const struct pmm_manager *pmm_manager;


static void check_alloc_page(void);

// init_pmm_manager - initialize a pmm_manager instance
static void init_pmm_manager(void) {
    //lab2 任务三 学号：2411433：切换为 Buddy System 连续物理内存分配算法
    pmm_manager = &buddy_pmm_manager;
    cprintf("memory management: %s\n", pmm_manager->name);
    pmm_manager->init();
}

// init_memmap - call pmm->init_memmap to build Page struct for free memory
static void init_memmap(struct Page *base, size_t n) {
    pmm_manager->init_memmap(base, n);
}

// alloc_pages - call pmm->alloc_pages to allocate a continuous n*PAGESIZE
// memory
struct Page *alloc_pages(size_t n) {
    return pmm_manager->alloc_pages(n);
}

// free_pages - call pmm->free_pages to free a continuous n*PAGESIZE memory
void free_pages(struct Page *base, size_t n) {
    pmm_manager->free_pages(base, n);
}

// nr_free_pages - call pmm->nr_free_pages to get the size (nr*PAGESIZE)
// of current free memory
size_t nr_free_pages(void) {
    return pmm_manager->nr_free_pages();
}

//lab2 function1 学号：2411570
/* page_init - 探测物理内存、建立 Page 描述符数组，并把空闲页交给 pmm_manager
 *
 * 地址的四种“身份”（务必区分）：
 *   - 物理地址 PA      ：设备树给出的 [mem_begin, mem_end)，以及 free_begin/free_end
 *   - 虚拟地址 VA      ：内核里的指针，如 end、pages（= PA + va_pa_offset）
 *   - 物理页号 PPN     ：PA >> PGSHIFT；npage 是“上界”页号，nbase 是 DRAM 起始页号
 *   - Page 描述符地址  ：pages[PPN - nbase] 的地址，由 pa2page()/page2pa() 转换
 */
static void page_init(void) {
    extern char end[];  // 链接脚本 kernel.ld 定义：内核镜像（含 BSS）的结束虚拟地址

    // 1. 内核虚拟地址 = 物理地址 + va_pa_offset，必须最先设置，后面的 PADDR() 依赖它
    va_pa_offset = PHYSICAL_MEMORY_OFFSET;

    // 2. 从设备树解析结果中取得物理内存范围；无效则无法继续
    uint64_t mem_begin = get_memory_base();
    uint64_t mem_size = get_memory_size();
    if (mem_size == 0 || mem_begin == 0) {
        panic("page_init: invalid physical memory info from device tree");
    }
    // nbase 在本框架中固定为 DRAM_BASE / PGSIZE，描述符数组按它下标，基址不符就无法正确管理
    if (mem_begin != DRAM_BASE) {
        panic("page_init: memory base 0x%lx != DRAM_BASE 0x%lx", mem_begin, (uint64_t)DRAM_BASE);
    }
    uint64_t mem_end = mem_begin + mem_size;  // 开区间上界（物理地址）

    cprintf("physical memory map:\n");
    cprintf("  memory: 0x%016lx, [0x%016lx, 0x%016lx].\n", mem_size, mem_begin, mem_end - 1);

    // 3. 总页数上界：对未对齐的 mem_end 向下取整（PPN 本身就是右移，天然向下取整）
    npage = PPN(mem_end);
    if (npage <= nbase) {
        panic("page_init: no physical pages above DRAM base");
    }
    size_t page_cnt = npage - nbase;  // 描述符数组的元素个数

    // 4. Page 描述符数组紧跟在内核镜像之后，起点按页对齐
    pages = (struct Page *)ROUNDUP((uintptr_t)end, PGSIZE);

    // 5. 先把整个数组清零（不依赖内存初值），再统一标记为“保留”。
    //    这一步使内核镜像、OpenSBI、数组自身所在的页都保持 Reserved；
    //    之后只有交给 init_memmap 的页才会被管理器清掉 Reserved（default/best-fit 的 init_memmap 里有 assert(PageReserved(p))）
    memset(pages, 0, sizeof(struct Page) * page_cnt);
    for (size_t i = 0; i < page_cnt; i++) {
        SetPageReserved(pages + i);
    }

    // 6. 可管理空闲区的物理区间：
    //    起点 = 描述符数组末尾（此时已在内核之后）向上对齐；
    //    终点 = min(mem_end, PHYSICAL_MEMORY_END) 向下对齐。
    //    framework 声明的可管理物理内存上界是 PHYSICAL_MEMORY_END；设备树报告的内存若更大
    //    （例如 QEMU 加了 -m 256M），超出的部分不交给管理器。npage 仍按设备树的真实上界计算，
    //    描述符数组照常覆盖这些页（它们保持 Reserved）。
    uintptr_t freemem = PADDR((uintptr_t)(pages + page_cnt));
    uintptr_t free_begin = ROUNDUP(freemem, PGSIZE);
    uintptr_t managed_end = mem_end < PHYSICAL_MEMORY_END ? mem_end : (uintptr_t)PHYSICAL_MEMORY_END;
    uintptr_t free_end = ROUNDDOWN(managed_end, PGSIZE);

    // 7. 只有确有完整空闲页时才交给管理器，否则不能调用 init_memmap（n 必须 > 0）
    if (free_begin < free_end) {
        init_memmap(pa2page(free_begin), (free_end - free_begin) / PGSIZE);
    }
}


/* pmm_init - initialize the physical memory management */
void pmm_init(void) {
    // We need to alloc/free the physical memory (granularity is 4KB or other size).
    // So a framework of physical memory manager (struct pmm_manager)is defined in pmm.h
    // First we should init a physical memory manager(pmm) based on the framework.
    // Then pmm can alloc/free the physical memory.
    // Now the first_fit/best_fit/worst_fit/buddy_system pmm are available.
    init_pmm_manager();

    // detect physical memory space, reserve already used memory,
    // then use pmm->init_memmap to create free page list
    page_init();

    // use pmm->check to verify the correctness of the alloc/free function in a pmm
    check_alloc_page();

    // 扩展任务 Challenge 学号：2411433：在页级分配器之上初始化并自检 SLUB 任意大小分配器
    slub_init();
    slub_check();

    extern char boot_page_table_sv39[];
    satp_virtual = (pte_t*)boot_page_table_sv39;
    satp_physical = PADDR(satp_virtual);
    cprintf("satp virtual address: 0x%016lx\nsatp physical address: 0x%016lx\n", satp_virtual, satp_physical);
}

static void check_alloc_page(void) {
    pmm_manager->check();
    cprintf("check_alloc_page() succeeded!\n");
}
