// SPDX-License-Identifier: GPL-3.0-only
//! Host harness for the bounded BusyBox FAT16 parser.

// This harness imports the complete inherited module while exercising only the
// new chain API; the kernel crate and inherited standalone test use the rest.
#![allow(dead_code)]

#[path = "../src/rust/fat16.rs"]
mod fat16;
#[path = "../src/rust/linux_fat16.rs"]
mod linux_fat16;
