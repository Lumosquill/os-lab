#include <pmm.h>
#include <list.h>
#include <string.h>
#include <best_fit_pmm.h>
#include <stdio.h>

//lab2 function2-2 学号：2410699

/* Best-Fit（最佳适配）连续物理内存分配器
 *
 * 机制（pmm_manager 函数指针表）完全不变，这里只是换一种“选块策略”：
 *   First-Fit（kern/mm/default_pmm.c）：从表头开始扫，遇到第一个 property >= n 的块就 break；
 *   Best-Fit（本文件）               ：扫完整条空闲链表，在所有 property >= n 的块里
 *                                      取 property 最小的那个（剩余空洞最小）；
 *                                      大小相同时取链表中靠前的那个，行为确定可复现。
 *
 * 空闲链表仍然按“物理地址升序”维护，因为释放时判断两个空闲块能否合并，
 * 靠的就是 prev/next 在物理上是否紧邻（p + p->property == base）。
 * 所以 init / init_memmap / free_pages 与 First-Fit 一致，只有 alloc_pages 的选块不同。
 */

static free_area_t free_area;

// 框架测试 basic_check() 与 best_fit_check() 直接引用了 free_list 和 nr_free，
// 这两个标识符必须存在，含义与 First-Fit 中相同（做法也沿用 default_pmm.c）。
#define free_list (free_area.free_list)
#define nr_free (free_area.nr_free)

/* 内部辅助函数：把空闲块头页 base（property 已填好）按物理地址升序插入空闲链表。
 * 不单独占一个管理函数位，只服务于 init_memmap 与 free_pages 两处。 */
static void
best_fit_insert_free_block(struct Page *base) {
    list_entry_t *le = &free_list;
    while ((le = list_next(le)) != &free_list) {
        // 找到第一个地址比 base 大的空闲块，插到它前面，链表即保持升序
        if (base < le2page(le, page_link)) {
            list_add_before(le, &(base->page_link));
            return;
        }
    }
    // 比链表里所有块都大（或链表本来为空）：追加到表尾
    // 表尾 = list_prev(&free_list)；链表为空时它就是表头，list_add 等价于插入空表
    list_add(list_prev(&free_list), &(base->page_link));
}

// 初始化内部状态：空链表、0 个空闲页
static void
best_fit_init(void) {
    list_init(&free_list);
    nr_free = 0;
}

/* 把 page_init() 交来的 [base, base + n) 这段空闲内存接管为“一个”空闲块：
 * 逐页清 flags / property / ref，再让头页 base 记录整块大小并挂上链表。 */
static void
best_fit_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p;
    for (p = base; p != base + n; p ++) {
        assert(PageReserved(p));        // 只接管原本被标记为“保留”的页
        p->flags = 0;                   // 清掉 PG_reserved / PG_property
        p->property = 0;
        set_page_ref(p, 0);
    }
    base->property = n;                 // 只有头页的 property 非 0，值为块大小
    SetPageProperty(base);
    nr_free += n;
    best_fit_insert_free_block(base);
}

/* 分配连续的 n 个页：Best-Fit 选块 —— 所有够大的块里挑最小的那一个。 */
static struct Page *
best_fit_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > nr_free) {                  // 总数都不够，直接失败
        return NULL;
    }

    // 扫完整条链表：用“严格小于”比较，所以大小相同时保留链表中靠前的那个
    struct Page *page = NULL;
    list_entry_t *le = &free_list;
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        if (p->property >= n && (page == NULL || p->property < page->property)) {
            page = p;
        }
    }
    if (page == NULL) {                 // 没有任何单个块够大
        return NULL;
    }

    // 先记下被选中块在链表中的位置：它被摘下后，剩余部分要放回这里才不会破坏地址升序
    list_entry_t *prev = list_prev(&(page->page_link));
    list_del(&(page->page_link));

    if (page->property > n) {
        // 情况二（过大）：切出前 n 页，剩下的 (page + n, property - n) 成为新的空闲块头页
        struct Page *rest = page + n;
        rest->property = page->property - n;
        SetPageProperty(rest);
        list_add(prev, &(rest->page_link));
    }
    // 情况一（恰好适配）：整块摘下，不产生剩余分片，property 归 0 表示已被占用
    page->property = 0;
    ClearPageProperty(page);            // 头页不再是非空闲块
    nr_free -= n;
    return page;
}

