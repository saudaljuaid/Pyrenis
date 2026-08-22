// SPDX-License-Identifier: GPL-3.0-only
//! Safe bounded FAT16 chain and payload validation for the BusyBox fixture.

use crate::fat16::{self, Geometry, RootEntry, RootQuery};

pub const FILE_BYTES: u32 = 33_584;
pub const FILE_CLUSTERS: u32 = 9;
pub const MAX_CLUSTERS: usize = 512;
pub const ROBUSTNESS_CONTROLS: u32 = 12;
pub const BUSYBOX_NAME: [u8; 11] = *b"BUSYBOX    ";
pub const BUSYBOX_SHA256: [u8; 32] = [
    0xB3, 0x08, 0xF2, 0xCA, 0xD5, 0xB5, 0xCD, 0x0E,
    0xEB, 0x92, 0xA6, 0x22, 0xDE, 0xC8, 0xD7, 0x1C,
    0x1A, 0x08, 0xF6, 0x28, 0xA2, 0x2C, 0xDC, 0x5B,
    0xCD, 0xE2, 0xB9, 0x8B, 0x53, 0x22, 0x07, 0x46,
];

const ATTR_ARCHIVE: u8 = 0x20;
const ATTR_LONG_NAME: u8 = 0x0F;
const DIRECTORY_ENTRY_BYTES: usize = 32;
const FAT16_RESERVED_MIN: u16 = 0xFFF0;
const FAT16_BAD: u16 = 0xFFF7;
const FAT16_EOC_MIN: u16 = 0xFFF8;

#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Status {
    Ok = 0,
    NullArgument = 1,
    Truncated = 2,
    RootEndMissing = 3,
    TargetAbsent = 4,
    TargetDuplicate = 5,
    UnsupportedEntry = 6,
    NameMalformed = 7,
    ClusterRange = 8,
    FileSize = 9,
    ChainCapacity = 10,
    FatReserved = 11,
    FatFree = 12,
    FatBad = 13,
    PrematureEoc = 14,
    OverlongChain = 15,
    ChainCycle = 16,
    ClusterTranslation = 17,
    PayloadLength = 18,
    PayloadDigest = 19,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Chain {
    pub clusters: [u16; MAX_CLUSTERS],
    pub lbas: [u64; MAX_CLUSTERS],
    pub cluster_count: u32,
    pub file_bytes: u32,
    pub final_cluster_bytes: u32,
    pub valid: u32,
}

