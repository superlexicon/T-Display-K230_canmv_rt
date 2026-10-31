#ifndef _RT_CACHE_H
#define _RT_CACHE_H
#include <stdint.h>
#include <stddef.h>

typedef uint8_t rt_uint8_t;
typedef uint32_t rt_uint32_t;
typedef int32_t rt_int32_t;
typedef size_t rt_ubase_t;
typedef long rt_err_t;
typedef void *rt_sem_t;
typedef void *rt_mutex_t;
typedef void *rt_thread_t;

#ifndef RT_EOK
#define RT_EOK 0
#define RT_ERROR 1
#define RT_EFULL 3
#define RT_IPC_FLAG_FIFO 0x00
#define RT_IPC_FLAG_PRIO 0x01
#define RT_WAITING_FOREVER (-1)
#define RT_NULL NULL
#endif

#define PKG_SIZE 225
#define PKG_NUM 20

typedef struct {
    rt_uint8_t buffer[PKG_NUM][PKG_SIZE];
    volatile rt_uint32_t wr_idx;
    volatile rt_uint32_t rd_idx;
    rt_sem_t sem;
    rt_mutex_t lock;
} pkg_cache_t;
#ifdef __cplusplus
extern "C" {
#endif
void *rt_memset(void *s, int c, rt_ubase_t count);
void *rt_memcpy(void *dst, const void *src, rt_ubase_t count);
rt_sem_t rt_sem_create(const char *name, rt_uint32_t value, rt_uint8_t flag);
rt_err_t rt_sem_delete(rt_sem_t sem);
rt_err_t rt_sem_take(rt_sem_t sem, rt_int32_t time);
rt_err_t rt_sem_release(rt_sem_t sem);
rt_mutex_t rt_mutex_create(const char *name, rt_uint8_t flag);
rt_err_t rt_mutex_delete(rt_mutex_t mutex);
rt_err_t rt_mutex_take(rt_mutex_t mutex, rt_int32_t time);
rt_err_t rt_mutex_release(rt_mutex_t mutex);
rt_err_t rt_thread_mdelay(rt_int32_t ms);
rt_thread_t rt_thread_create(const char *name, void (*entry)(void *parameter),
                             void *parameter, rt_uint32_t stack_size,
                             rt_uint8_t priority, rt_uint32_t tick);
rt_err_t rt_thread_startup(rt_thread_t thread);
void cache_init(pkg_cache_t *cache);
rt_err_t cache_write(pkg_cache_t *cache, const void *data);
rt_err_t cache_read(pkg_cache_t *cache, void *output);
void cache_deinit(pkg_cache_t *cache);
void cache_force_recover(pkg_cache_t *cache);
#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
