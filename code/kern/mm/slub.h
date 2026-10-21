#ifndef __KERN_MM_SLUB_H__
#define __KERN_MM_SLUB_H__

#include <defs.h>

/* 两层 SLUB 内存分配器
 *   第一层：以页为单位，直接使用 alloc_pages()/free_pages()
 *   第二层：kmalloc()/kfree() 任意大小分配（<= 2048 字节走 slab，更大的直接按页分配）
 */
void   slub_init(void);
void  *kmalloc(size_t size);
void   kfree(void *ptr);

size_t slub_class_size(size_t size);            // 请求大小对应的分级大小；size == 0 或 > 2048 返回 0
size_t slub_objs_per_slab(size_t class_size);   // 某分级下单个 slab 能容纳的对象数；非法分级返回 0

void   slub_check(void);                        // 自检

#endif /* !__KERN_MM_SLUB_H__ */
