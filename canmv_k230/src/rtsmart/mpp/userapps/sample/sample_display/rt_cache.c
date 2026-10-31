#include "rt_cache.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>

void *rt_memset(void *s, int c, rt_ubase_t count) {
    return memset(s, c, count);
}

void *rt_memcpy(void *dst, const void *src, rt_ubase_t count) {
    return memcpy(dst, src, count);
}

rt_sem_t rt_sem_create(const char *name, rt_uint32_t value, rt_uint8_t flag) {
    (void)name;
    (void)flag;
    sem_t *s = (sem_t *)calloc(1, sizeof(sem_t));
    if (s) sem_init(s, 0, value);
    return (rt_sem_t)s;
}

rt_err_t rt_sem_delete(rt_sem_t sem) {
    if (sem) {
        sem_destroy((sem_t *)sem);
        free(sem);
    }
    return RT_EOK;
}

rt_err_t rt_sem_take(rt_sem_t sem, rt_int32_t time) {
    (void)time;
    if (!sem) return -RT_ERROR;
    return sem_wait((sem_t *)sem) == 0 ? RT_EOK : -RT_ERROR;
}

rt_err_t rt_sem_release(rt_sem_t sem) {
    if (!sem) return -RT_ERROR;
    return sem_post((sem_t *)sem) == 0 ? RT_EOK : -RT_ERROR;
}

rt_mutex_t rt_mutex_create(const char *name, rt_uint8_t flag) {
    (void)name;
    (void)flag;
    pthread_mutex_t *m = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
    if (m) pthread_mutex_init(m, NULL);
    return (rt_mutex_t)m;
}

rt_err_t rt_mutex_delete(rt_mutex_t mutex) {
    if (mutex) {
        pthread_mutex_destroy((pthread_mutex_t *)mutex);
        free(mutex);
    }
    return RT_EOK;
}

rt_err_t rt_mutex_take(rt_mutex_t mutex, rt_int32_t time) {
    (void)time;
    if (!mutex) return -RT_ERROR;
    return pthread_mutex_lock((pthread_mutex_t *)mutex) == 0 ? RT_EOK : -RT_ERROR;
}

rt_err_t rt_mutex_release(rt_mutex_t mutex) {
    if (!mutex) return -RT_ERROR;
    return pthread_mutex_unlock((pthread_mutex_t *)mutex) == 0 ? RT_EOK : -RT_ERROR;
}

rt_err_t rt_thread_mdelay(rt_int32_t ms) {
    if (ms > 0) usleep((useconds_t)ms * 1000);
    return RT_EOK;
}

typedef struct {
    void (*entry)(void *);
    void *parameter;
} rtt_pthread_wrap_t;

static void *rtt_pthread_trampoline(void *arg) {
    rtt_pthread_wrap_t *w = (rtt_pthread_wrap_t *)arg;
    void (*entry)(void *) = w->entry;
    void *param = w->parameter;
    free(w);
    entry(param);
    return NULL;
}

rt_thread_t rt_thread_create(const char *name, void (*entry)(void *parameter),
                             void *parameter, rt_uint32_t stack_size,
                             rt_uint8_t priority, rt_uint32_t tick) {
    (void)name;
    (void)stack_size;
    (void)priority;
    (void)tick;
    rtt_pthread_wrap_t *w = (rtt_pthread_wrap_t *)calloc(1, sizeof(rtt_pthread_wrap_t));
    if (!w) return RT_NULL;
    w->entry = entry;
    w->parameter = parameter;
    return (rt_thread_t)w;
}

rt_err_t rt_thread_startup(rt_thread_t thread) {
    if (!thread) return -RT_ERROR;
    pthread_t tid;
    if (pthread_create(&tid, NULL, rtt_pthread_trampoline, (void *)thread) == 0) {
        pthread_detach(tid);
        return RT_EOK;
    }
    free(thread);
    return -RT_ERROR;
}

void cache_init(pkg_cache_t *cache) {
    rt_memset(cache->buffer, 0, sizeof(cache->buffer));
    cache->wr_idx = cache->rd_idx = 0;
    cache->sem = rt_sem_create("cache_sem", 0, RT_IPC_FLAG_FIFO);
    cache->lock = rt_mutex_create("cache_lock", RT_IPC_FLAG_FIFO);
}

rt_err_t cache_write(pkg_cache_t *cache, const void *data) {
    rt_mutex_take(cache->lock, RT_WAITING_FOREVER);
    if (((cache->wr_idx + 1) % PKG_NUM) == cache->rd_idx) {
        //rt_sem_release(cache->sem);
        rt_mutex_release(cache->lock);
        //rt_kprintf("cache_write err\n");
        return -RT_EFULL;
    }
    rt_memcpy(cache->buffer[cache->wr_idx], data, PKG_SIZE);
    cache->wr_idx = (cache->wr_idx + 1) % PKG_NUM;    
     rt_sem_release(cache->sem);  // 先发信号量
    rt_mutex_release(cache->lock); // 后放锁
    return RT_EOK;
}

rt_err_t cache_read(pkg_cache_t *cache, void *output) {
    rt_sem_take(cache->sem, RT_WAITING_FOREVER);
    rt_mutex_take(cache->lock, RT_WAITING_FOREVER);
    rt_memcpy(output, cache->buffer[cache->rd_idx], PKG_SIZE);
    cache->rd_idx = (cache->rd_idx + 1) % PKG_NUM;
    rt_mutex_release(cache->lock);
    return RT_EOK;
}

void cache_deinit(pkg_cache_t *cache) {
    rt_mutex_take(cache->lock, RT_WAITING_FOREVER);
    rt_sem_delete(cache->sem);
    rt_mutex_release(cache->lock);
    rt_mutex_delete(cache->lock);
}


// 添加强制恢复函数
void cache_force_recover(pkg_cache_t *cache) {
    rt_mutex_take(cache->lock, RT_WAITING_FOREVER);
    // 重置缓冲区状态
    cache->wr_idx = 0;
    cache->rd_idx = 0;
    rt_memset(cache->buffer, 0, sizeof(cache->buffer));
    
    // 释放可能阻塞的读取线程
    //while (rt_sem_take(cache->sem,10) == RT_EOK) {
        // 清空所有信号量
         rt_sem_release(cache->sem);  // 先发信号量
    //}
    rt_mutex_release(cache->lock);
}

