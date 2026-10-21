#include <pmm.h>
#include <list.h>
#include <string.h>
#include <stdio.h>
#include <slub.h>

/* 简化版两层 SLUB 分配器
 *
 * 第一层：页级分配，直接用 alloc_pages()/free_pages()（不关心具体是哪种 pmm_manager）。
 * 第二层：kmalloc()/kfree()
 *   - 小对象（size <= 2048）：按 16/32/.../2048 共 8 个分级，每级一个 kmem_cache。
 *     每个 slab 就是一个物理页：页首放 struct slab 头部（向上取整到 16 字节），
 *     其后从低到高紧密切成等大对象；空闲对象的前 8 字节串成 LIFO 单链表。
 *     cache 维护 partial（还有空闲对象）和 full（已满）两条 slab 链表。
 *     slab 变空时立即把整页还给第一层。
 *   - 大块（size > 2048）：直接按页向第一层申请，头页置 PG_large，Page.property 记录页数。
 *
 * 地址：Page -> 内核虚拟地址用 page2pa(page) + va_pa_offset；
 *       kfree(ptr) 用 PADDR(ptr) 与 pa2page() 找回页描述符，据 PG_slab / PG_large 判定归属。
 */

#define SLUB_NR_CLASSES     8
#define SLUB_MIN_SIZE       16
#define SLUB_MAX_SIZE       2048

struct kmem_cache;

struct slab {
    struct kmem_cache *cache;       // 所属 cache
    void *freelist;                 // 空闲对象链表头（LIFO），空则为 NULL
    unsigned int inuse;             // 已分配对象数
    list_entry_t link;              // 挂在 cache 的 partial / full 链表上
};

struct kmem_cache {
    size_t obj_size;                // 对象大小（分级大小）
    size_t objs_per_slab;           // 单个 slab 容纳的对象数
    list_entry_t partial;           // 有空闲对象的 slab
    list_entry_t full;              // 已满的 slab
};

// slab 头部大小：向上取整到 16 字节，保证对象至少 16 字节对齐
#define SLAB_HDR_SIZE   ((sizeof(struct slab) + 15) & ~(size_t)15)

#define le2slab(le)     to_struct((le), struct slab, link)

static struct kmem_cache slub_caches[SLUB_NR_CLASSES];     // 静态分配，不依赖 kmalloc 自身

/* ------------------------------------------------------------------ 辅助函数 */

static inline void *
page2kva(struct Page *page) {
    return (void *)(page2pa(page) + va_pa_offset);
}

static inline size_t
slub_class_index(size_t size) {
    size_t idx = 0, cls = SLUB_MIN_SIZE;
    while (cls < size) {
        cls <<= 1;
        idx ++;
    }
    return idx;
}

// 为 cache 申请一个新 slab：取一页、写头部、把对象按地址升序串成空闲链表，挂入 partial
static struct slab *
slub_new_slab(struct kmem_cache *cache) {
    struct Page *page = alloc_page();
    if (page == NULL) {
        return NULL;
    }
    SetPageSlab(page);

    struct slab *slab = (struct slab *)page2kva(page);
    slab->cache = cache;
    slab->inuse = 0;

    char *base = (char *)slab + SLAB_HDR_SIZE;
    void *head = NULL;
    for (size_t i = cache->objs_per_slab; i > 0; i --) {    // 从最后一个对象开始头插，最终链表地址升序
        void *obj = base + (i - 1) * cache->obj_size;
        *(void **)obj = head;
        head = obj;
    }
    slab->freelist = head;
    list_add(&cache->partial, &slab->link);
    return slab;
}

/* ------------------------------------------------------------------ 对外接口 */

void
slub_init(void) {
    size_t size = SLUB_MIN_SIZE;
    for (size_t i = 0; i < SLUB_NR_CLASSES; i ++, size <<= 1) {
        slub_caches[i].obj_size = size;
        slub_caches[i].objs_per_slab = (PGSIZE - SLAB_HDR_SIZE) / size;
        list_init(&slub_caches[i].partial);
        list_init(&slub_caches[i].full);
    }
}

size_t
slub_class_size(size_t size) {
    if (size == 0 || size > SLUB_MAX_SIZE) {
        return 0;
    }
    return slub_caches[slub_class_index(size)].obj_size;
}

