#include <pmm.h>
#include <list.h>
#include <string.h>
#include <stdio.h>
#include <buddy_pmm.h>

/* Buddy System（伙伴系统）连续物理页分配器
 *
 * 基本约定：
 *   - 页下标 idx = page - pages。阶为 k 的块含 2^k 个连续页，头页 idx 必须是 2^k 的倍数
 *     （按"全局下标"对齐，与 init_memmap 交来的区间起点无关）。
 *   - 块 [idx, idx + 2^k) 的伙伴是下标 idx ^ (1 << k) 处的同阶块。
 *   - 空闲块：只有头页置 PG_property，且 property == 块的页数(2^k)；其余页 property == 0。
 *   - 已分配页：flags == 0、ref == 0，头页 property == 0。
 *   - buddy_free_list[k] 挂所有 2^k 页的空闲块，块按头页地址升序排列，使结果确定可复现。
 *   - 请求 n 页实际占 2^ceil(log2 n) 页。
 */

#define BUDDY_MAX_ORDER 15                                   // 最大块 = 2^15 页

static list_entry_t buddy_free_list[BUDDY_MAX_ORDER + 1];    // 第 k 条链表挂大小为 2^k 页的空闲块
static size_t buddy_nr_free;                                 // 空闲页总数

/* ------------------------------------------------------------------ 辅助函数 */

// 受管理的 Page 描述符个数
static inline size_t
buddy_page_cnt(void) {
    return npage - nbase;
}

// 能容纳 n 页的最小阶（2^order >= n）；n 过大时返回 BUDDY_MAX_ORDER + 1
static size_t
buddy_order_of(size_t n) {
    size_t order = 0;
    while (order <= BUDDY_MAX_ORDER && ((size_t)1 << order) < n) {
        order ++;
    }
    return order;
}

// 把头页为 page 的 2^order 页块按地址升序挂入 buddy_free_list[order]，并写好块头状态
static void
buddy_insert_block(struct Page *page, size_t order) {
    page->property = (unsigned int)((size_t)1 << order);
    SetPageProperty(page);
    list_entry_t *head = &buddy_free_list[order];
    list_entry_t *le = head;
    while ((le = list_next(le)) != head) {
        if (page < le2page(le, page_link)) {
            list_add_before(le, &(page->page_link));
            return;
        }
    }
    list_add_before(head, &(page->page_link));       // 比所有块都大（或链表为空）：追加到表尾
}

// 把空闲块 page 从链表中摘下，并清除块头状态（变成"非空闲头页"）
static void
buddy_remove_block(struct Page *page) {
    list_del(&(page->page_link));
    ClearPageProperty(page);
    page->property = 0;
}

/* ------------------------------------------------------------------ pmm_manager 接口 */

static void
buddy_init(void) {
    for (size_t i = 0; i <= BUDDY_MAX_ORDER; i ++) {
        list_init(&buddy_free_list[i]);
    }
    buddy_nr_free = 0;
}

/* 接管 [base, base+n)：把区间分解成若干对齐的块，每次取"起点按块大小对齐、
 * 不超出区间末尾、阶不超过 BUDDY_MAX_ORDER"的最大块。 */