impl Chain {
    pub const fn invalid() -> Self {
        Self {
            clusters: [0; MAX_CLUSTERS],
            lbas: [0; MAX_CLUSTERS],
            cluster_count: 0,
            file_bytes: 0,
            final_cluster_bytes: 0,
            valid: 0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Payload {
    pub sha256: [u8; 32],
    pub byte_count: u32,
    pub deterministic: u32,
}

impl Payload {
    pub const fn invalid() -> Self {
        Self { sha256: [0; 32], byte_count: 0, deterministic: 0 }
    }
}

fn canonical_component(component: &[u8], required: bool) -> bool {
    let mut saw_character = false;
    let mut saw_space = false;
    for byte in component {
        if *byte == b' ' {
            saw_space = true;
        } else if byte.is_ascii_uppercase() || byte.is_ascii_digit() {
            if saw_space {
                return false;
            }
            saw_character = true;
        } else {
            return false;
        }
    }
    saw_character || !required
}

fn canonical_name(name: &[u8]) -> bool {
    let Some(base) = name.get(..8) else {
        return false;
    };
    let Some(extension) = name.get(8..11) else {
        return false;
    };
    name.len() == 11
        && canonical_component(base, true)
        && canonical_component(extension, false)
}

fn read_u16(bytes: &[u8], offset: usize) -> Result<u16, Status> {
    let end = offset.checked_add(2).ok_or(Status::Truncated)?;
    let field = bytes.get(offset..end).ok_or(Status::Truncated)?;
    let first = field.first().copied().ok_or(Status::Truncated)?;
    let second = field.get(1).copied().ok_or(Status::Truncated)?;
    Ok(u16::from_le_bytes([first, second]))
}

fn read_u32(bytes: &[u8], offset: usize) -> Result<u32, Status> {
    let end = offset.checked_add(4).ok_or(Status::Truncated)?;
    let field = bytes.get(offset..end).ok_or(Status::Truncated)?;
    let first = field.first().copied().ok_or(Status::Truncated)?;
    let second = field.get(1).copied().ok_or(Status::Truncated)?;
    let third = field.get(2).copied().ok_or(Status::Truncated)?;
    let fourth = field.get(3).copied().ok_or(Status::Truncated)?;
    Ok(u32::from_le_bytes([first, second, third, fourth]))
}

fn geometry_valid(geometry: &Geometry) -> bool {
    geometry.bytes_per_sector == fat16::BLOCK_BYTES as u32
        && geometry.sectors_per_cluster == 1
        && geometry.root_entries == fat16::ROOT_ENTRIES
        && geometry.first_data_sector == 4
        && geometry.cluster_count == 4092
        && geometry.total_sectors == fat16::TOTAL_SECTORS
        && geometry.total_sectors <= geometry.namespace_blocks
}

pub fn make_query() -> RootQuery {
    RootQuery { canonical_name: BUSYBOX_NAME }
}

pub fn find_root(
    block: &[u8],
    geometry: &Geometry,
    query: &RootQuery,
    destination_bytes: u32,
) -> Result<RootEntry, Status> {
    if block.len() != fat16::BLOCK_BYTES || !geometry_valid(geometry) {
        return Err(Status::Truncated);
    }
    if query.canonical_name != BUSYBOX_NAME ||
        !canonical_name(&query.canonical_name)
    {
        return Err(Status::NameMalformed);
    }
    let mut found = RootEntry::invalid();
    let mut found_target = false;
    let mut saw_end = false;
    for index in 0..geometry.root_entries as usize {
        let offset = index.checked_mul(DIRECTORY_ENTRY_BYTES)
            .ok_or(Status::Truncated)?;
        let entry_end = offset.checked_add(DIRECTORY_ENTRY_BYTES)
            .ok_or(Status::Truncated)?;
        let entry = block.get(offset..entry_end).ok_or(Status::Truncated)?;
        let first = entry.first().copied().ok_or(Status::Truncated)?;
        if first == 0 {
            saw_end = true;
            let trailing = block.get(entry_end..).ok_or(Status::Truncated)?;
            for trailing in trailing {
                if *trailing != 0 {
                    return Err(Status::UnsupportedEntry);
                }
            }
            break;
        }
        let attribute = entry.get(11).copied().ok_or(Status::Truncated)?;
        if first == 0xE5 || attribute == ATTR_LONG_NAME {
            return Err(Status::UnsupportedEntry);
        }
        let name = entry.get(..11).ok_or(Status::Truncated)?;
        if !canonical_name(name) || attribute != ATTR_ARCHIVE {
            return Err(Status::UnsupportedEntry);
        }
        if name != query.canonical_name {
            return Err(Status::UnsupportedEntry);
        }
        if found_target {
            return Err(Status::TargetDuplicate);
        }
        let cluster = read_u16(block, offset + 26)?;
        let file_size = read_u32(block, offset + 28)?;
        if cluster < 2 || u64::from(cluster - 2) >= geometry.cluster_count {
            return Err(Status::ClusterRange);
        }
        let cluster_bytes = geometry.bytes_per_sector
            .checked_mul(geometry.sectors_per_cluster)
            .ok_or(Status::FileSize)?;
        let clusters = file_size.checked_add(cluster_bytes - 1)
            .ok_or(Status::FileSize)? / cluster_bytes;
        if file_size != FILE_BYTES || file_size > destination_bytes ||
            clusters != FILE_CLUSTERS || clusters as usize > MAX_CLUSTERS
        {
            return Err(Status::FileSize);
        }
        found = RootEntry {
            canonical_name: BUSYBOX_NAME,
            attribute: ATTR_ARCHIVE,
            first_cluster: cluster,
            file_size,
        };
        found_target = true;
    }
    if !saw_end {
        return Err(Status::RootEndMissing);
    }
    if !found_target {
        return Err(Status::TargetAbsent);
    }
    Ok(found)
}

fn cluster_lba(geometry: &Geometry, cluster: u16) -> Result<u64, Status> {
    if cluster < 2 || u64::from(cluster - 2) >= geometry.cluster_count {
        return Err(Status::ClusterRange);
    }
    let relative = u64::from(cluster - 2)
        .checked_mul(u64::from(geometry.sectors_per_cluster))
        .ok_or(Status::ClusterTranslation)?;
    let lba = geometry.first_data_sector.checked_add(relative)
        .ok_or(Status::ClusterTranslation)?;
    if lba >= geometry.total_sectors || lba >= geometry.namespace_blocks {
        return Err(Status::ClusterTranslation);
    }
    Ok(lba)
}

pub fn build_chain(
    fat: &[u8],
    geometry: &Geometry,
    entry: &RootEntry,
) -> Result<Chain, Status> {
    if fat.len() != fat16::BLOCK_BYTES || !geometry_valid(geometry) {
        return Err(Status::Truncated);
    }
    if read_u16(fat, 0)? & 0x00FF != 0x00F8 ||
        read_u16(fat, 2)? < FAT16_EOC_MIN
    {
        return Err(Status::FatReserved);
    }
    if entry.canonical_name != BUSYBOX_NAME || entry.attribute != ATTR_ARCHIVE ||
        entry.file_size != FILE_BYTES
    {
        return Err(Status::FileSize);
    }
    let cluster_bytes = geometry.bytes_per_sector
        .checked_mul(geometry.sectors_per_cluster)
        .ok_or(Status::FileSize)?;
    let needed = entry.file_size.checked_add(cluster_bytes - 1)
        .ok_or(Status::FileSize)? / cluster_bytes;
    if needed == 0 || needed != FILE_CLUSTERS || needed as usize > MAX_CLUSTERS {
        return Err(Status::ChainCapacity);
    }
    let mut chain = Chain::invalid();
    let mut cluster = entry.first_cluster;
    for index in 0..needed as usize {
        let prior = chain.clusters.get(..index).ok_or(Status::ChainCapacity)?;
        if prior.contains(&cluster) {
            return Err(Status::ChainCycle);
        }
        let cluster_slot = chain.clusters.get_mut(index).ok_or(Status::ChainCapacity)?;
        *cluster_slot = cluster;
        let lba_slot = chain.lbas.get_mut(index).ok_or(Status::ChainCapacity)?;
        *lba_slot = cluster_lba(geometry, cluster)?;
        let fat_offset = usize::from(cluster).checked_mul(2)
            .ok_or(Status::ClusterTranslation)?;
        let next = read_u16(fat, fat_offset)?;
        if index + 1 == needed as usize {
            if next < FAT16_EOC_MIN {
                return if next == 0 || next == 1 {
                    Err(Status::FatFree)
                } else if next == FAT16_BAD {
                    Err(Status::FatBad)
                } else if next >= FAT16_RESERVED_MIN {
                    Err(Status::FatReserved)
                } else {
                    Err(Status::OverlongChain)
                };
            }
        } else {
            if next >= FAT16_EOC_MIN {
                return Err(Status::PrematureEoc);
            }
            if next == 0 || next == 1 {
                return Err(Status::FatFree);
            }
            if next == FAT16_BAD {
                return Err(Status::FatBad);
            }
            if next >= FAT16_RESERVED_MIN {
                return Err(Status::FatReserved);
            }
            if next < 2 || u64::from(next - 2) >= geometry.cluster_count {
                return Err(Status::ClusterRange);
            }
            cluster = next;
        }
    }
    chain.cluster_count = needed;
    chain.file_bytes = entry.file_size;
    chain.final_cluster_bytes = entry.file_size - (needed - 1) * cluster_bytes;
    chain.valid = 1;
    Ok(chain)
}

const SHA256_INITIAL: [u32; 8] = [
    0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
    0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
];
const SHA256_K: [u32; 64] = [
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5,
    0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
    0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC,
    0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7,
    0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
    0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3,
    0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5,
    0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
    0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
];

fn padded_byte(data: &[u8], offset: usize, padded_bytes: usize) -> Result<u8, Status> {
    if offset < data.len() {
        data.get(offset).copied().ok_or(Status::PayloadLength)
    } else if offset == data.len() {
        Ok(0x80)
    } else {
        let length_start = padded_bytes.checked_sub(8).ok_or(Status::PayloadLength)?;
        if offset < length_start {
            return Ok(0);
        }
        let length_index = offset.checked_sub(length_start).ok_or(Status::PayloadLength)?;
        let bit_length = u64::try_from(data.len()).map_err(|_| Status::PayloadLength)?
            .checked_mul(8).ok_or(Status::PayloadLength)?;
        bit_length.to_be_bytes().get(length_index).copied()
            .ok_or(Status::PayloadLength)
    }
}

fn sha256(data: &[u8]) -> Result<[u8; 32], Status> {
    let unrounded = data.len().checked_add(9).ok_or(Status::PayloadLength)?;
    let blocks = unrounded.checked_add(63).ok_or(Status::PayloadLength)? / 64;
    let padded_bytes = blocks.checked_mul(64).ok_or(Status::PayloadLength)?;
    let mut state = SHA256_INITIAL;
    for block in 0..blocks {
        let mut words = [0u32; 64];
        for word in 0usize..16 {
            let block_offset = block.checked_mul(64).ok_or(Status::PayloadLength)?;
            let word_offset = word.checked_mul(4).ok_or(Status::PayloadLength)?;
            let offset = block_offset.checked_add(word_offset).ok_or(Status::PayloadLength)?;
            let second = offset.checked_add(1).ok_or(Status::PayloadLength)?;
            let third = offset.checked_add(2).ok_or(Status::PayloadLength)?;
            let fourth = offset.checked_add(3).ok_or(Status::PayloadLength)?;
            let value = u32::from_be_bytes([
                padded_byte(data, offset, padded_bytes)?,
                padded_byte(data, second, padded_bytes)?,
                padded_byte(data, third, padded_bytes)?,
                padded_byte(data, fourth, padded_bytes)?,
            ]);
            let destination = words.get_mut(word).ok_or(Status::PayloadLength)?;
            *destination = value;
        }
        for word in 16usize..64 {
            let previous_fifteen = word.checked_sub(15).ok_or(Status::PayloadLength)?;
            let previous_two = word.checked_sub(2).ok_or(Status::PayloadLength)?;
            let previous_sixteen = word.checked_sub(16).ok_or(Status::PayloadLength)?;
            let previous_seven = word.checked_sub(7).ok_or(Status::PayloadLength)?;
            let a = words.get(previous_fifteen).copied().ok_or(Status::PayloadLength)?;
            let b = words.get(previous_two).copied().ok_or(Status::PayloadLength)?;
            let s0 = a.rotate_right(7) ^ a.rotate_right(18) ^ (a >> 3);
            let s1 = b.rotate_right(17) ^ b.rotate_right(19) ^ (b >> 10);
            let value = words.get(previous_sixteen).copied().ok_or(Status::PayloadLength)?
                .wrapping_add(s0)
                .wrapping_add(words.get(previous_seven).copied()
                    .ok_or(Status::PayloadLength)?)
                .wrapping_add(s1);
            let destination = words.get_mut(word).ok_or(Status::PayloadLength)?;
            *destination = value;
        }
        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = state;
        for (constant, word) in SHA256_K.iter().copied().zip(words.iter().copied()) {
            let upper = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let choose = (e & f) ^ ((!e) & g);
            let first = h.wrapping_add(upper).wrapping_add(choose)
                .wrapping_add(constant).wrapping_add(word);
            let lower = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let majority = (a & b) ^ (a & c) ^ (b & c);
            let second = lower.wrapping_add(majority);
            h = g; g = f; f = e; e = d.wrapping_add(first);
            d = c; c = b; b = a; a = first.wrapping_add(second);
        }
        let [s0, s1, s2, s3, s4, s5, s6, s7] = state;
        state = [
            s0.wrapping_add(a), s1.wrapping_add(b), s2.wrapping_add(c),
            s3.wrapping_add(d), s4.wrapping_add(e), s5.wrapping_add(f),
            s6.wrapping_add(g), s7.wrapping_add(h),
        ];
    }
    let mut digest = [0u8; 32];
    let bytes = state.iter().flat_map(|word| word.to_be_bytes());
    for (destination, source) in digest.iter_mut().zip(bytes) {
        *destination = source;
    }
    Ok(digest)
}

pub fn validate_payload(data: &[u8]) -> Result<Payload, Status> {
    if data.len() != FILE_BYTES as usize {
        return Err(Status::PayloadLength);
    }
    let digest = sha256(data)?;
    let mut difference = 0u8;
    for (actual, expected) in digest.iter().zip(BUSYBOX_SHA256.iter()) {
        difference |= actual ^ expected;
    }
    if difference != 0 {
        return Err(Status::PayloadDigest);
    }
    Ok(Payload { sha256: digest, byte_count: FILE_BYTES, deterministic: 1 })
}

pub fn self_test() -> u32 {
    if MAX_CLUSTERS != 512 || FILE_CLUSTERS != 9 ||
        FILE_BYTES.div_ceil(fat16::BLOCK_BYTES as u32) != FILE_CLUSTERS ||
        !canonical_name(&BUSYBOX_NAME)
    {
        return 0;
    }
    ROBUSTNESS_CONTROLS
}

#[cfg(test)]
mod tests {
    use super::*;

    fn geometry() -> Geometry {
        Geometry {
            total_sectors: 4096, root_dir_sectors: 1,
            first_fat_sector: 1, fat_sectors: 2, first_root_sector: 3,
            first_data_sector: 4, data_sectors: 4092, cluster_count: 4092,
            namespace_blocks: 4096, bytes_per_sector: 4096,
            sectors_per_cluster: 1, root_entries: 128, media: 0xF8,
        }
    }

    fn put_u16(bytes: &mut [u8], offset: usize, value: u16) {
        bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
    }

    fn put_u32(bytes: &mut [u8], offset: usize, value: u32) {
        bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }

    fn root() -> [u8; fat16::BLOCK_BYTES] {
        let mut block = [0u8; fat16::BLOCK_BYTES];
        block[..11].copy_from_slice(&BUSYBOX_NAME);
        block[11] = ATTR_ARCHIVE;
        put_u16(&mut block, 26, 2);
        put_u32(&mut block, 28, FILE_BYTES);
        block
    }

    fn make_fat() -> [u8; fat16::BLOCK_BYTES] {
        let mut block = [0u8; fat16::BLOCK_BYTES];
        put_u16(&mut block, 0, 0xFFF8);
        put_u16(&mut block, 2, 0xFFFF);
        for cluster in 2..10 { put_u16(&mut block, cluster * 2, cluster as u16 + 1); }
        put_u16(&mut block, 20, 0xFFFF);
        block
    }

    #[test]
    fn bounded_chain_controls() {
        let geometry = geometry();
        let query = RootQuery { canonical_name: BUSYBOX_NAME };
        let mut root = root();
        let entry = find_root(&root, &geometry, &query, FILE_BYTES).unwrap();
        let mut fat = make_fat();
        let chain = build_chain(&fat, &geometry, &entry).unwrap();
        assert_eq!(chain.cluster_count, 9);
        assert_eq!(chain.final_cluster_bytes, 816);
        put_u16(&mut fat, 8, 3);
        assert!(matches!(build_chain(&fat, &geometry, &entry), Err(Status::ChainCycle)));
        fat = make_fat();
        put_u16(&mut fat, 8, 0xFFFF);
        assert!(matches!(build_chain(&fat, &geometry, &entry), Err(Status::PrematureEoc)));
        fat = make_fat();
        put_u16(&mut fat, 20, 11);
        assert!(matches!(build_chain(&fat, &geometry, &entry), Err(Status::OverlongChain)));
        root[32] = b'X';
        assert!(find_root(&root, &geometry, &query, FILE_BYTES).is_err());
        assert_eq!(self_test(), ROBUSTNESS_CONTROLS);
    }

    #[test]
    fn generic_sha256_matches_known_vector() {
        assert_eq!(
            sha256(b"abc"),
            Ok([0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA,
             0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
             0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
             0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD])
        );
    }
}