size_t
slub_objs_per_slab(size_t class_size) {
    if (class_size < SLUB_MIN_SIZE || class_size > SLUB_MAX_SIZE
        || (class_size & (class_size - 1)) != 0) {
        return 0;
    }
    return (PGSIZE - SLAB_HDR_SIZE) / class_size;
}

void *
kmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    if (size > SLUB_MAX_SIZE) {
        // 大块直通：先和剩余页数比较，既快速失败，也避免 ROUNDUP 溢出
        if (size > nr_free_pages() * PGSIZE) {
            return NULL;
        }
        size_t n = (size + PGSIZE - 1) / PGSIZE;
        struct Page *page = alloc_pages(n);
        if (page == NULL) {
            return NULL;
        }
        SetPageLarge(page);
        page->property = n;
        return page2kva(page);
    }

    struct kmem_cache *cache = &slub_caches[slub_class_index(size)];
    struct slab *slab;
    if (list_empty(&cache->partial)) {
        slab = slub_new_slab(cache);
        if (slab == NULL) {
            return NULL;
        }
    } else {
        slab = le2slab(list_next(&cache->partial));
    }

    void *obj = slab->freelist;                  // 弹出空闲链表头
    slab->freelist = *(void **)obj;
    slab->inuse ++;
    if (slab->freelist == NULL) {                // 刚好用完：partial -> full
        list_del(&slab->link);
        list_add(&cache->full, &slab->link);
    }
    return obj;
}

void
kfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    uintptr_t pa = PADDR(ptr);                   // ptr 不是内核虚拟地址会 panic
    if (PPN(pa) < nbase || PPN(pa) >= npage) {
        panic("kfree: invalid pointer %p", ptr);
    }
    struct Page *page = pa2page(pa);

    if (PageSlab(page)) {
        struct slab *slab = (struct slab *)ROUNDDOWN((uintptr_t)ptr, PGSIZE);
        struct kmem_cache *cache = slab->cache;
        char *base = (char *)slab + SLAB_HDR_SIZE;
        if ((char *)ptr < base) {
            panic("kfree: pointer %p points into slab header", ptr);
        }
        size_t off = (char *)ptr - base;
        if (off % cache->obj_size != 0 || off / cache->obj_size >= cache->objs_per_slab) {
            panic("kfree: pointer %p is not an object of its slab", ptr);
        }

        bool was_full = (slab->freelist == NULL);
        *(void **)ptr = slab->freelist;          // 压回空闲链表头部（LIFO）
        slab->freelist = ptr;
        slab->inuse --;

        if (slab->inuse == 0) {                  // 空 slab 立即还给第一层
            list_del(&slab->link);
            ClearPageSlab(page);
            free_page(page);
        } else if (was_full) {                   // full -> partial
            list_del(&slab->link);
            list_add(&cache->partial, &slab->link);
        }
        return;
    }

    if (PageLarge(page)) {
        if (((uintptr_t)ptr & (PGSIZE - 1)) != 0) {
            panic("kfree: pointer %p is not page aligned", ptr);
        }
        size_t n = page->property;
        ClearPageLarge(page);
        page->property = 0;
        free_pages(page, n);
        return;
    }

    panic("kfree: pointer %p was not returned by kmalloc", ptr);
}

/* ------------------------------------------------------------------ 测试 */

#define SLUB_TEST_MAX   1024

struct slub_rec {
    void *ptr;
    size_t size;
    unsigned char val;
};
static struct slub_rec slub_recs[SLUB_TEST_MAX];

static uint32_t slub_rng_state = 12345;

static uint32_t
slub_rng(void) {
    slub_rng_state = slub_rng_state * 1664525u + 1013904223u;
    return slub_rng_state >> 8;
}

static void
slub_fill(void *p, size_t size, unsigned char val) {
    unsigned char *c = (unsigned char *)p;
    for (size_t i = 0; i < size; i ++) {
        c[i] = (unsigned char)(val + (i & 3));   // 每个字节位置略有不同，更易发现错位
    }
}

static void
slub_verify(void *p, size_t size, unsigned char val) {
    unsigned char *c = (unsigned char *)p;
    for (size_t i = 0; i < size; i ++) {
        assert(c[i] == (unsigned char)(val + (i & 3)));
    }
}

