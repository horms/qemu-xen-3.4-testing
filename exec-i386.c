/*
 *  i386 emulator main execution loop
 * 
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "exec-i386.h"
#include "disas.h"

//#define DEBUG_EXEC
//#define DEBUG_SIGNAL

/* main execution loop */

/* thread support */

spinlock_t global_cpu_lock = SPIN_LOCK_UNLOCKED;

void cpu_lock(void)
{
    spin_lock(&global_cpu_lock);
}

void cpu_unlock(void)
{
    spin_unlock(&global_cpu_lock);
}

/* exception support */
/* NOTE: not static to force relocation generation by GCC */
void raise_exception_err(int exception_index, int error_code)
{
    /* NOTE: the register at this point must be saved by hand because
       longjmp restore them */
#ifdef __sparc__
	/* We have to stay in the same register window as our caller,
	 * thus this trick.
	 */
	__asm__ __volatile__("restore\n\t"
			     "mov\t%o0, %i0");
#endif
#ifdef reg_EAX
    env->regs[R_EAX] = EAX;
#endif
#ifdef reg_ECX
    env->regs[R_ECX] = ECX;
#endif
#ifdef reg_EDX
    env->regs[R_EDX] = EDX;
#endif
#ifdef reg_EBX
    env->regs[R_EBX] = EBX;
#endif
#ifdef reg_ESP
    env->regs[R_ESP] = ESP;
#endif
#ifdef reg_EBP
    env->regs[R_EBP] = EBP;
#endif
#ifdef reg_ESI
    env->regs[R_ESI] = ESI;
#endif
#ifdef reg_EDI
    env->regs[R_EDI] = EDI;
#endif
    env->exception_index = exception_index;
    env->error_code = error_code;
    longjmp(env->jmp_env, 1);
}

/* short cut if error_code is 0 or not present */
void raise_exception(int exception_index)
{
    raise_exception_err(exception_index, 0);
}