static void
buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p;
    for (p = base; p != base + n; p ++) {
        assert(PageReserved(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }

    size_t idx = base - pages;
    size_t remain = n;
    while (remain > 0) {
        size_t order = 0;
        while (order < BUDDY_MAX_ORDER
               && (idx & (((size_t)1 << (order + 1)) - 1)) == 0      // 起点按 2^(order+1) 对齐
               && ((size_t)1 << (order + 1)) <= remain) {            // 且不越过区间末尾
            order ++;
        }
        buddy_insert_block(pages + idx, order);
        idx += (size_t)1 << order;
        remain -= (size_t)1 << order;
    }
    buddy_nr_free += n;
}

static struct Page *
buddy_alloc_pages(size_t n) {
    assert(n > 0);
    size_t order = buddy_order_of(n);
    if (order > BUDDY_MAX_ORDER || ((size_t)1 << order) > buddy_nr_free) {
        return NULL;
    }

    // 找到阶不小于 order 的最小非空链表
    size_t cur = order;
    while (cur <= BUDDY_MAX_ORDER && list_empty(&buddy_free_list[cur])) {
        cur ++;
    }
    if (cur > BUDDY_MAX_ORDER) {                     // 碎片：没有足够大的单个块
        return NULL;
    }

    // 取该链表中地址最小的块，逐级对半拆分：低半继续用，高半挂入低一阶链表
    struct Page *page = le2page(list_next(&buddy_free_list[cur]), page_link);
    buddy_remove_block(page);
    while (cur > order) {
        cur --;
        buddy_insert_block(page + ((size_t)1 << cur), cur);
    }

    buddy_nr_free -= (size_t)1 << order;
    return page;                                     // flags == 0、property == 0、ref == 0：已分配状态
}

static void
buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    size_t order = buddy_order_of(n);
    assert(order <= BUDDY_MAX_ORDER);
    size_t size = (size_t)1 << order;
    size_t idx = base - pages;
    assert((idx & (size - 1)) == 0);                 // 头页必须按块大小对齐

    struct Page *p;
    for (p = base; p != base + size; p ++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }
    buddy_nr_free += size;

    // 与伙伴逐级合并：伙伴必须在数组范围内、是空闲块头页、且大小恰为当前块大小
    while (order < BUDDY_MAX_ORDER) {
        size_t buddy_idx = idx ^ ((size_t)1 << order);
        if (buddy_idx >= buddy_page_cnt()) {
            break;
        }
        struct Page *buddy = pages + buddy_idx;
        if (!PageProperty(buddy) || buddy->property != ((size_t)1 << order)) {
            break;                                   // 保留页 / 已分配 / 阶不同：不合并
        }
        buddy_remove_block(buddy);
        if (buddy_idx < idx) {
            idx = buddy_idx;                         // 合并后的头页取低地址者
        }
        order ++;
    }
    buddy_insert_block(pages + idx, order);
}

static size_t
buddy_nr_free_pages(void) {
    return buddy_nr_free;
}

/* ------------------------------------------------------------------ 测试 */

#define BUDDY_POOL_PAGES 16                          // 受控小池大小（阶 4）
#define BUDDY_POOL_ORDER 4
#define BUDDY_MAX_BLOCKS 128

static size_t blk_begin[BUDDY_MAX_BLOCKS];
static size_t blk_end[BUDDY_MAX_BLOCKS];

/* Case 0：全局不变量。每个场景之后都调用。 */
static void
buddy_check_invariants(void) {
    size_t total = 0, nblk = 0;
    for (size_t k = 0; k <= BUDDY_MAX_ORDER; k ++) {
        size_t size = (size_t)1 << k;
        size_t last_idx = 0;
        bool first = 1;
        list_entry_t *head = &buddy_free_list[k];
        list_entry_t *le = head;
        while ((le = list_next(le)) != head) {
            struct Page *p = le2page(le, page_link);
            size_t idx = p - pages;
            assert(PageProperty(p));
            assert(!PageReserved(p));
            assert(p->property == size);
            assert((idx & (size - 1)) == 0);                 // 按全局下标对齐
            assert(idx + size <= buddy_page_cnt());
            assert(first || idx > last_idx);                 // 链表内严格升序
            first = 0;
            last_idx = idx;
            for (size_t i = 1; i < size; i ++) {             // 非头页没有 property 状态
                assert(!PageProperty(p + i) && p[i].property == 0 && !PageReserved(p + i));
            }
            assert(nblk < BUDDY_MAX_BLOCKS);
            blk_begin[nblk] = idx;
            blk_end[nblk] = idx + size;
            nblk ++;
            total += size;
        }
    }
    for (size_t i = 0; i < nblk; i ++) {                     // 块之间互不重叠
        for (size_t j = i + 1; j < nblk; j ++) {
            assert(blk_end[i] <= blk_begin[j] || blk_end[j] <= blk_begin[i]);
        }
    }
    assert(total == buddy_nr_free);
    assert(total == nr_free_pages());
}

