// SPDX-License-Identifier: GPL-3.0-only
//! Safe parser for the one measured static BusyBox ELF64 executable.

/// Exact pinned executable length.
pub const FILE_BYTES: usize = 33_584;
/// Maximum admitted ELF program headers.
pub const MAX_PROGRAM_HEADERS: usize = 8;
/// Exact measured program-header count.
pub const PROGRAM_HEADERS: usize = 5;
/// Maximum and exact admitted load-segment count.
pub const MAX_LOAD_SEGMENTS: usize = 4;
/// Exact installed image page count.
pub const IMAGE_PAGES: usize = 9;
/// Parser robustness controls represented by the frozen matrix.
pub const ROBUSTNESS_CONTROLS: u32 = 24;

const HEADER_BYTES: u16 = 64;
const PROGRAM_HEADER_BYTES: u16 = 56;
const PAGE_BYTES: u64 = 4096;
const ENTRY: u64 = 0x0000_4000_0100_107A;
const PT_LOAD: u32 = 1;
const PT_GNU_STACK: u32 = 0x6474_E551;
const PF_X: u32 = 1;
const PF_W: u32 = 2;
const PF_R: u32 = 4;

const ELF_TYPE: usize = 16;
const ELF_MACHINE: usize = 18;
const ELF_VERSION: usize = 20;
const ELF_ENTRY: usize = 24;
const ELF_PROGRAM_OFFSET: usize = 32;
const ELF_FLAGS: usize = 48;
const ELF_HEADER_SIZE: usize = 52;
const ELF_PROGRAM_SIZE: usize = 54;
const ELF_PROGRAM_COUNT: usize = 56;

const PROGRAM_TYPE: usize = 0;
const PROGRAM_FLAGS: usize = 4;
const PROGRAM_OFFSET: usize = 8;
const PROGRAM_VIRTUAL: usize = 16;
const PROGRAM_PHYSICAL: usize = 24;
const PROGRAM_FILE_SIZE: usize = 32;
const PROGRAM_MEMORY_SIZE: usize = 40;
const PROGRAM_ALIGNMENT: usize = 48;

/// Named parser conclusion mirrored by the C header.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    /// The complete measured conjunction is valid.
    Ok = 0,
    /// A null pointer crossed the C boundary.
    NullArgument = 1,
    /// A required byte or range was truncated.
    Truncated = 2,
    /// The complete file length is not exact.
    FileLength = 3,
    /// ELF magic is invalid.
    Magic = 4,
    /// The file is not ELFCLASS64.
    Class = 5,
    /// The file is not little endian.
    Data = 6,
    /// ELF identification version is not current.
    IdentVersion = 7,
    /// OSABI or ABI version is not the measured System V value.
    Abi = 8,
    /// An identification padding byte is nonzero.
    IdentPadding = 9,
    /// The object is not fixed ET_EXEC.
    Type = 10,
    /// The machine is not x86-64.
    Machine = 11,
    /// The ELF header version is not current.
    HeaderVersion = 12,
    /// Processor-specific header flags are present.
    HeaderFlags = 13,
    /// ELF header size differs from ELF64.
    HeaderSize = 14,
    /// Program headers do not begin at the measured offset.
    ProgramOffset = 15,
    /// Program-header entries are not ELF64-sized.
    ProgramSize = 16,
    /// Program-header cardinality is outside the exact contract.
    ProgramCount = 17,
    /// Program-table arithmetic or range is invalid.
    ProgramTable = 18,
    /// A dynamic, interpreter, TLS, or other unmeasured header appeared.
    SegmentType = 19,
    /// Segment flags are invalid or writable/executable.
    SegmentFlags = 20,
    /// A file extent wraps or exceeds the executable.
    FileRange = 21,
    /// A load has invalid file/memory size.
    LoadSize = 22,
    /// Segment alignment or address/offset congruence is invalid.
    Alignment = 23,
    /// A load is outside canonical user memory.
    VirtualAddress = 24,
    /// Address or page-rounding arithmetic overflowed.
    AddressOverflow = 25,
    /// Load byte or page extents overlap.
    Overlap = 26,
    /// The entry is not inside the measured executable load.
    Entry = 27,
    /// The sole GNU stack header is absent or malformed.
    Stack = 28,
    /// A decoded field differs from the checksum-pinned measured conjunction.
    MeasuredConjunction = 29,
}

/// Pointer-free facts for one admitted load segment.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Segment {
    /// File offset of initialized bytes.
    pub file_offset: u64,
    /// First virtual byte.
    pub virtual_address: u64,
    /// Initialized byte count.
    pub file_size: u64,
    /// Complete in-memory byte count including BSS.
    pub memory_size: u64,
    /// First mapped page.
    pub mapping_start: u64,
    /// Exclusive mapped page end.
    pub mapping_end: u64,
    /// Exact ELF permission flags.
    pub flags: u32,
    /// Explicit zero padding for the C ABI.
    pub reserved: u32,
}

