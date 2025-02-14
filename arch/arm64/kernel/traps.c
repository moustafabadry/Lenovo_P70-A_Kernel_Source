/*
 * Based on arch/arm/kernel/traps.c
 *
 * Copyright (C) 1995-2009 Russell King
 * Copyright (C) 2012 ARM Ltd.
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/signal.h>
#include <linux/personality.h>
#include <linux/kallsyms.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/hardirq.h>
#include <linux/kdebug.h>
#include <linux/module.h>
#include <linux/kexec.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/syscalls.h>

#include <asm/atomic.h>
#include <asm/debug-monitors.h>
#include <asm/traps.h>
#include <asm/stacktrace.h>
#include <asm/exception.h>
#include <asm/system_misc.h>
#include <asm/cacheflush.h>

static const char *handler[]= {
	"Synchronous Abort",
	"IRQ",
	"FIQ",
	"Error"
};

int show_unhandled_signals = 1;

/*
 * Dump out the contents of some memory nicely...
 */
static void dump_mem(const char *lvl, const char *str, unsigned long bottom,
		     unsigned long top)
{
	unsigned long first;
	mm_segment_t fs;
	int i;

	/*
	 * We need to switch to kernel mode so that we can use __get_user
	 * to safely read from kernel space.  Note that we now dump the
	 * code first, just in case the backtrace kills us.
	 */
	fs = get_fs();
	set_fs(KERNEL_DS);

	printk("%s%s(0x%016lx to 0x%016lx)\n", lvl, str, bottom, top);

	for (first = bottom & ~31; first < top; first += 32) {
		unsigned long p;
		char str[sizeof(" 12345678") * 8 + 1];

		memset(str, ' ', sizeof(str));
		str[sizeof(str) - 1] = '\0';

		for (p = first, i = 0; i < 8 && p < top; i++, p += 4) {
			if (p >= bottom && p < top) {
				unsigned int val;
				if (__get_user(val, (unsigned int *)p) == 0)
					sprintf(str + i * 9, " %08x", val);
				else
					sprintf(str + i * 9, " ????????");
			}
		}
		printk("%s%04lx:%s\n", lvl, first & 0xffff, str);
	}

	set_fs(fs);
}

static void dump_backtrace_entry(unsigned long where, unsigned long stack)
{
	print_ip_sym(where);
	if (in_exception_text(where))
		dump_mem("", "Exception stack", stack,
			 stack + sizeof(struct pt_regs) + 180); /* Additional 180 to workaround sp offset */
}

static void dump_instr(const char *lvl, struct pt_regs *regs)
{
	unsigned long addr = instruction_pointer(regs);
	mm_segment_t fs;
	char str[sizeof("00000000 ") * 5 + 2 + 1], *p = str;
	int i;

	/*
	 * We need to switch to kernel mode so that we can use __get_user
	 * to safely read from kernel space.  Note that we now dump the
	 * code first, just in case the backtrace kills us.
	 */
	fs = get_fs();
	set_fs(KERNEL_DS);

	for (i = -4; i < 1; i++) {
		unsigned int val, bad;

		bad = __get_user(val, &((u32 *)addr)[i]);

		if (!bad)
			p += sprintf(p, i == 0 ? "(%08x) " : "%08x ", val);
		else {
			p += sprintf(p, "bad PC value");
			break;
		}
	}
	printk("%sCode: %s\n", lvl, str);

	set_fs(fs);
}