// 对一个刚分配的小/大对象做通用检查
static void
slub_check_ptr(void *p, size_t size) {
    assert(p != NULL);
    assert(((uintptr_t)p & 15) == 0);                         // 至少 16 字节对齐
    assert((uintptr_t)p >= KERNBASE);                         // 内核虚拟地址
    if (size <= SLUB_MAX_SIZE) {
        assert((uintptr_t)p / PGSIZE == ((uintptr_t)p + size - 1) / PGSIZE);   // 对象不跨页
    } else {
        assert(((uintptr_t)p & (PGSIZE - 1)) == 0);           // 大块页对齐
    }
}

static void
slub_check_unique(size_t cnt) {
    for (size_t i = 0; i < cnt; i ++) {
        for (size_t j = i + 1; j < cnt; j ++) {
            uintptr_t a = (uintptr_t)slub_recs[i].ptr, b = (uintptr_t)slub_recs[j].ptr;
            assert(a + slub_recs[i].size <= b || b + slub_recs[j].size <= a);   // 存活分配互不重叠
        }
    }
}

void
slub_check(void) {
    cprintf("slub_check() begin\n");
    size_t nr0 = nr_free_pages();

    /* ---------- Case 1：分级选择 ---------- */
    {
        static const size_t in[]  = {1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129,
                                     255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 2047, 2048};
        static const size_t out[] = {16, 16, 16, 32, 32, 32, 64, 64, 64, 128, 128, 128, 256,
                                     256, 256, 512, 512, 512, 1024, 1024, 1024, 2048, 2048, 2048};
        for (size_t i = 0; i < sizeof(in) / sizeof(in[0]); i ++) {
            assert(slub_class_size(in[i]) == out[i]);
        }
        assert(slub_class_size(0) == 0);
        assert(slub_class_size(2049) == 0);
        assert(slub_class_size(PGSIZE) == 0);
        for (size_t c = 16; c <= 2048; c <<= 1) {
            size_t per = slub_objs_per_slab(c);
            assert(per > 0 && per * c <= PGSIZE);
        }
        assert(slub_objs_per_slab(0) == 0);
        assert(slub_objs_per_slab(24) == 0);
        assert(slub_objs_per_slab(4096) == 0);
    }

    /* ---------- Case 2：基本分配与内容保持 ---------- */
    {
        size_t cnt = 0;
        for (size_t c = 16; c <= 2048; c <<= 1) {
            for (size_t j = 0; j < 5; j ++) {
                size_t size = (j == 0) ? 1 : (j == 1 ? c / 2 + 1 : c);   // 含"比分级小"的请求
                if (size > c) size = c;
                void *p = kmalloc(size);
                slub_check_ptr(p, size);
                slub_recs[cnt].ptr = p;
                slub_recs[cnt].size = size;
                slub_recs[cnt].val = (unsigned char)(cnt * 7 + 1);
                slub_fill(p, size, slub_recs[cnt].val);
                cnt ++;
            }
        }
        slub_check_unique(cnt);
        for (size_t i = 0; i < cnt; i ++) {                          // 全部分配完之后统一校验
            slub_verify(slub_recs[i].ptr, slub_recs[i].size, slub_recs[i].val);
        }
        for (size_t i = 0; i < cnt; i ++) {
            kfree(slub_recs[i].ptr);
        }
        assert(nr_free_pages() == nr0);
    }

    /* ---------- Case 3：slab 增长与回收的页账目 ---------- */
    {
        static const size_t classes[] = {16, 256, 2048};
        for (size_t ci = 0; ci < 3; ci ++) {
            size_t c = classes[ci];
            size_t per = slub_objs_per_slab(c);
            assert(per > 0 && 3 * per + 1 <= SLUB_TEST_MAX);

            // 恰好填满 1 个 slab 占 1 页，再多 1 个占第 2 页
            size_t before = nr_free_pages();
            for (size_t i = 0; i < per; i ++) {
                slub_recs[i].ptr = kmalloc(c);
                slub_recs[i].size = c;
                slub_check_ptr(slub_recs[i].ptr, c);
            }
            assert(nr_free_pages() == before - 1);
            slub_recs[per].ptr = kmalloc(c);
            slub_recs[per].size = c;
            assert(slub_recs[per].ptr != NULL);
            assert(nr_free_pages() == before - 2);
            slub_check_unique(per + 1);
            for (size_t i = 0; i <= per; i ++) {
                kfree(slub_recs[i].ptr);
            }
            assert(nr_free_pages() == before);                       // 空 slab 被及时归还

            // 3 个 slab
            for (size_t i = 0; i < 3 * per; i ++) {
                slub_recs[i].ptr = kmalloc(c);
                slub_recs[i].size = c;
                assert(slub_recs[i].ptr != NULL);
            }
            assert(nr_free_pages() == before - 3);
            for (size_t i = 0; i < 3 * per; i ++) {
                kfree(slub_recs[i].ptr);
            }
            assert(nr_free_pages() == before);
        }
    }

    /* ---------- Case 4：LIFO 复用与 full/partial 状态流转 ---------- */
    {
        size_t c = 64;
        size_t per = slub_objs_per_slab(c);
        size_t before = nr_free_pages();
        for (size_t i = 0; i < per; i ++) {
            slub_recs[i].ptr = kmalloc(c);
            slub_recs[i].size = c;
            slub_recs[i].val = (unsigned char)(i + 1);
            slub_fill(slub_recs[i].ptr, c, slub_recs[i].val);
        }
        assert(nr_free_pages() == before - 1);                        // slab 已满
        size_t x = per / 2;
        void *px = slub_recs[x].ptr;
        kfree(px);                                                    // full -> partial
        void *again = kmalloc(c);
        assert(again == px);                                          // LIFO：刚释放的最先复用
        assert(nr_free_pages() == before - 1);                        // 没有新开 slab
        slub_fill(again, c, 0x99);                                    // 写新对象不影响其余对象
        for (size_t i = 0; i < per; i ++) {
            if (i != x) {
                slub_verify(slub_recs[i].ptr, c, slub_recs[i].val);
            }
        }
        // 连续释放两个，再连续分配：后释放的先出来
        kfree(slub_recs[1].ptr);
        kfree(slub_recs[2].ptr);
        void *n1 = kmalloc(c);
        void *n2 = kmalloc(c);
        assert(n1 == slub_recs[2].ptr && n2 == slub_recs[1].ptr);
        assert(nr_free_pages() == before - 1);
        for (size_t i = 0; i < per; i ++) {
            if (i == x) {
                kfree(again);
            } else if (i == 1) {
                kfree(n2);
            } else if (i == 2) {
                kfree(n1);
            } else {
                kfree(slub_recs[i].ptr);
            }
        }
        assert(nr_free_pages() == before);
    }

    /* ---------- Case 5：不同释放顺序 ---------- */
    {
        size_t c = 128, cnt = 100;
        for (int mode = 0; mode < 3; mode ++) {
            size_t before = nr_free_pages();
            for (size_t i = 0; i < cnt; i ++) {
                slub_recs[i].ptr = kmalloc(c);
                slub_recs[i].size = c;
                slub_recs[i].val = (unsigned char)(i * 3 + mode);
                assert(slub_recs[i].ptr != NULL);
                slub_fill(slub_recs[i].ptr, c, slub_recs[i].val);
            }
            slub_check_unique(cnt);
            if (mode == 0) {
                for (size_t i = 0; i < cnt; i ++) {
                    slub_verify(slub_recs[i].ptr, c, slub_recs[i].val);
                    kfree(slub_recs[i].ptr);
                }
            } else if (mode == 1) {
                for (size_t i = cnt; i > 0; i --) {
                    slub_verify(slub_recs[i - 1].ptr, c, slub_recs[i - 1].val);
                    kfree(slub_recs[i - 1].ptr);
                }
            } else {
                for (size_t i = 0; i < cnt; i += 2) {
                    slub_verify(slub_recs[i].ptr, c, slub_recs[i].val);
                    kfree(slub_recs[i].ptr);
                }
                for (size_t i = 1; i < cnt; i += 2) {
                    slub_verify(slub_recs[i].ptr, c, slub_recs[i].val);
                    kfree(slub_recs[i].ptr);
                }
            }
            assert(nr_free_pages() == before);
            // 释放后同样数量的对象仍能全部重新分配成功
            for (size_t i = 0; i < cnt; i ++) {
                slub_recs[i].ptr = kmalloc(c);
                assert(slub_recs[i].ptr != NULL);
            }
            for (size_t i = 0; i < cnt; i ++) {
                kfree(slub_recs[i].ptr);
            }
            assert(nr_free_pages() == before);
        }
    }

    /* ---------- Case 6：大块直通 ---------- */
    {
        static const size_t sizes[] = {2049, 4096, 4097, 3 * PGSIZE, 5 * PGSIZE + 1};
        static const size_t pages_needed[] = {1, 1, 2, 3, 6};
        for (size_t i = 0; i < 5; i ++) {
            size_t before = nr_free_pages();
            void *p = kmalloc(sizes[i]);
            slub_check_ptr(p, sizes[i]);
            assert(nr_free_pages() <= before - pages_needed[i]);      // 至少占这么多页（页分配器可能向上取整）
            slub_fill(p, sizes[i], 0x5a);
            slub_verify(p, sizes[i], 0x5a);
            kfree(p);
            assert(nr_free_pages() == before);
        }
        // 大块与小对象交错，互不影响
        size_t before = nr_free_pages();
        void *s1 = kmalloc(32);
        void *b1 = kmalloc(2 * PGSIZE + 5);
        void *s2 = kmalloc(1024);
        void *b2 = kmalloc(3000);
        slub_fill(s1, 32, 0x11);
        slub_fill(b1, 2 * PGSIZE + 5, 0x22);
        slub_fill(s2, 1024, 0x33);
        slub_fill(b2, 3000, 0x44);
        slub_verify(s1, 32, 0x11);
        slub_verify(b1, 2 * PGSIZE + 5, 0x22);
        slub_verify(s2, 1024, 0x33);
        slub_verify(b2, 3000, 0x44);
        kfree(b1);
        slub_verify(s1, 32, 0x11);
        slub_verify(s2, 1024, 0x33);
        slub_verify(b2, 3000, 0x44);
        kfree(s1);
        kfree(b2);
        slub_verify(s2, 1024, 0x33);
        kfree(s2);
        assert(nr_free_pages() == before);
    }

    /* ---------- Case 7：失败路径与无效参数 ---------- */
    {
        size_t before = nr_free_pages();
        assert(kmalloc(0) == NULL);
        kfree(NULL);
        assert(kmalloc(nr_free_pages() * PGSIZE + 1) == NULL);
        assert(nr_free_pages() == before);
        assert(kmalloc((size_t)-1) == NULL);
        assert(kmalloc((size_t)-1 - 100) == NULL);
        assert(kmalloc((size_t)1 << 63) == NULL);
        assert(nr_free_pages() == before);
    }

    /* ---------- Case 8：混合随机压力 ---------- */
    {
        size_t before = nr_free_pages();
        size_t live = 0;
        slub_rng_state = 2411433;                                     // 固定种子，可复现
        for (int step = 0; step < 800; step ++) {
            bool do_alloc = (live == 0) || (live < 128 && (slub_rng() % 100) < 55);
            if (do_alloc) {
                uint32_t r = slub_rng() % 100;
                size_t size;
                if (r < 70) {
                    size = 1 + slub_rng() % SLUB_MAX_SIZE;                      // 小对象
                } else if (r < 95) {
                    size = SLUB_MAX_SIZE + 1 + slub_rng() % (2 * PGSIZE - SLUB_MAX_SIZE);   // 大块 1~2 页
                } else {
                    size = 2 * PGSIZE + 1 + slub_rng() % PGSIZE;                // 大块 3 页
                }
                void *p = kmalloc(size);
                slub_check_ptr(p, size);
                slub_recs[live].ptr = p;
                slub_recs[live].size = size;
                slub_recs[live].val = (unsigned char)slub_rng();
                slub_fill(p, size, slub_recs[live].val);
                live ++;
            } else {
                size_t k = slub_rng() % live;
                slub_verify(slub_recs[k].ptr, slub_recs[k].size, slub_recs[k].val);   // 释放前校验内容完整
                kfree(slub_recs[k].ptr);
                slub_recs[k] = slub_recs[live - 1];
                live --;
            }
        }
        slub_check_unique(live);
        for (size_t i = 0; i < live; i ++) {
            slub_verify(slub_recs[i].ptr, slub_recs[i].size, slub_recs[i].val);
            kfree(slub_recs[i].ptr);
        }
        assert(nr_free_pages() == before);
    }

    assert(nr_free_pages() == nr0);
    cprintf("slub_check() succeeded!\n");
}