int cpu_x86_exec(CPUX86State *env1)
{
    int saved_T0, saved_T1, saved_A0;
    CPUX86State *saved_env;
#ifdef reg_EAX
    int saved_EAX;
#endif
#ifdef reg_ECX
    int saved_ECX;
#endif
#ifdef reg_EDX
    int saved_EDX;
#endif
#ifdef reg_EBX
    int saved_EBX;
#endif
#ifdef reg_ESP
    int saved_ESP;
#endif
#ifdef reg_EBP
    int saved_EBP;
#endif
#ifdef reg_ESI
    int saved_ESI;
#endif
#ifdef reg_EDI
    int saved_EDI;
#endif
    int code_gen_size, ret, code_size;
    void (*gen_func)(void);
    TranslationBlock *tb, **ptb;
    uint8_t *tc_ptr, *cs_base, *pc;
    unsigned int flags;

    /* first we save global registers */
    saved_T0 = T0;
    saved_T1 = T1;
    saved_A0 = A0;
    saved_env = env;
    env = env1;
#ifdef reg_EAX
    saved_EAX = EAX;
    EAX = env->regs[R_EAX];
#endif
#ifdef reg_ECX
    saved_ECX = ECX;
    ECX = env->regs[R_ECX];
#endif
#ifdef reg_EDX
    saved_EDX = EDX;
    EDX = env->regs[R_EDX];
#endif
#ifdef reg_EBX
    saved_EBX = EBX;
    EBX = env->regs[R_EBX];
#endif
#ifdef reg_ESP
    saved_ESP = ESP;
    ESP = env->regs[R_ESP];
#endif
#ifdef reg_EBP
    saved_EBP = EBP;
    EBP = env->regs[R_EBP];
#endif
#ifdef reg_ESI
    saved_ESI = ESI;
    ESI = env->regs[R_ESI];
#endif
#ifdef reg_EDI
    saved_EDI = EDI;
    EDI = env->regs[R_EDI];
#endif
    
    /* put eflags in CPU temporary format */
    CC_SRC = env->eflags & (CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C);
    DF = 1 - (2 * ((env->eflags >> 10) & 1));
    CC_OP = CC_OP_EFLAGS;
    env->eflags &= ~(DF_MASK | CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C);
    env->interrupt_request = 0;

    /* prepare setjmp context for exception handling */
    if (setjmp(env->jmp_env) == 0) {
        for(;;) {
            if (env->interrupt_request) {
                raise_exception(EXCP_INTERRUPT);
            }
#ifdef DEBUG_EXEC
            if (loglevel) {
                /* XXX: save all volatile state in cpu state */
                /* restore flags in standard format */
                env->regs[R_EAX] = EAX;
                env->regs[R_EBX] = EBX;
                env->regs[R_ECX] = ECX;
                env->regs[R_EDX] = EDX;
                env->regs[R_ESI] = ESI;
                env->regs[R_EDI] = EDI;
                env->regs[R_EBP] = EBP;
                env->regs[R_ESP] = ESP;
                env->eflags = env->eflags | cc_table[CC_OP].compute_all() | (DF & DF_MASK);
                cpu_x86_dump_state(env, logfile, 0);
                env->eflags &= ~(DF_MASK | CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C);
            }
#endif
            /* we compute the CPU state. We assume it will not
               change during the whole generated block. */
            flags = env->seg_cache[R_CS].seg_32bit << GEN_FLAG_CODE32_SHIFT;
            flags |= env->seg_cache[R_SS].seg_32bit << GEN_FLAG_SS32_SHIFT;
            flags |= (((unsigned long)env->seg_cache[R_DS].base | 
                       (unsigned long)env->seg_cache[R_ES].base |
                       (unsigned long)env->seg_cache[R_SS].base) != 0) << 
                GEN_FLAG_ADDSEG_SHIFT;
            if (!(env->eflags & VM_MASK)) {
                flags |= (env->segs[R_CS] & 3) << GEN_FLAG_CPL_SHIFT;
            } else {
                /* NOTE: a dummy CPL is kept */
                flags |= (1 << GEN_FLAG_VM_SHIFT);
                flags |= (3 << GEN_FLAG_CPL_SHIFT);
            }
            flags |= (env->eflags & IOPL_MASK) >> (12 - GEN_FLAG_IOPL_SHIFT);
            flags |= (env->eflags & TF_MASK) << (GEN_FLAG_TF_SHIFT - 8);
            cs_base = env->seg_cache[R_CS].base;
            pc = cs_base + env->eip;
            tb = tb_find(&ptb, (unsigned long)pc, (unsigned long)cs_base, 
                         flags);
            if (!tb) {
                /* if no translated code available, then translate it now */
                /* very inefficient but safe: we lock all the cpus
                   when generating code */
                spin_lock(&tb_lock);
                tc_ptr = code_gen_ptr;
                ret = cpu_x86_gen_code(code_gen_ptr, CODE_GEN_MAX_SIZE, 
                                       &code_gen_size, pc, cs_base, flags,
                                       &code_size);
                /* if invalid instruction, signal it */
                if (ret != 0) {
                    spin_unlock(&tb_lock);
                    raise_exception(EXCP06_ILLOP);
                }
                tb = tb_alloc((unsigned long)pc, code_size);
                *ptb = tb;
                tb->cs_base = (unsigned long)cs_base;
                tb->flags = flags;
                tb->tc_ptr = tc_ptr;
                tb->hash_next = NULL;
                code_gen_ptr = (void *)(((unsigned long)code_gen_ptr + code_gen_size + CODE_GEN_ALIGN - 1) & ~(CODE_GEN_ALIGN - 1));
                spin_unlock(&tb_lock);
            }
#ifdef DEBUG_EXEC
	    if (loglevel) {
		fprintf(logfile, "Trace 0x%08lx [0x%08lx] %s\n",
			(long)tb->tc_ptr, (long)tb->pc,
			lookup_symbol((void *)tb->pc));
	    }
#endif
            /* execute the generated code */
            tc_ptr = tb->tc_ptr;
            gen_func = (void *)tc_ptr;
#ifdef __sparc__
	    __asm__ __volatile__("call	%0\n\t"
				 " mov	%%o7,%%i0"
				 : /* no outputs */
				 : "r" (gen_func)
				 : "i0", "i1", "i2", "i3", "i4", "i5");
#else
            gen_func();
#endif
        }
    }
    ret = env->exception_index;

    /* restore flags in standard format */
    env->eflags = env->eflags | cc_table[CC_OP].compute_all() | (DF & DF_MASK);

    /* restore global registers */
#ifdef reg_EAX
    EAX = saved_EAX;
#endif
#ifdef reg_ECX
    ECX = saved_ECX;
#endif
#ifdef reg_EDX
    EDX = saved_EDX;
#endif
#ifdef reg_EBX
    EBX = saved_EBX;
#endif
#ifdef reg_ESP
    ESP = saved_ESP;
#endif
#ifdef reg_EBP
    EBP = saved_EBP;
#endif
#ifdef reg_ESI
    ESI = saved_ESI;
#endif
#ifdef reg_EDI
    EDI = saved_EDI;
#endif
    T0 = saved_T0;
    T1 = saved_T1;
    A0 = saved_A0;
    env = saved_env;
    return ret;
}