static void dump_backtrace(struct pt_regs *regs, struct task_struct *tsk)
{
	struct stackframe frame;
	const register unsigned long current_sp asm ("sp");

	pr_debug("%s(regs = %p tsk = %p)\n", __func__, regs, tsk);

	if (!tsk)
		tsk = current;

	if (regs) {
		frame.fp = regs->regs[29];
		frame.sp = regs->sp;
		frame.pc = regs->pc;
	} else if (tsk == current) {
		frame.fp = (unsigned long)__builtin_frame_address(0);
		frame.sp = current_sp;
		frame.pc = (unsigned long)dump_backtrace;
	} else {
		/*
		 * task blocked in __switch_to
		 */
		frame.fp = thread_saved_fp(tsk);
		frame.sp = thread_saved_sp(tsk);
		frame.pc = thread_saved_pc(tsk);
	}

	printk("Call trace:\n");
	while (1) {
		unsigned long where = frame.pc;
		int ret;

		ret = unwind_frame(&frame);
		if (ret < 0)
			break;
		dump_backtrace_entry(where, frame.sp);
	}
}

void show_stack(struct task_struct *tsk, unsigned long *sp)
{
	dump_backtrace(NULL, tsk);
	barrier();
}

#ifdef CONFIG_PREEMPT
#define S_PREEMPT " PREEMPT"
#else
#define S_PREEMPT ""
#endif
#ifdef CONFIG_SMP
#define S_SMP " SMP"
#else
#define S_SMP ""
#endif

static int __die(const char *str, int err, struct thread_info *thread,
		 struct pt_regs *regs)
{
	unsigned long sp, stack;
	struct task_struct *tsk = thread->task;
	static int die_counter;
	int ret;

	pr_emerg("Internal error: %s: %x [#%d]" S_PREEMPT S_SMP "\n",
		 str, err, ++die_counter);

	/* trap and error numbers are mostly meaningless on ARM */
	ret = notify_die(DIE_OOPS, str, regs, err, 0, SIGSEGV);
	if (ret == NOTIFY_STOP)
		return ret;

	print_modules();
	__show_regs(regs);
	pr_emerg("Process %.*s (pid: %d, stack limit = 0x%p)\n",
		 TASK_COMM_LEN, tsk->comm, task_pid_nr(tsk), thread + 1);

	if (!user_mode(regs) || in_interrupt()) {
		sp = regs->sp;
		stack = (unsigned long)task_stack_page(tsk);
		dump_mem(KERN_EMERG, "Stack: ", sp, ALIGN(sp, THREAD_SIZE));
		if (sp < stack || (sp - stack) > THREAD_SIZE) {
			printk(KERN_EMERG "Invalid sp[%lx] or stack address[%lx]\n", sp, stack);
			dump_mem(KERN_EMERG, "Stack(backup) ", stack, THREAD_SIZE + stack);
		}
		dump_backtrace(regs, tsk);
		dump_instr(KERN_EMERG, regs);
	}

	return ret;
}

static DEFINE_RAW_SPINLOCK(die_lock);

/*
 * This function is protected against re-entrancy.
 */
void die(const char *str, struct pt_regs *regs, int err)
{
	struct thread_info *thread = current_thread_info();
	int ret;

	oops_enter();

	raw_spin_lock_irq(&die_lock);
	console_verbose();
	bust_spinlocks(1);
	ret = __die(str, err, thread, regs);

	if (regs && kexec_should_crash(thread->task))
		crash_kexec(regs);

	bust_spinlocks(0);
	add_taint(TAINT_DIE, LOCKDEP_NOW_UNRELIABLE);
	/* keep preemption/irq disabled in KE flow to prevent context switch*/
	//raw_spin_unlock_irq(&die_lock);
	oops_exit();

	if (in_interrupt())
		panic("Fatal exception in interrupt");
	if (panic_on_oops)
		panic("Fatal exception");
	if (ret != NOTIFY_STOP)
		do_exit(SIGSEGV);
}

void arm64_notify_die(const char *str, struct pt_regs *regs,
		      struct siginfo *info, int err)
{
	if (user_mode(regs))
		force_sig_info(info->si_signo, info, current);
	else
		die(str, regs, err);
}

static LIST_HEAD(undef_hook);

void register_undef_hook(struct undef_hook *hook)
{
	list_add(&hook->node, &undef_hook);
}