// 小池应恰好是一个以 p 为头页的 16 页单块，其余链表全空，且空闲页数 16
static void
buddy_check_pool_whole(struct Page *p) {
    for (size_t k = 0; k <= BUDDY_MAX_ORDER; k ++) {
        if (k == BUDDY_POOL_ORDER) {
            continue;
        }
        assert(list_empty(&buddy_free_list[k]));
    }
    list_entry_t *head = &buddy_free_list[BUDDY_POOL_ORDER];
    assert(!list_empty(head));
    assert(list_next(head) == &(p->page_link));
    assert(list_next(&(p->page_link)) == head);
    assert(PageProperty(p) && p->property == BUDDY_POOL_PAGES);
    for (size_t i = 1; i < BUDDY_POOL_PAGES; i ++) {
        assert(!PageProperty(p + i) && p[i].property == 0);
    }
    assert(nr_free_pages() == BUDDY_POOL_PAGES);
    buddy_check_invariants();
}

// 链表 order 中是否有头页 p
static bool
buddy_list_has(size_t order, struct Page *p) {
    list_entry_t *head = &buddy_free_list[order];
    list_entry_t *le = head;
    while ((le = list_next(le)) != head) {
        if (le2page(le, page_link) == p) {
            return 1;
        }
    }
    return 0;
}

static void
buddy_check_allocated(struct Page *a, size_t n_round) {
    assert(a != NULL);
    for (size_t i = 0; i < n_round; i ++) {
        assert(a[i].flags == 0);                              // 不是 reserved，也没有 property
        assert(page_ref(a + i) == 0);
        assert(a[i].property == 0);
    }
    assert(((size_t)(a - pages) & (n_round - 1)) == 0);       // 取整后大小的对齐
}