impl Segment {
    const fn invalid() -> Self {
        Self {
            file_offset: 0,
            virtual_address: 0,
            file_size: 0,
            memory_size: 0,
            mapping_start: 0,
            mapping_end: 0,
            flags: 0,
            reserved: 0,
        }
    }
}

/// Complete pointer-free validated executable result.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ValidatedImage {
    /// One only after every check succeeds.
    pub valid: u32,
    /// Exact measured program-header count.
    pub program_header_count: u32,
    /// Exact load-segment count.
    pub segment_count: u32,
    /// Exact measured non-load count.
    pub non_load_count: u32,
    /// Checked Ring 3 entry point.
    pub entry: u64,
    /// Load segments in file order.
    pub segments: [Segment; MAX_LOAD_SEGMENTS],
}

impl ValidatedImage {
    /// Stable all-zero value for failed FFI calls.
    pub const fn invalid() -> Self {
        Self {
            valid: 0,
            program_header_count: 0,
            segment_count: 0,
            non_load_count: 0,
            entry: 0,
            segments: [Segment::invalid(); MAX_LOAD_SEGMENTS],
        }
    }
}

#[derive(Clone, Copy)]
struct ProgramHeader {
    kind: u32,
    flags: u32,
    offset: u64,
    virtual_address: u64,
    physical_address: u64,
    file_size: u64,
    memory_size: u64,
    alignment: u64,
}

impl ProgramHeader {
    const fn invalid() -> Self {
        Self {
            kind: 0,
            flags: 0,
            offset: 0,
            virtual_address: 0,
            physical_address: 0,
            file_size: 0,
            memory_size: 0,
            alignment: 0,
        }
    }
}

const MEASURED: [ProgramHeader; PROGRAM_HEADERS] = [
    ProgramHeader { kind: PT_LOAD, flags: PF_R, offset: 0, virtual_address: 0x4000_0100_0000, physical_address: 0x4000_0100_0000, file_size: 0x158, memory_size: 0x158, alignment: 0x1000 },
    ProgramHeader { kind: PT_LOAD, flags: PF_R | PF_X, offset: 0x1000, virtual_address: 0x4000_0100_1000, physical_address: 0x4000_0100_1000, file_size: 0x5563, memory_size: 0x5563, alignment: 0x1000 },
    ProgramHeader { kind: PT_LOAD, flags: PF_R, offset: 0x7000, virtual_address: 0x4000_0100_7000, physical_address: 0x4000_0100_7000, file_size: 0xED1, memory_size: 0xED1, alignment: 0x1000 },
    ProgramHeader { kind: PT_LOAD, flags: PF_R | PF_W, offset: 0x8000, virtual_address: 0x4000_0100_8000, physical_address: 0x4000_0100_8000, file_size: 0xFE, memory_size: 0xB38, alignment: 0x1000 },
    ProgramHeader { kind: PT_GNU_STACK, flags: PF_R | PF_W, offset: 0, virtual_address: 0, physical_address: 0, file_size: 0, memory_size: 0, alignment: 0x10 },
];

fn byte(input: &[u8], offset: usize) -> Result<u8, Status> {
    input.get(offset).copied().ok_or(Status::Truncated)
}

fn u16_le(input: &[u8], offset: usize) -> Result<u16, Status> {
    Ok(u16::from(byte(input, offset)?)
        | (u16::from(byte(input, offset.checked_add(1).ok_or(Status::Truncated)?)?) << 8))
}

fn u32_le(input: &[u8], offset: usize) -> Result<u32, Status> {
    let mut value = 0u32;
    for index in 0..4usize {
        value |= u32::from(byte(input, offset.checked_add(index).ok_or(Status::Truncated)?)?)
            << (index * 8);
    }
    Ok(value)
}

fn u64_le(input: &[u8], offset: usize) -> Result<u64, Status> {
    let mut value = 0u64;
    for index in 0..8usize {
        value |= u64::from(byte(input, offset.checked_add(index).ok_or(Status::Truncated)?)?)
            << (index * 8);
    }
    Ok(value)
}

fn canonical_user(address: u64) -> bool {
    address <= 0x0000_7FFF_FFFF_FFFF
}

