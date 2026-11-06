/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>
#include <riscv_io.h>
#include "ioremap.h"
#include "drv_ts.h"
#include "board.h"
#include "drv_hardlock.h"
#ifdef RT_USING_POSIX
#include <dfs_posix.h>
#include <dfs_poll.h>
#include <posix_termios.h>
#endif

#include <rthw.h>

static struct rt_device g_ts_device = {0};
void *ts_base_addr = RT_NULL;
static int ts_hardlock;

rt_size_t ts_device_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    rt_uint32_t *outbuf = (rt_uint32_t *)buffer;
    rt_uint32_t val = 0;
    rt_uint32_t ret = -1;
    void *ts_read_addr = (void *)((char *)ts_base_addr + TS_READ_OFFSET);
    void *ts_config_addr = (void *)((char *)ts_base_addr + TS_CONFIG_OFFSET);

    while(0 != kd_hardlock_lock(ts_hardlock));

    writel(0x22, ts_config_addr);
    writel(0x23, ts_config_addr);
    rt_thread_mdelay(20);
    kd_hardlock_unlock(ts_hardlock);

    int timeout = 50000;
    while(timeout-- > 0)
    {
        val = readl(ts_read_addr);

        if(val >> 12)
        {
            *outbuf = val;
            ret = sizeof(outbuf) / sizeof(outbuf[0]);
            break;
        }
        rt_hw_us_delay(1);
    }
    if (timeout <= 0) {
        *outbuf = 0;
        ret = 0;
    }

    return ret;
}


static struct rt_device g_urandom_device = {0};
static struct rt_device g_random_device = {0};

static rt_size_t urandom_device_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    static rt_uint64_t rng_state = 0x243f6a8885a308d3ULL;
    rt_uint8_t *out = (rt_uint8_t *)buffer;
    rt_uint64_t cycles = 0, timer = 0;
    (void)dev;
    (void)pos;
    if (!out || size == 0)
        return 0;
#if defined(__riscv)
    asm volatile("rdcycle %0" : "=r"(cycles));
    asm volatile("rdtime %0" : "=r"(timer));
#endif
    if (ts_base_addr != RT_NULL) {
        rt_uint32_t ts_noise = readl((void *)((char *)ts_base_addr + TS_READ_OFFSET));
        rng_state ^= ((rt_uint64_t)ts_noise << 48);
    }
    rng_state ^= cycles ^ (timer << 19) ^ ((rt_uint64_t)rt_tick_get() << 32);
    for (rt_size_t i = 0; i < size; i++) {
        if ((i & 7) == 0) {
#if defined(__riscv)
            asm volatile("rdcycle %0" : "=r"(cycles));
#endif
            rt_uint64_t z = (rng_state += 0x9e3779b97f4a7c15ULL ^ (cycles & 0xffULL));
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            rng_state ^= z ^ (z >> 31);
        }
        out[i] = (rt_uint8_t)(rng_state >> ((i & 7) * 8));
    }
    return size;
}

const static struct rt_device_ops urandom_ops =
{
    RT_NULL,
    RT_NULL,
    RT_NULL,
    urandom_device_read,
    RT_NULL,
    RT_NULL
};

const static struct rt_device_ops ts_ops =
{
    RT_NULL,
    RT_NULL,
    RT_NULL,
    ts_device_read,
    RT_NULL,
    RT_NULL
};

int rt_hw_ts_init(void)
{
    rt_err_t ret = RT_EOK;
    rt_device_t ts_device = &g_ts_device;

    ts_base_addr = rt_ioremap((void*)TS_BASE_ADDR, TS_IO_SIZE);
    if(ts_base_addr == RT_NULL) {
        rt_kprintf("ts ioremap error\n");
        return -1;
    }

    ret = rt_device_register(ts_device, "ts", RT_DEVICE_FLAG_RDWR);
    if(ret) {
        rt_kprintf("ts device register fail\n");
        return ret;
    }

    ts_device->ops = &ts_ops;

    if (rt_device_register(&g_urandom_device, "urandom", RT_DEVICE_FLAG_RDONLY) == RT_EOK) {
        g_urandom_device.ops = &urandom_ops;
    }
    if (rt_device_register(&g_random_device, "random", RT_DEVICE_FLAG_RDONLY) == RT_EOK) {
        g_random_device.ops = &urandom_ops;
    }

    if(kd_request_lock(HARDLOCK_TS)) {
        rt_kprintf("fail to request hardlock-%d\n", HARDLOCK_TS);
        ts_hardlock = -1;
    } else
        ts_hardlock = HARDLOCK_TS;
#ifndef RT_FASTBOOT
    if(!ret)
        rt_kprintf("%s OK\n", __func__);
#endif
    return ret;
}
INIT_BOARD_EXPORT(rt_hw_ts_init);