void cpu_x86_interrupt(CPUX86State *s)
{
    s->interrupt_request = 1;
}


void cpu_x86_load_seg(CPUX86State *s, int seg_reg, int selector)
{
    CPUX86State *saved_env;

    saved_env = env;
    env = s;
    load_seg(seg_reg, selector);
    env = saved_env;
}

#undef EAX
#undef ECX
#undef EDX
#undef EBX
#undef ESP
#undef EBP
#undef ESI
#undef EDI
#undef EIP
#include <signal.h>
#include <sys/ucontext.h>

/* 'pc' is the host PC at which the exception was raised. 'address' is
   the effective address of the memory exception. 'is_write' is 1 if a
   write caused the exception and otherwise 0'. 'old_set' is the
   signal set which should be restored */
static inline int handle_cpu_signal(unsigned long pc,
                                    unsigned long address,
                                    int is_write,
                                    sigset_t *old_set)
{
#if defined(DEBUG_SIGNAL)
    printf("qemu: SIGSEGV pc=0x%08lx address=%08lx wr=%d oldset=0x%08lx\n", 
           pc, address, is_write, *(unsigned long *)old_set);
#endif
    /* XXX: locking issue */
    if (is_write && page_unprotect(address)) {
        sigprocmask(SIG_SETMASK, old_set, NULL);
        return 1;
    }
    if (pc >= (unsigned long)code_gen_buffer &&
        pc < (unsigned long)code_gen_buffer + CODE_GEN_BUFFER_SIZE) {
        /* the PC is inside the translated code. It means that we have
           a virtual CPU fault */
        /* we restore the process signal mask as the sigreturn should
           do it */
        sigprocmask(SIG_SETMASK, old_set, NULL);
        /* XXX: need to compute virtual pc position by retranslating
           code. The rest of the CPU state should be correct. */
        env->cr2 = address;
        raise_exception_err(EXCP0E_PAGE, 4 | (is_write << 1));
        /* never comes here */
        return 1;
    } else {
        return 0;
    }
}

int cpu_x86_signal_handler(int host_signum, struct siginfo *info, 
                           void *puc)
{
#if defined(__i386__)
    struct ucontext *uc = puc;
    unsigned long pc;
    sigset_t *pold_set;
    
#ifndef REG_EIP
/* for glibc 2.1 */
#define REG_EIP    EIP
#define REG_ERR    ERR
#define REG_TRAPNO TRAPNO
#endif
    pc = uc->uc_mcontext.gregs[REG_EIP];
    pold_set = &uc->uc_sigmask;
    return handle_cpu_signal(pc, (unsigned long)info->si_addr, 
                             uc->uc_mcontext.gregs[REG_TRAPNO] == 0xe ? 
                             (uc->uc_mcontext.gregs[REG_ERR] >> 1) & 1 : 0,
                             pold_set);
#elif defined(__powerpc)
    struct ucontext *uc = puc;
    struct pt_regs *regs = uc->uc_mcontext.regs;
    unsigned long pc;
    sigset_t *pold_set;
    int is_write;

    pc = regs->nip;
    pold_set = &uc->uc_sigmask;
    is_write = 0;
#if 0
    /* ppc 4xx case */
    if (regs->dsisr & 0x00800000)
        is_write = 1;
#else
    if (regs->trap != 0x400 && (regs->dsisr & 0x02000000))
        is_write = 1;
#endif
    return handle_cpu_signal(pc, (unsigned long)info->si_addr, 
                             is_write, pold_set);
#else
#error CPU specific signal handler needed
    return 0;
#endif
}