/* 释放连续的 n 个页：按地址插回链表，并与物理相邻的前/后空闲块合并。 */
static void
best_fit_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p;
    for (p = base; p != base + n; p ++) {
        assert(!PageReserved(p) && !PageProperty(p));   // 确实是一段已分配出去的页
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }
    base->property = n;
    SetPageProperty(base);
    nr_free += n;

    best_fit_insert_free_block(base);

    // 1) 先与低地址方向的邻居合并：链表升序，所以 prev 就是物理上位于它前面的块
    list_entry_t *le = list_prev(&(base->page_link));
    if (le != &free_list) {
        struct Page *prev = le2page(le, page_link);
        if (prev + prev->property == base) {    // 必须在物理地址上紧邻
            prev->property += base->property;
            ClearPageProperty(base);
            base->property = 0;
            list_del(&(base->page_link));
            base = prev;                        // 合并后的头页是 prev
        }
    }

    // 2) 再与高地址方向的邻居合并：此时 base 已是合并后大块的头页
    le = list_next(&(base->page_link));
    if (le != &free_list) {
        struct Page *next = le2page(le, page_link);
        if (base + base->property == next) {    // 同样要求物理上紧邻
            base->property += next->property;
            ClearPageProperty(next);
            next->property = 0;
            list_del(&(next->page_link));
        }
    }
}

// 空闲页总数（各块 property 之和）
static size_t
best_fit_nr_free_pages(void) {
    return nr_free;
}

static void
basic_check(void) {
    struct Page *p0, *p1, *p2;
    p0 = p1 = p2 = NULL;
    assert((p0 = alloc_page()) != NULL);
    assert((p1 = alloc_page()) != NULL);
    assert((p2 = alloc_page()) != NULL);

    assert(p0 != p1 && p0 != p2 && p1 != p2);
    assert(page_ref(p0) == 0 && page_ref(p1) == 0 && page_ref(p2) == 0);

    assert(page2pa(p0) < npage * PGSIZE);
    assert(page2pa(p1) < npage * PGSIZE);
    assert(page2pa(p2) < npage * PGSIZE);

    list_entry_t free_list_store = free_list;
    list_init(&free_list);
    assert(list_empty(&free_list));

    unsigned int nr_free_store = nr_free;
    nr_free = 0;

    assert(alloc_page() == NULL);

    free_page(p0);
    free_page(p1);
    free_page(p2);
    assert(nr_free == 3);

    assert((p0 = alloc_page()) != NULL);
    assert((p1 = alloc_page()) != NULL);
    assert((p2 = alloc_page()) != NULL);

    assert(alloc_page() == NULL);

    free_page(p0);
    assert(!list_empty(&free_list));

    struct Page *p;
    assert((p = alloc_page()) == p0);
    assert(alloc_page() == NULL);

    assert(nr_free == 0);
    free_list = free_list_store;
    nr_free = nr_free_store;

    free_page(p);
    free_page(p1);
    free_page(p2);
}

// LAB2: below code is used to check the best fit allocation algorithm (your EXERCISE 1) 
// NOTICE: You SHOULD NOT CHANGE basic_check, default_check functions!
static void
best_fit_check(void) {
    int score = 0 ,sumscore = 6;
    int count = 0, total = 0;
    list_entry_t *le = &free_list;
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        assert(PageProperty(p));
        count ++, total += p->property;
    }
    assert(total == nr_free_pages());

    basic_check();

    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n",score, sumscore);
    #endif
    struct Page *p0 = alloc_pages(5), *p1, *p2;
    assert(p0 != NULL);
    assert(!PageProperty(p0));

    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n",score, sumscore);
    #endif
    list_entry_t free_list_store = free_list;
    list_init(&free_list);
    assert(list_empty(&free_list));
    assert(alloc_page() == NULL);

    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n",score, sumscore);
    #endif
    unsigned int nr_free_store = nr_free;
    nr_free = 0;

    // * - - * -
    free_pages(p0 + 1, 2);
    free_pages(p0 + 4, 1);
    assert(alloc_pages(4) == NULL);
    assert(PageProperty(p0 + 1) && p0[1].property == 2);
    // * - - * *
    assert((p1 = alloc_pages(1)) != NULL);
    assert(alloc_pages(2) != NULL);      // best fit feature
    assert(p0 + 4 == p1);

    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n",score, sumscore);
    #endif
    p2 = p0 + 1;
    free_pages(p0, 5);
    assert((p0 = alloc_pages(5)) != NULL);
    assert(alloc_page() == NULL);

    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n",score, sumscore);
    #endif
    assert(nr_free == 0);
    nr_free = nr_free_store;

    free_list = free_list_store;
    free_pages(p0, 5);

    le = &free_list;
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        count --, total -= p->property;
    }
    assert(count == 0);
    assert(total == 0);
    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n",score, sumscore);
    #endif
}

/* 注册到 pmm_manager 这套“机制”上：pmm.c 的 init_pmm_manager() 选中它之后，
 * 上层的 alloc_pages()/free_pages()/nr_free_pages() 就都转发到本文件的 5 个 static 函数。 */
const struct pmm_manager best_fit_pmm_manager = {
    .name = "best_fit_pmm_manager",
    .init = best_fit_init,
    .init_memmap = best_fit_init_memmap,
    .alloc_pages = best_fit_alloc_pages,
    .free_pages = best_fit_free_pages,
    .nr_free_pages = best_fit_nr_free_pages,
    .check = best_fit_check,
};
