// SPDX-License-Identifier: GPL-3.0-only
//! The parts of Seneri written in Rust.
//!
//! Seneri is a C kernel. Rust is used here for one specific job: parsing input
//! this kernel did not produce. A bounds check that the compiler inserts and
//! cannot be talked out of is worth more on a byte stream from outside than
//! anywhere else, because that is where a missing one becomes an attacker's
//! primitive rather than a bug.
//!
//! It is deliberately *not* used for the layers that talk to hardware. Page
//! tables, port I/O and the context switch are unsafe operations by nature:
//! writing them in Rust would wrap every line in `unsafe` and buy nothing but
//! a second language in the boot path. `docs/RUST.md` argues that split.
//!
//! The whole crate compiles with no heap, no operating system, and exactly one
//! `unsafe` block - the place a C pointer becomes a Rust slice.

#![no_std]
#![deny(warnings)]
#![deny(unsafe_op_in_unsafe_fn)]
#![deny(missing_docs)]

pub mod abi;
pub mod font;
pub mod logo;

/// Where a Rust panic goes.
///
/// Nothing in this crate panics: every fallible path returns a status instead,
/// and the crate is built with `panic=abort` so unwinding does not exist. This
/// is the backstop for a bounds check the compiler inserted that this code did
/// not anticipate - which is exactly the class of bug Rust is here to convert
/// from silent corruption into a stop.
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    // Declared in src/kernel/console.c. Never returns.
    unsafe extern "C" {
        fn console_panic(message: *const u8) -> !;
    }

    // SAFETY: a static, NUL-terminated string, and console_panic never returns.
    unsafe { console_panic(c"Rust panicked".as_ptr() as *const u8) }
}
