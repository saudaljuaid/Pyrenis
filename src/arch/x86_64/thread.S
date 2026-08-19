/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * The context switch, and the place a new thread starts.
 *
 * This is in assembly because both halves are statements about the ABI that C
 * cannot make. A C function is entitled to assume it returns on the stack it
 * was called on; this one deliberately does not, and the compiler must not be
 * allowed to reason about it. And the set of registers a switch has to preserve
 * is fixed by the System V AMD64 ABI rather than by anything the compiler would
 * tell us.
 */

.section .text, "ax"
.code64

/*
 * void thread_switch_context(uint64_t *save_stack_pointer,
 *                            uint64_t load_stack_pointer)
 *
 * System V AMD64 makes RBX, RBP and R12 through R15 callee-saved, so those are
 * exactly the registers that must survive. Everything else is caller-saved:
 * the compiler has already spilled anything it still needs across the call to
 * thread_switch_context, so saving them here would be saving them twice.
 *
 * RFLAGS is saved as well, and that is a deliberate addition rather than an ABI
 * requirement. Two reasons. The direction flag must be clear on entry to any
 * function, so a switch that restored a thread's flags from another thread's
 * would be a lasting corruption rather than a lost value. And the interrupt
 * enable bit lives here: when preemption arrives, a thread must resume with the
 * interrupt state it was suspended with, not with the state of whichever thread
 * happened to run before it. Saving it now means that change adds nothing here.
 *
 * The return address is already on the stack, put there by the call. That is
 * what makes this work: switching the stack pointer switches which return
 * address the final ret will use, so the ret lands in whichever thread this
 * stack belongs to, at the point it last called this function.
 */
.global thread_switch_context
.type thread_switch_context, @function
thread_switch_context:
    pushfq
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /*
     * Publish the outgoing stack pointer only after every register is on that
     * stack. A stack pointer recorded earlier would name a frame that is still
     * being written, and on a preempting kernel something could resume it.
     */
    movq %rsp, (%rdi)
    movq %rsi, %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    popfq
    ret
.size thread_switch_context, . - thread_switch_context

/*
 * void thread_trampoline(void)
 *
 * Never called. A new thread's stack is built so that the first switch onto it
 * pops seven prepared slots and returns here, which is why this needs no
 * prologue and must never be entered any other way.
 *
 * thread.c places the entry function in the saved R12 and its context in R13,
 * so the switch that arrives here has already loaded both - they are restored
 * as ordinary callee-saved registers and cost nothing extra to pass.
 */
.global thread_trampoline
.type thread_trampoline, @function
thread_trampoline:
    /*
     * Terminate the frame chain. This stack has no caller, and a debugger or
     * fault dump walking RBP off the top of it would read the guard page.
     */
    xorq %rbp, %rbp

    /*
     * The ABI requires RSP to be 16-byte aligned immediately before a call.
     * Arriving by ret rather than by call means the usual reasoning about
     * alignment does not apply, so it is forced rather than assumed. Nothing
     * below this point on the stack is live, so discarding those bytes is free.
     */
    andq $-16, %rsp

    movq %r13, %rdi
    callq *%r12

    /*
     * The entry function returned, which is how a thread ends. thread_exit does
     * not return, so there is deliberately no ret here: falling out of this
     * would return to whatever the alignment above left on the stack.
     */
    callq thread_exit
    ud2
.size thread_trampoline, . - thread_trampoline

.section .note.GNU-stack, "", @progbits