static int call_undef_hook(struct pt_regs *regs, unsigned int instr)
{
	struct undef_hook *hook;
	int (*fn)(struct pt_regs *regs, unsigned int instr) = NULL;

	list_for_each_entry(hook, &undef_hook, node)
		if ((instr & hook->instr_mask) == hook->instr_val &&
		    (regs->pstate & hook->pstate_mask) == hook->pstate_val)
			fn = hook->fn;

	return fn ? fn(regs, instr) : 1;
}

static DEFINE_PER_CPU(void *, __prev_undefinstr_pc) = 0;
static DEFINE_PER_CPU(int, __prev_undefinstr_counter) = 0;

asmlinkage void __exception do_undefinstr(struct pt_regs *regs)
{
	u32 instr;
	siginfo_t info;
	void __user *pc = (void __user *)instruction_pointer(regs);
	struct thread_info *thread = current_thread_info();

	/* check for AArch32 breakpoint instructions */
	if (!aarch32_break_handler(regs))
		return;
	if (user_mode(regs)) {
		if (compat_thumb_mode(regs)) {
			if (get_user(instr, (u16 __user *)pc))
				goto die_sig;
			if (is_wide_instruction(instr)) {
				u32 instr2;
				if (get_user(instr2, (u16 __user *)pc+1))
					goto die_sig;
				instr <<= 16;
				instr |= instr2;
			}
		} else if (get_user(instr, (u32 __user *)pc)) {
			goto die_sig;
		}
	} else {
		/* kernel mode */
		instr = *((u32 *)pc);
	}

	if (call_undef_hook(regs, instr) == 0)
		return;

die_sig:
	if (show_unhandled_signals && unhandled_signal(current, SIGILL) &&
	    printk_ratelimit()) {
		pr_info("%s[%d]: undefined instruction: pc=%p\n",
			current->comm, task_pid_nr(current), pc);
		dump_instr(KERN_INFO, regs);
	}

	/* Place the SIGILL ICache Invalidate after the Debugger Undefined-Instruction Solution. */
	if ((user_mode(regs)) || processor_mode(regs) == PSR_MODE_EL1h) {
		void **prev_undefinstr_pc = &get_cpu_var(__prev_undefinstr_pc);
		int *prev_undefinstr_counter = &get_cpu_var(__prev_undefinstr_counter);

                /* Only do it for User-Space Application. */
		pr_alert("USR_MODE / SVC_MODE Undefined Instruction Address curr:%p pc=%p:%p, instr: 0x%x compat: %s\n",
			(void *)current, (void *)pc, (void *)*prev_undefinstr_pc, instr,
			is_compat_task() ? "yes" : "no");
		if ((*prev_undefinstr_pc != pc)) {
			/* If the current process or program counter is changed......renew the counter. */
			pr_alert("First Time Recovery curr:%p pc=%p:%p\n",
				(void *)current, (void *)pc, (void *)*prev_undefinstr_pc);
			*prev_undefinstr_pc = pc;
			*prev_undefinstr_counter = 0;
			put_cpu_var(__prev_undefinstr_pc);
			put_cpu_var(__prev_undefinstr_counter);
			__flush_icache_all();
			flush_cache_all();
			/* 
			 * undo cpu_excp to cancel nest_panic code, see entry.S
			 */
			if (!user_mode(regs)) {
				thread->cpu_excp--;
			}
			return;
		}
		else if(*prev_undefinstr_counter < 1) {
			pr_alert("2nd Time Recovery curr:%p pc=%p:%p\n",
				(void *)current, (void *)pc,
				(void *)*prev_undefinstr_pc);
			*prev_undefinstr_counter += 1;
			put_cpu_var(__prev_undefinstr_pc);
			put_cpu_var(__prev_undefinstr_counter);
			__flush_icache_all();
			flush_cache_all();
			/* 
			 * undo cpu_excp to cancel nest_panic code, see entry.S
			 */
			if (!user_mode(regs)) {
				thread->cpu_excp--;
			}
			return;
		}
		*prev_undefinstr_counter += 1;
		if(*prev_undefinstr_counter >= 4) {
			/* 2=first time SigILL,3=2nd time NE-SigILL,4=3rd time CoreDump-SigILL */
			*prev_undefinstr_pc = 0;
			*prev_undefinstr_counter = 0;
		}
		put_cpu_var(__prev_undefinstr_pc);
		put_cpu_var(__prev_undefinstr_counter);
		pr_alert("Go to ARM Notify Die curr:%p pc=%p:%p\n",
			(void *)current, (void *)pc, (void *)*prev_undefinstr_pc);
	}

	info.si_signo = SIGILL;
	info.si_errno = 0;
	info.si_code  = ILL_ILLOPC;
	info.si_addr  = pc;

	arm64_notify_die("Oops - undefined instruction", regs, &info, 0);
}

