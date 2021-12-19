/*
 * Copyright (C) 2021 Colin Ian King
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */
#include "stress-ng.h"

#define STRESS_SYNCLOAD_MS_DEFAULT	(125)	/* 125 milliseconds */
#define STRESS_SYNCLOAD_MS_MIN		(1)	/* 1 millisecond */
#define STRESS_SYNCLOAD_MS_MAX		(10000)	/* 1 second */

typedef void(* stress_syncload_op_t)(void);

bool stress_sysload_x86_has_rdrand;

static const stress_help_t help[] = {
	{ NULL,	"syncload N",		"start N workers that synchronize load spikes" },
	{ NULL,	"syncload-ops N",	"stop after N syncload bogo operations" },
	{ NULL, "syncload-msbusy M",	"maximum busy duration in milliseconds" },
	{ NULL, "syncload-mssleep M",	"maximum sleep duration in milliseconds" },
	{ NULL,	NULL,			NULL }
};

static int stress_set_syncload_ms(const char *opt, const char *setting)
{
	uint64_t ms;

	ms = stress_get_uint64(opt);
	stress_check_range(setting, ms, STRESS_SYNCLOAD_MS_MIN, STRESS_SYNCLOAD_MS_MAX);
	return stress_set_setting(setting, TYPE_ID_UINT64, &ms);
}

static int stress_set_syncload_msbusy(const char *opt)
{
	return stress_set_syncload_ms(opt, "syncload-msbusy");
}

static int stress_set_syncload_mssleep(const char *opt)
{
	return stress_set_syncload_ms(opt, "syncload-mssleep");
}

static const stress_opt_set_func_t opt_set_funcs[] = {
	{ OPT_syncload_msbusy,	stress_set_syncload_msbusy },
	{ OPT_syncload_mssleep,	stress_set_syncload_mssleep },
	{ 0,			NULL },
};

static void stress_syncload_none(void)
{
	return;
}

static void stress_syncload_nop(void)
{
#if defined(HAVE_ASM_NOP)
	__asm__ __volatile__("nop;\n");
	__asm__ __volatile__("nop;\n");
	__asm__ __volatile__("nop;\n");
	__asm__ __volatile__("nop;\n");
	__asm__ __volatile__("nop;\n");
	__asm__ __volatile__("nop;\n");
	__asm__ __volatile__("nop;\n");
	__asm__ __volatile__("nop;\n");
	__asm__ __volatile__("nop;\n");
#endif
}

#if defined(HAVE_ASM_X86_PAUSE)
static void stress_syncload_pause(void)
{
	__asm__ __volatile__("pause;\n");
}
#endif

#if defined(HAVE_ASM_ARM_YIELD)
static void stress_syncload_yield(void)
{
	__asm__ __volatile__("yield;\n");
}
#endif

#if defined(__x86_64__) || defined(__x86_64)
static void stress_syncload_rdrand(void)
{
	if (stress_sysload_x86_has_rdrand) {
		int64_t        ret;

		asm volatile("1:;\n\
			     rdrand %0;\n\
			     jnc 1b;\n":"=r"(ret));
		return;
	}
	stress_syncload_nop();
}
#endif

static void stress_syncload_sched_yield(void)
{
	shim_sched_yield();
}

static void stress_syncload_mfence(void)
{
	shim_mfence();
}

static void stress_syncload_loop(void)
{
	register int i = 1000;

	while (i--) {
		__asm__ __volatile__("");
	}
}

static const stress_syncload_op_t stress_syncload_ops[] = {
	stress_syncload_none,
	stress_syncload_nop,
#if defined(HAVE_ASM_X86_PAUSE)
	stress_syncload_pause,
#endif
#if defined(HAVE_ASM_ARM_YIELD)
	stress_syncload_yield,
#endif
	stress_syncload_sched_yield,
#if defined(__x86_64__) || defined(__x86_64)
	stress_syncload_rdrand,
#endif
	stress_syncload_mfence,
	stress_syncload_loop,
};