fn decode_program(input: &[u8], offset: usize) -> Result<ProgramHeader, Status> {
    Ok(ProgramHeader {
        kind: u32_le(input, offset.checked_add(PROGRAM_TYPE).ok_or(Status::ProgramTable)?)?,
        flags: u32_le(input, offset.checked_add(PROGRAM_FLAGS).ok_or(Status::ProgramTable)?)?,
        offset: u64_le(input, offset.checked_add(PROGRAM_OFFSET).ok_or(Status::ProgramTable)?)?,
        virtual_address: u64_le(input, offset.checked_add(PROGRAM_VIRTUAL).ok_or(Status::ProgramTable)?)?,
        physical_address: u64_le(input, offset.checked_add(PROGRAM_PHYSICAL).ok_or(Status::ProgramTable)?)?,
        file_size: u64_le(input, offset.checked_add(PROGRAM_FILE_SIZE).ok_or(Status::ProgramTable)?)?,
        memory_size: u64_le(input, offset.checked_add(PROGRAM_MEMORY_SIZE).ok_or(Status::ProgramTable)?)?,
        alignment: u64_le(input, offset.checked_add(PROGRAM_ALIGNMENT).ok_or(Status::ProgramTable)?)?,
    })
}

fn same_header(left: ProgramHeader, right: ProgramHeader) -> bool {
    left.kind == right.kind && left.flags == right.flags && left.offset == right.offset
        && left.virtual_address == right.virtual_address
        && left.physical_address == right.physical_address
        && left.file_size == right.file_size && left.memory_size == right.memory_size
        && left.alignment == right.alignment
}

fn validate_programs(programs: &[ProgramHeader; PROGRAM_HEADERS]) -> Result<[Segment; MAX_LOAD_SEGMENTS], Status> {
    let mut segments = [Segment::invalid(); MAX_LOAD_SEGMENTS];
    let mut loads = 0usize;
    let mut stacks = 0usize;
    for (program, measured) in programs.iter().copied().zip(MEASURED) {
        if !same_header(program, measured) {
            return Err(Status::MeasuredConjunction);
        }
        if program.kind == PT_GNU_STACK {
            if program.flags != PF_R | PF_W || program.offset != 0
                || program.virtual_address != 0 || program.physical_address != 0
                || program.file_size != 0 || program.memory_size != 0
                || program.alignment != 16
            {
                return Err(Status::Stack);
            }
            stacks += 1;
            continue;
        }
        if program.kind != PT_LOAD {
            return Err(Status::SegmentType);
        }
        if loads == MAX_LOAD_SEGMENTS {
            return Err(Status::ProgramCount);
        }
        if program.flags != PF_R && program.flags != PF_R | PF_X
            && program.flags != PF_R | PF_W
        {
            return Err(Status::SegmentFlags);
        }
        if program.flags & (PF_W | PF_X) == (PF_W | PF_X) {
            return Err(Status::SegmentFlags);
        }
        if program.file_size == 0 || program.memory_size < program.file_size {
            return Err(Status::LoadSize);
        }
        let file_end = program.offset.checked_add(program.file_size).ok_or(Status::FileRange)?;
        if file_end > FILE_BYTES as u64 {
            return Err(Status::FileRange);
        }
        if program.alignment != PAGE_BYTES || !program.alignment.is_power_of_two()
            || program.offset & (program.alignment - 1)
                != program.virtual_address & (program.alignment - 1)
        {
            return Err(Status::Alignment);
        }
        let virtual_end = program.virtual_address.checked_add(program.memory_size)
            .ok_or(Status::AddressOverflow)?;
        if virtual_end == 0 || !canonical_user(program.virtual_address)
            || !canonical_user(virtual_end - 1)
        {
            return Err(Status::VirtualAddress);
        }
        let mapping_start = program.virtual_address & !(PAGE_BYTES - 1);
        let mapping_end = virtual_end.checked_add(PAGE_BYTES - 1)
            .ok_or(Status::AddressOverflow)? & !(PAGE_BYTES - 1);
        if mapping_end <= mapping_start || !canonical_user(mapping_end - 1) {
            return Err(Status::VirtualAddress);
        }
        let prior_segments = segments.get(..loads)
            .ok_or(Status::ProgramCount)?;
        for prior in prior_segments {
            if program.virtual_address < prior.virtual_address + prior.memory_size
                && prior.virtual_address < virtual_end
                || mapping_start < prior.mapping_end && prior.mapping_start < mapping_end
            {
                return Err(Status::Overlap);
            }
        }
        let destination = segments.get_mut(loads)
            .ok_or(Status::ProgramCount)?;
        *destination = Segment {
            file_offset: program.offset,
            virtual_address: program.virtual_address,
            file_size: program.file_size,
            memory_size: program.memory_size,
            mapping_start,
            mapping_end,
            flags: program.flags,
            reserved: 0,
        };
        loads += 1;
    }
    if loads != MAX_LOAD_SEGMENTS || stacks != 1 {
        return Err(Status::ProgramCount);
    }
    let executable = segments.iter().find(|segment| segment.flags == PF_R | PF_X)
        .ok_or(Status::Entry)?;
    let executable_end = executable.virtual_address.checked_add(executable.file_size)
        .ok_or(Status::AddressOverflow)?;
    if ENTRY < executable.virtual_address || ENTRY >= executable_end {
        return Err(Status::Entry);
    }
    Ok(segments)
}