static void
buddy_check(void) {
    cprintf("buddy_check() begin\n");

    /* ---------- Case 0：真实空闲内存上的不变量 ---------- */
    buddy_check_invariants();
    size_t nr0 = nr_free_pages();
    assert(nr0 > 0);

    /* ---------- Case 1：构造受控小池，并与真实空闲块隔离 ---------- */
    // 从真实分配器取一个对齐的 32 页块，只把前 16 页当作小池，后 16 页保持已分配，
    // 这样小池释放时，上一级伙伴是"已分配"，不会与真实空闲块合并。
    struct Page *big = alloc_pages(32);
    assert(big != NULL);
    assert(((size_t)(big - pages) & 31) == 0);
    assert(nr_free_pages() == nr0 - 32);
    buddy_check_invariants();

    list_entry_t saved_lists[BUDDY_MAX_ORDER + 1];            // 保存链表头本身（节点仍指向原链表头地址）
    for (size_t k = 0; k <= BUDDY_MAX_ORDER; k ++) {
        saved_lists[k] = buddy_free_list[k];
        list_init(&buddy_free_list[k]);
    }
    size_t saved_nr_free = buddy_nr_free;
    buddy_nr_free = 0;

    struct Page *p = big;
    buddy_insert_block(p, BUDDY_POOL_ORDER);
    buddy_nr_free = BUDDY_POOL_PAGES;
    buddy_check_pool_whole(p);

    /* ---------- Case 2：取整规则 ---------- */
    {
        static const size_t req[5] = {1, 2, 3, 4, 5};
        static const size_t rounded[5] = {1, 2, 4, 4, 8};
        for (size_t i = 0; i < 5; i ++) {
            size_t before = nr_free_pages();
            struct Page *a = alloc_pages(req[i]);
            buddy_check_allocated(a, rounded[i]);
            assert(a >= p && a + rounded[i] <= p + BUDDY_POOL_PAGES);
            assert(nr_free_pages() == before - rounded[i]);
            buddy_check_invariants();
            free_pages(a, req[i]);
            assert(nr_free_pages() == before);
            buddy_check_pool_whole(p);
        }
    }

    /* ---------- Case 3：拆分顺序确定 ---------- */
    {
        struct Page *a[BUDDY_POOL_PAGES];
        for (size_t i = 0; i < BUDDY_POOL_PAGES; i ++) {
            a[i] = alloc_page();
            assert(a[i] == p + i);                            // 低地址优先，依次得到 p, p+1, ...
            buddy_check_allocated(a[i], 1);
        }
        assert(alloc_page() == NULL);
        assert(nr_free_pages() == 0);
        buddy_check_invariants();
        for (size_t i = 0; i < BUDDY_POOL_PAGES; i ++) {
            free_page(a[i]);
        }
        buddy_check_pool_whole(p);
    }

    /* ---------- Case 4：伙伴合并 ---------- */
    {
        struct Page *a0 = alloc_page(), *a1 = alloc_page();
        assert(a0 == p && a1 == p + 1);
        free_page(a1);
        free_page(a0);                                        // 先释放高的再释放低的
        buddy_check_pool_whole(p);
        a0 = alloc_page(), a1 = alloc_page();
        assert(a0 == p && a1 == p + 1);
        free_page(a0);
        free_page(a1);                                        // 先低后高
        buddy_check_pool_whole(p);
    }

    /* ---------- Case 5：相邻但不是伙伴，不得合并 ---------- */
    {
        struct Page *a[5];
        for (size_t i = 0; i < 5; i ++) {
            a[i] = alloc_page();
            assert(a[i] == p + i);
        }
        free_page(p + 1);
        free_page(p + 2);                                     // 地址相邻，但 p+1 的伙伴是 p，p+2 的伙伴是 p+3
        assert(buddy_list_has(0, p + 1) && buddy_list_has(0, p + 2));
        assert(p[1].property == 1 && p[2].property == 1);
        assert(!buddy_list_has(1, p + 1));
        buddy_check_invariants();
        struct Page *two = alloc_pages(2);
        assert(two != p + 1 && two != p + 2);                 // 不能把相邻的两个单页当成一个 2 页块
        assert(two == p + 6);                                 // 来自已有的 2 页空闲块
        free_pages(two, 2);
        buddy_check_invariants();
        free_page(p);                                         // 与 p+1 合并为阶 1，p+2 阶不同，停住
        assert(buddy_list_has(1, p) && p->property == 2);
        assert(buddy_list_has(0, p + 2));
        free_page(p + 3);                                     // p+2、p+3 合并，再与 p 合并为阶 2；p+4 仍被占用，停住
        assert(buddy_list_has(2, p) && p->property == 4);
        assert(!PageProperty(p + 2) && !PageProperty(p + 3));
        assert(!buddy_list_has(0, p + 2) && !buddy_list_has(1, p + 2));
        buddy_check_invariants();
        free_page(p + 4);                                     // 级联合并回完整 16 页
        buddy_check_pool_whole(p);
    }

    /* ---------- Case 6：释放顺序无关与多级级联 ---------- */
    {
        for (int mode = 0; mode < 3; mode ++) {
            struct Page *a[BUDDY_POOL_PAGES];
            for (size_t i = 0; i < BUDDY_POOL_PAGES; i ++) {
                a[i] = alloc_page();
                assert(a[i] == p + i);
            }
            if (mode == 0) {                                  // 正序
                for (size_t i = 0; i < BUDDY_POOL_PAGES; i ++) {
                    free_page(a[i]);
                    buddy_check_invariants();
                }
            } else if (mode == 1) {                           // 倒序
                for (size_t i = BUDDY_POOL_PAGES; i > 0; i --) {
                    free_page(a[i - 1]);
                    buddy_check_invariants();
                }
            } else {                                          // 奇偶交错：先所有偶数下标，再所有奇数下标
                for (size_t i = 0; i < BUDDY_POOL_PAGES; i += 2) {
                    free_page(a[i]);
                    buddy_check_invariants();
                }
                assert(nr_free_pages() == BUDDY_POOL_PAGES / 2);
                for (size_t i = 1; i < BUDDY_POOL_PAGES; i += 2) {
                    free_page(a[i]);
                    buddy_check_invariants();
                }
            }
            buddy_check_pool_whole(p);
        }
    }

    /* ---------- Case 7：边界与失败路径 ---------- */
    {
        assert(alloc_pages(17) == NULL);                      // 取整后 32 页 > 16
        buddy_check_pool_whole(p);
        assert(alloc_pages(((size_t)1 << BUDDY_MAX_ORDER) + 1) == NULL);   // 超过最大阶
        buddy_check_pool_whole(p);

        struct Page *all = alloc_pages(16);
        assert(all == p);
        assert(alloc_page() == NULL);
        assert(nr_free_pages() == 0);
        free_pages(all, 16);
        buddy_check_pool_whole(p);

        // 碎片：16 个单页取出后只释放偶数下标，空闲页 8 个，但没有连续 2 页
        struct Page *a[BUDDY_POOL_PAGES];
        for (size_t i = 0; i < BUDDY_POOL_PAGES; i ++) {
            a[i] = alloc_page();
        }
        for (size_t i = 0; i < BUDDY_POOL_PAGES; i += 2) {
            free_page(a[i]);
        }
        assert(nr_free_pages() == 8);
        assert(alloc_pages(2) == NULL);
        assert(nr_free_pages() == 8);
        buddy_check_invariants();
        for (size_t i = 1; i < BUDDY_POOL_PAGES; i += 2) {
            free_page(a[i]);
        }
        buddy_check_pool_whole(p);
        all = alloc_pages(16);
        assert(all == p);
        free_pages(all, 16);
        buddy_check_pool_whole(p);
    }

    /* ---------- Case 8：块互不重叠与数据无串扰 ---------- */
    {
        static const size_t req[5] = {1, 3, 2, 5, 1};         // 取整后 1 + 4 + 2 + 8 + 1 = 16
        static const size_t rounded[5] = {1, 4, 2, 8, 1};
        struct Page *a[5];
        for (size_t i = 0; i < 5; i ++) {
            a[i] = alloc_pages(req[i]);
            buddy_check_allocated(a[i], rounded[i]);
            memset((void *)(page2pa(a[i]) + va_pa_offset), (int)(0x30 + i), rounded[i] * PGSIZE);
            buddy_check_invariants();
        }
        assert(nr_free_pages() == 0);
        for (size_t i = 0; i < 5; i ++) {                     // 两两不重叠
            for (size_t j = i + 1; j < 5; j ++) {
                assert(a[i] + rounded[i] <= a[j] || a[j] + rounded[j] <= a[i]);
            }
        }
        for (size_t i = 0; i < 5; i ++) {                     // 内容无串扰
            unsigned char *va = (unsigned char *)(page2pa(a[i]) + va_pa_offset);
            for (size_t b = 0; b < rounded[i] * PGSIZE; b += 509) {
                assert(va[b] == 0x30 + i);
            }
            assert(va[rounded[i] * PGSIZE - 1] == 0x30 + i);
        }
        free_pages(a[3], req[3]);
        free_pages(a[0], req[0]);
        free_pages(a[2], req[2]);
        free_pages(a[4], req[4]);
        free_pages(a[1], req[1]);
        buddy_check_pool_whole(p);
    }

    /* ---------- Case 9：恢复现场 ---------- */
    {
        struct Page *all = alloc_pages(16);                   // 把小池整块取回，16 页回到"已分配"状态
        assert(all == p);
        assert(nr_free_pages() == 0);
        for (size_t k = 0; k <= BUDDY_MAX_ORDER; k ++) {
            assert(list_empty(&buddy_free_list[k]));
            buddy_free_list[k] = saved_lists[k];              // 链表头地址不变，只恢复内容
        }
        buddy_nr_free = saved_nr_free;
        buddy_check_invariants();
        assert(nr_free_pages() == nr0 - 32);

        free_pages(big, 32);                                  // 与真实空闲块正常合并
        assert(nr_free_pages() == nr0);
        buddy_check_invariants();
    }

    cprintf("buddy_check() succeeded!\n");
}

/* 注册到 pmm_manager 机制上 */
const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