static inline void stress_syncload_settime(void)
{
#if defined(HAVE_ATOMIC_LOAD) &&	\
    defined(HAVE_ATOMIC_STORE) &&	\
    defined(__ATOMIC_CONSUME) &&	\
    defined(__ATOMIC_RELEASE)
		double now = stress_time_now();

		__atomic_store(&g_shared->syncload.start_time, &now, __ATOMIC_RELEASE);
#elif defined(HAVE_LIB_PTHREAD)
	int ret;

	ret = shim_pthread_spin_lock(&g_shared->syncload.lock);
	g_shared->syncload.start_time = stress_time_now();
	shim_mb();
	if (ret == 0) {
		ret = shim_pthread_spin_unlock(&g_shared->syncload.lock);
		(void)ret;
	}
#else
	g_shared->syncload.start_time = stress_time_now();
	shim_mb();
#endif
}

static inline double stress_syncload_gettime(const stress_args_t *args)
{
	double t;

	do {
#if defined(HAVE_ATOMIC_LOAD) &&	\
    defined(HAVE_ATOMIC_STORE) &&	\
    defined(__ATOMIC_CONSUME) &&	\
    defined(__ATOMIC_RELEASE)
		__atomic_load(&t, &g_shared->syncload.start_time, __ATOMIC_CONSUME);
#elif defined(HAVE_LIB_PTHREAD)
		int ret;

		ret = shim_pthread_spin_lock(&g_shared->syncload.lock);
		t = (volatile double)g_shared->syncload.start_time;
		if (ret == 0) {
			ret = shim_pthread_spin_unlock(&g_shared->syncload.lock);
			(void)ret;
		}
#else
		/* Racy version */
		shim_mb();
		t = (volatile double)g_shared->syncload.start_time;
		shim_mb();
#endif
	} while ((t <= 0.0) && keep_stressing(args));

	return t;
}

/*
 *  Add +/- 10% jitter to delays
 */
static double stress_syncload_jitter(const double sec)
{
	switch ((stress_mwc8() >> 3) & 2) {
	case 0:
		return sec / 10;
	case 1:
		return -sec / 10;
	}
	return 0.0;
}

/*
 *  stress_syncload()
 *	stress that does lots of not a lot
 */
static int stress_syncload(const stress_args_t *args)
{
	uint64_t syncload_msbusy = STRESS_SYNCLOAD_MS_DEFAULT;
	uint64_t syncload_mssleep = STRESS_SYNCLOAD_MS_DEFAULT / 2;
	double timeout, sec_busy, sec_sleep;
	size_t delay_type = 0;

	(void)stress_get_setting("syncload-msbusy", &syncload_msbusy);
	(void)stress_get_setting("syncload-mssleep", &syncload_mssleep);

	sec_busy = syncload_msbusy / 1000.0;
	sec_sleep = syncload_mssleep / 1000.0;

	stress_mwc_seed(0x6deb3a92, 0x189f7245);

	stress_sysload_x86_has_rdrand = stress_cpu_x86_has_rdrand();

	if (args->instance == 0)
		stress_syncload_settime();

	timeout = stress_syncload_gettime(args);

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	do {
		const stress_syncload_op_t op = stress_syncload_ops[delay_type];

		delay_type++;
		if (delay_type >= SIZEOF_ARRAY(stress_syncload_ops))
			delay_type = 0;

		timeout += sec_busy + stress_syncload_jitter(sec_busy);
		while (stress_time_now() < timeout)
			op();

		if (!keep_stressing_flag())
			break;

		timeout += sec_sleep + stress_syncload_jitter(sec_sleep);
		if (stress_time_now() < timeout)
			shim_nanosleep_uint64(syncload_mssleep * 1000000);

		inc_counter(args);
	} while (keep_stressing(args));
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	return EXIT_SUCCESS;
}

stressor_info_t stress_syncload_info = {
	.stressor = stress_syncload,
	.class = CLASS_CPU,
	.opt_set_funcs = opt_set_funcs,
	.help = help
};
