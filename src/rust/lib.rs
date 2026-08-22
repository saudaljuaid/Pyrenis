// SPDX-License-Identifier: GPL-3.0-only
//! The parts of Sapote written in Rust.
//!
//! Sapote is a C kernel. Rust is used here for one specific job: parsing input
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
//! The whole crate compiles with no heap and no operating system. Unsafe code is
//! confined to ABI entry points that turn validated C pointers into Rust slices
//! or write results through validated C pointers.

#![no_std]
#![deny(warnings)]
#![deny(unsafe_op_in_unsafe_fn)]
#![deny(missing_docs)]

pub mod abi;
pub mod elf64;
pub mod fat16;
pub(crate) mod linux_fat16;
pub(crate) mod linux_elf64;
pub mod font;
pub mod logo;
pub mod ui_font;

/// Where a Rust panic goes.
///
/// Nothing in this crate panics: every fallible path returns a status instead,
/// and the crate is built with `panic=abort` so unwinding does not exist. This
/// is the backstop for a bounds check the compiler inserted that this code did
/// not anticipate - which is exactly the class of bug Rust is here to convert
/// from silent corruption into a stop.
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    abi::panic()
}