long compat_arm_syscall(struct pt_regs *regs);

asmlinkage long do_ni_syscall(struct pt_regs *regs)
{
#ifdef CONFIG_COMPAT
	long ret;
	if (is_compat_task()) {
		ret = compat_arm_syscall(regs);
		if (ret != -ENOSYS)
			return ret;
	}
#endif

	if (show_unhandled_signals && printk_ratelimit()) {
		pr_info("%s[%d]: syscall %d\n", current->comm,
			task_pid_nr(current), (int)regs->syscallno);
		dump_instr("", regs);
		if (user_mode(regs))
			__show_regs(regs);
	}

	return sys_ni_syscall();
}

#ifdef CONFIG_MEDIATEK_SOLUTION
static void (*async_abort_handler)(struct pt_regs *regs, void *);
static void *async_abort_priv;

int register_async_abort_handler(void (*fn)(struct pt_regs *regs, void *), void *priv)
{
	async_abort_handler = fn;
	async_abort_priv = priv;

	return 0;
}
#endif

/*
 * bad_mode handles the impossible case in the exception vector. This is always
 * fatal.
 */
asmlinkage void bad_mode(struct pt_regs *regs, int reason, unsigned int esr)
{
	//siginfo_t info;
	//void __user *pc = (void __user *)instruction_pointer(regs);
	console_verbose();

#ifdef CONFIG_MEDIATEK_SOLUTION
	/*
	 * reason is defined in entry.S, 3 means BAD_ERROR,
	 * which would be triggered by async abort
	 */
	if ((reason == 3) && async_abort_handler) {
		async_abort_handler(regs, async_abort_priv);
	}
#endif
	pr_crit("Bad mode in %s handler detected, code 0x%08x\n",
		handler[reason], esr);

	die("Oops - bad mode", regs, 0);
	local_irq_disable();
	panic("bad mode");
}

/*
 * bad_el0_sync handles unexpected, but potentially recoverable synchronous
 * exceptions taken from EL0. Unlike bad_mode, this returns.
 */
asmlinkage void bad_el0_sync(struct pt_regs *regs, int reason, unsigned int esr)
{
	siginfo_t info;
	void __user *pc = (void __user *)instruction_pointer(regs);
	console_verbose();

	pr_crit("Bad EL0 synchronous exception detected on CPU%d, code 0x%08x\n",
		smp_processor_id(), esr);
	__show_regs(regs);

	info.si_signo = SIGILL;
	info.si_errno = 0;
	info.si_code  = ILL_ILLOPC;
	info.si_addr  = pc;

	force_sig_info(info.si_signo, &info, current);
}

void __pte_error(const char *file, int line, unsigned long val)
{
	printk("%s:%d: bad pte %016lx.\n", file, line, val);
}

void __pmd_error(const char *file, int line, unsigned long val)
{
	printk("%s:%d: bad pmd %016lx.\n", file, line, val);
}

void __pgd_error(const char *file, int line, unsigned long val)
{
	printk("%s:%d: bad pgd %016lx.\n", file, line, val);
}

void __init trap_init(void)
{
	return;
}
