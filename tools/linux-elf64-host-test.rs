// SPDX-License-Identifier: GPL-3.0-only

#[path = "../src/rust/linux_elf64.rs"]
mod linux_elf64;

static BUSYBOX: &[u8] = include_bytes!(env!("SAPOTE_BUSYBOX_BINARY"));

fn changed(offset: usize, value: u8, expected: linux_elf64::Status) {
    let mut image = BUSYBOX.to_vec();
    image[offset] = value;
    assert_eq!(linux_elf64::parse(&image), Err(expected));
}

#[test]
fn measured_busybox_is_the_only_accepted_conjunction() {
    assert_eq!(linux_elf64::Status::Ok as i32, 0);
    assert_eq!(linux_elf64::Status::NullArgument as i32, 1);
    assert_eq!(linux_elf64::ValidatedImage::invalid().valid, 0);
    let image = linux_elf64::parse(BUSYBOX).unwrap();
    assert_eq!(image.valid, 1);
    assert_eq!(image.program_header_count, 5);
    assert_eq!(image.segment_count, 4);
    assert_eq!(image.non_load_count, 1);
    assert_eq!(image.entry, 0x0000_4000_0100_107A);
    assert_eq!(linux_elf64::self_test(), 24);
}

#[test]
fn every_short_file_and_one_long_file_are_refused() {
    for length in 0..BUSYBOX.len() {
        assert_eq!(linux_elf64::parse(&BUSYBOX[..length]), Err(linux_elf64::Status::Truncated));
    }
    let mut long = BUSYBOX.to_vec();
    long.push(0);
    assert_eq!(linux_elf64::parse(&long), Err(linux_elf64::Status::FileLength));
}

#[test]
fn malformed_header_and_program_states_are_named() {
    changed(0, 0, linux_elf64::Status::Magic);
    changed(4, 1, linux_elf64::Status::Class);
    changed(5, 2, linux_elf64::Status::Data);
    changed(6, 0, linux_elf64::Status::IdentVersion);
    changed(7, 3, linux_elf64::Status::Abi);
    changed(9, 1, linux_elf64::Status::IdentPadding);
    changed(16, 3, linux_elf64::Status::Type);
    changed(18, 3, linux_elf64::Status::Machine);
    changed(20, 0, linux_elf64::Status::HeaderVersion);
    changed(48, 1, linux_elf64::Status::HeaderFlags);
    changed(52, 63, linux_elf64::Status::HeaderSize);
    changed(32, 0, linux_elf64::Status::ProgramOffset);
    changed(54, 55, linux_elf64::Status::ProgramSize);
    changed(56, 9, linux_elf64::Status::ProgramCount);
    changed(64, 2, linux_elf64::Status::MeasuredConjunction);
    changed(68, 7, linux_elf64::Status::MeasuredConjunction);
    changed(64 + 56, 3, linux_elf64::Status::MeasuredConjunction);
}