/// Decode and validate the exact checksum-pinned static BusyBox image.
pub fn parse(input: &[u8]) -> Result<ValidatedImage, Status> {
    if input.len() < FILE_BYTES {
        return Err(Status::Truncated);
    }
    if input.len() != FILE_BYTES {
        return Err(Status::FileLength);
    }
    if byte(input, 0)? != 0x7F || byte(input, 1)? != b'E'
        || byte(input, 2)? != b'L' || byte(input, 3)? != b'F'
    {
        return Err(Status::Magic);
    }
    if byte(input, 4)? != 2 { return Err(Status::Class); }
    if byte(input, 5)? != 1 { return Err(Status::Data); }
    if byte(input, 6)? != 1 { return Err(Status::IdentVersion); }
    if byte(input, 7)? != 0 || byte(input, 8)? != 0 { return Err(Status::Abi); }
    for offset in 9..16 {
        if byte(input, offset)? != 0 { return Err(Status::IdentPadding); }
    }
    if u16_le(input, ELF_TYPE)? != 2 { return Err(Status::Type); }
    if u16_le(input, ELF_MACHINE)? != 62 { return Err(Status::Machine); }
    if u32_le(input, ELF_VERSION)? != 1 { return Err(Status::HeaderVersion); }
    if u32_le(input, ELF_FLAGS)? != 0 { return Err(Status::HeaderFlags); }
    if u16_le(input, ELF_HEADER_SIZE)? != HEADER_BYTES { return Err(Status::HeaderSize); }
    let program_offset = u64_le(input, ELF_PROGRAM_OFFSET)?;
    if program_offset != u64::from(HEADER_BYTES) { return Err(Status::ProgramOffset); }
    let program_size = u16_le(input, ELF_PROGRAM_SIZE)?;
    if program_size != PROGRAM_HEADER_BYTES { return Err(Status::ProgramSize); }
    let program_count = u16_le(input, ELF_PROGRAM_COUNT)?;
    if program_count as usize != PROGRAM_HEADERS || program_count as usize > MAX_PROGRAM_HEADERS {
        return Err(Status::ProgramCount);
    }
    let table_offset = usize::try_from(program_offset).map_err(|_| Status::ProgramTable)?;
    let table_bytes = usize::from(program_size).checked_mul(usize::from(program_count))
        .ok_or(Status::ProgramTable)?;
    let table_end = table_offset.checked_add(table_bytes).ok_or(Status::ProgramTable)?;
    if table_end > input.len() { return Err(Status::ProgramTable); }
    let mut programs = [ProgramHeader::invalid(); PROGRAM_HEADERS];
    for (index, program) in programs.iter_mut().enumerate() {
        let offset = table_offset.checked_add(index.checked_mul(usize::from(program_size))
            .ok_or(Status::ProgramTable)?).ok_or(Status::ProgramTable)?;
        *program = decode_program(input, offset)?;
    }
    let segments = validate_programs(&programs)?;
    let entry = u64_le(input, ELF_ENTRY)?;
    if entry != ENTRY { return Err(Status::Entry); }
    Ok(ValidatedImage {
        valid: 1,
        program_header_count: PROGRAM_HEADERS as u32,
        segment_count: MAX_LOAD_SEGMENTS as u32,
        non_load_count: 1,
        entry,
        segments,
    })
}

/// Run pointer-free invariants for the measured header conjunction.
pub fn self_test() -> u32 {
    if FILE_BYTES > 2 * 1024 * 1024 || FILE_BYTES.div_ceil(PAGE_BYTES as usize) != IMAGE_PAGES
        || PROGRAM_HEADERS > MAX_PROGRAM_HEADERS || MAX_LOAD_SEGMENTS != 4
        || validate_programs(&MEASURED).is_err()
    {
        return 0;
    }
    let mut changed = MEASURED;
    let Some(executable) = changed.get_mut(1) else {
        return 0;
    };
    executable.flags |= PF_W;
    if validate_programs(&changed).is_ok() {
        return 0;
    }
    changed = MEASURED;
    let Some(writable) = changed.get_mut(3) else {
        return 0;
    };
    let Some(invalid_size) = writable.file_size.checked_sub(1) else {
        return 0;
    };
    writable.memory_size = invalid_size;
    if validate_programs(&changed).is_ok() {
        return 0;
    }
    ROBUSTNESS_CONTROLS
}
