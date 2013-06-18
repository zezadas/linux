#ifndef __ASMARM_TLS_H
#define __ASMARM_TLS_H

#include <linux/compiler.h>
#include <asm/thread_info.h>

#ifdef __ASSEMBLY__
	.macro set_tls_none, tp, tmp1, tmp2
	.endm

	.macro set_tls_v6k, tp, tmp1, tmp2
	mcr	p15, 0, \tp, c13, c0, 3		@ set TLS register
	mov	\tmp1, #0
	mcr	p15, 0, \tmp1, c13, c0, 2	@ clear user r/w TLS register
	.endm

	.macro set_tls_v6, tp, tmp1, tmp2
	ldr	\tmp1, =elf_hwcap
	ldr	\tmp1, [\tmp1, #0]
	mov	\tmp2, #0xffff0fff
	tst	\tmp1, #HWCAP_TLS		@ hardware TLS available?
	mcrne	p15, 0, \tp, c13, c0, 3		@ yes, set TLS register
	movne	\tmp1, #0
	mcrne	p15, 0, \tmp1, c13, c0, 2	@ clear user r/w TLS register
	streq	\tp, [\tmp2, #-15]		@ set TLS value at 0xffff0ff0
	.endm

	.macro set_tls_software, tp, tmp1, tmp2
	mov	\tmp1, #0xffff0fff
	str	\tp, [\tmp1, #-15]		@ set TLS value at 0xffff0ff0
	.endm
#endif

#ifdef CONFIG_TLS_REG_EMUL
#define tls_emu		1
#define has_tls_reg		1
#define set_tls		set_tls_none
#elif defined(CONFIG_CPU_V6)
#define tls_emu		0
#define has_tls_reg		(elf_hwcap & HWCAP_TLS)
#define set_tls		set_tls_v6
#elif defined(CONFIG_CPU_32v6K)
#define tls_emu		0
#define has_tls_reg		1
#define set_tls		set_tls_v6k
#else
#define tls_emu		0
#define has_tls_reg		0
#define set_tls		set_tls_software
#endif

#ifndef __ASSEMBLY__

static inline void set_tls(unsigned long val)
{
	struct thread_info *thread;

	thread = current_thread_info();

	thread->tp_value = val;

	/*
	 * This code runs with preemption enabled and therefore must
	 * be reentrant with respect to switch_tls.
	 *
	 * We need to ensure ordering between the shadow state and the
	 * hardware state, so that we don't corrupt the hardware state
	 * with a stale shadow state during context switch.
	 *
	 * If we're preempted here, switch_tls will load TPIDRURO from
	 * thread_info upon resuming execution and the following mcr
	 * is merely redundant.
	 */
	barrier();

	if (!tls_emu) {
		if (has_tls_reg) {
			asm("mcr p15, 0, %0, c13, c0, 3"
			    : : "r" (val));
		} else {
#ifdef CONFIG_KUSER_HELPERS
			/*
			 * User space must never try to access this
			 * directly.  Expect your app to break
			 * eventually if you do so.  The user helper
			 * at 0xffff0fe0 must be used instead.  (see
			 * entry-armv.S for details)
			 */
			*((unsigned int *)0xffff0ff0) = val;
#endif
		}

	}
}

static inline unsigned long get_tpuser(void)
{
	unsigned long reg = 0;

	if (has_tls_reg && !tls_emu)
		__asm__("mrc p15, 0, %0, c13, c0, 2" : "=r" (reg));

	return reg;
}

static inline void set_tpuser(unsigned long val)
{
	/* Since TPIDRURW is fully context-switched (unlike TPIDRURO),
	 * we need not update thread_info.
	 */
	if (has_tls_reg && !tls_emu) {
		asm("mcr p15, 0, %0, c13, c0, 2"
		    : : "r" (val));
	}
}

static inline void flush_tls(void)
{
	set_tls(0);
	set_tpuser(0);
}

#endif
#endif	/* __ASMARM_TLS_H */
