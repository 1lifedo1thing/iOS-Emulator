//! The iPhone 4S's NAND (n94_recipe): the PPN page store the engine serves (qemu-ios hw/arm/s5l8940_iop.c), its
//! marker sources, and the table the install boot's helper reads. A port of iPhonEmu's tools/ios-prep/nand_ppn.py
//! and guest_install.py (https://github.com/kazin1898/iPhonEmu, GPL-2.0-or-later).
//!
//! The store is a directory: geometry.json (the one geometry, also written into the kboot device tree, so the kernel
//! and the model agree), one sparse file per chip (ch<n>-ce<m>.pages: every page record `page_bytes + meta_bytes`,
//! XOR 0xff so a hole reads as erased), and markers.json. The 4S kernel has only AppleSwissPPNFTL, whose on-flash
//! layout is its own, so the host writes no volume into the chips: it_install (guest tools) writes *marker* pages
//! through the FTL in one boot, each naming a 4 KiB block of a host file that the model serves in its place.
//!
//! The geometry is a consistent part, not a real chip: two channels with one chip enable each, two CAUs per chip,
//! 128 pages of 8 KiB (+32 bytes of meta) per block, 64 SLC pages, 1536-byte boot pages, and 2048 blocks per CAU at
//! 8 GiB, scaled for other sizes. The FTL exports the decimal size (16 GiB: 3,906,250 4 KiB sectors).
use crate::error::Result;
use serde_json::{json, Value};
use std::path::Path;

pub const MARKER_MAGIC: &[u8; 16] = b"iPhonEmu-marker1";
pub const MARKER_CHECK: u32 = 0xA500_0000;
pub const BLOCK: u64 = 4096;
/// The MBR takes block 0 of the FTL's disk; the system volume follows.
pub const SYSTEM_LBA: u32 = 1;

/// The store's geometry for `gib` GiB of raw pages.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Geometry {
    pub blocks_per_cau: u32,
    pub block_bits: u32,
}

const CHANNELS: u32 = 2;
const CE_PER_CHANNEL: u32 = 1;
const CAUS_PER_CE: u32 = 2;
const PAGES_PER_BLOCK: u32 = 128;
const SLC_PAGES: u32 = 64;
const PAGE_BYTES: u32 = 8192;
const META_BYTES: u32 = 32;
const LOGICAL_PAGE: u32 = 4096;
const BOOT_PAGE_BYTES: u32 = 0x600;
const CHIP_ID: [u8; 6] = [0x98, 0x3a, 0x94, 0x93, 0x76, 0x5a];
const FIRMWARE_MIN_CAPACITY: u32 = 0x18_0000;

impl Geometry {
    pub fn for_gib(gib: u32) -> Result<Geometry> {
        if ![2, 4, 8, 16, 32, 64].contains(&gib) {
            return Err(crate::unsupported!("iPhone 4S storage {gib} GiB: one of 2, 4, 8, 16, 32, 64"));
        }
        let blocks_per_cau = 2048 * gib / 8;
        Ok(Geometry { blocks_per_cau, block_bits: 31 - blocks_per_cau.leading_zeros() })
    }
    /// From the catalog's storage ("16g").
    pub fn for_storage(storage: &str) -> Result<Geometry> {
        let gib = storage.trim_end_matches(['g', 'G']).parse::<u32>().map_err(|_| crate::unsupported!("storage {storage}"))?;
        Geometry::for_gib(gib)
    }
    fn chips(&self) -> u32 {
        CHANNELS * CE_PER_CHANNEL
    }
    fn blocks_per_ce(&self) -> u32 {
        CAUS_PER_CE * self.blocks_per_cau
    }
    fn stride(&self) -> u64 {
        (PAGE_BYTES + META_BYTES) as u64
    }
    /// The bytes of one chip file.
    pub fn chip_bytes(&self) -> u64 {
        self.blocks_per_ce() as u64 * PAGES_PER_BLOCK as u64 * self.stride()
    }
    /// The FTL disk's 4 KiB sectors, as the 4S's FTL exports them (decimal gigabytes).
    pub fn disk_blocks(&self) -> u64 {
        let raw = self.chips() as u64 * self.chip_bytes() / self.stride() * PAGE_BYTES as u64;
        raw / (1 << 30) * 1_000_000_000 / BLOCK
    }

    pub fn json(&self) -> Value {
        json!({
            "format": "ppn-xor-ff-v2", "channels": CHANNELS, "ce_per_channel": CE_PER_CHANNEL, "caus_per_ce": CAUS_PER_CE,
            "blocks_per_cau": self.blocks_per_cau, "pages_per_block": PAGES_PER_BLOCK, "slc_pages_per_block": SLC_PAGES,
            "page_bytes": PAGE_BYTES, "meta_bytes": META_BYTES, "logical_page_bytes": LOGICAL_PAGE,
            "boot_page_bytes": BOOT_PAGE_BYTES, "cau_bits": 1, "block_bits": self.block_bits, "page_bits": 7,
            "chip_id": hex::encode(CHIP_ID), "ppn_spec_version": 0x10000,
        })
    }

    /// (key, value) for arm-io/flash-controller0 and its disk child: what iBoot reads off the chips. Only keys the DT
    /// has get written (kboot).
    pub fn dt_props(&self) -> Vec<(&'static str, Vec<u8>)> {
        let ces = self.chips();
        let mut ce_bitmap = 0u32;
        for ch in 0..CHANNELS {
            for ce in 0..CE_PER_CHANNEL {
                ce_bitmap |= 1 << (ch * 8 + ce);
            }
        }
        let meta_per_lp = META_BYTES * LOGICAL_PAGE / PAGE_BYTES;
        let u = |v: u32| v.to_le_bytes().to_vec();
        let mut readid = CHIP_ID.to_vec();
        readid.extend([0, 0]);
        vec![
            ("ppn-device", u(1)), ("ppn-spec-version", u(0x10000)), ("ppn-ftl-ver", u(2)),
            ("#ce", u(ces)), ("ce-bitmap", u(ce_bitmap)), ("#databus", u(CHANNELS)), ("#die-ce", u(CAUS_PER_CE)),
            ("#ce-blocks", u(self.blocks_per_ce())), ("#block-pages", u(PAGES_PER_BLOCK)), ("#page-bytes", u(PAGE_BYTES)),
            ("#spare-bytes", u(META_BYTES)), ("#page-sectors", u(PAGE_BYTES / 512)),
            ("meta-per-logical-page", u(meta_per_lp)), ("valid-meta-per-logical-page", u(meta_per_lp)),
            ("logical-page-size", u(LOGICAL_PAGE)), ("#bootloader-bytes", u(BOOT_PAGE_BYTES)),
            ("caus-ce", u(CAUS_PER_CE)), ("blocks-cau", u(self.blocks_per_cau)),
            ("cau-bits", u(1)), ("block-bits", u(self.block_bits)), ("page-bits", u(7)),
            ("block-stride", u(1 << 7)), ("slc-pages", u(SLC_PAGES)),
            ("mlc-pages", u(PAGES_PER_BLOCK)), ("match-oddeven-caus", u(0)), ("max-transaction-size", u(64)),
            ("vendor-type", u(0)), ("bbt-format", u(0)), ("ecc-threshold", u(0)), ("ecc-correctable", u(0)),
            ("toggle-device", u(0)), ("banks-per-ce", u(1)),
            ("device-readid", readid.clone()), ("dbg-chipids", readid.repeat(ces as usize)),
        ]
    }
}

// ---- the partition table in the boot blocks (IOFlashStorage-664.13.12's nand_part_core) ---------------------------
//
// A 1536-byte boot page in pages 0 and 1 of block 0 of every chip: 'ndrG', version 1.9, the geometry, a cemetery, the
// spares ('rAps') and substitutions ('sbus') the pool partitions live in, and 32 entries laid out as the kernel's PPN
// format string "slice:boot:1 pool:plog:4:1 pool:nvrm:2:3 flex:firm:2:s:/:firmware-min-capacity bulk:fsys info:scfg
// info:diag info:fbbt" builds them. The guards are the Fletcher-16 of the bytes before them.
const ENTRY_SLICE: u32 = 2;
const ENTRY_INFO: u32 = 1;
const ENTRY_BULK: u32 = 4;
const ENTRY_POOL: u32 = 8;
const POOL_SLC: u32 = 0x100;
const POOL_FULL_PAGES: u32 = 0x200;

fn fletcher16(data: &[u8]) -> u16 {
    let (mut s1, mut s2) = (0xffu32, 0xffu32);
    for chunk in data.chunks(20) {
        for &b in chunk {
            s1 += b as u32;
            s2 += s1;
        }
        s1 = (s1 & 0xff) + (s1 >> 8);
        s2 = (s2 & 0xff) + (s2 >> 8);
    }
    s1 = (s1 & 0xff) + (s1 >> 8);
    s2 = (s2 & 0xff) + (s2 >> 8);
    ((s2 << 8 | s1) & 0xffff) as u16
}

/// (name, type, a, b, flags) as nand_part lays them out.
fn partition_specs(g: &Geometry) -> Vec<(&'static str, u32, u32, u32, u32)> {
    let slc_block_bytes = SLC_PAGES * PAGE_BYTES;
    let firm_depth = FIRMWARE_MIN_CAPACITY.div_ceil(slc_block_bytes);
    let mut specs = vec![("boot", ENTRY_SLICE, 0, 1, 0)];
    let mut used = g.chips();
    for (name, width, depth, flags) in
        [("plog", 4, 1, POOL_SLC), ("nvrm", 2, 3, POOL_SLC), ("firm", 2, firm_depth, POOL_SLC | POOL_FULL_PAGES)]
    {
        specs.push((name, ENTRY_POOL, width, depth, flags));
        used += width * depth;
    }
    specs.push(("fsys", ENTRY_BULK, used, g.blocks_per_ce() - used, 2));
    for name in ["scfg", "diag", "fbbt"] {
        specs.push((name, ENTRY_INFO, 0, 1, 0));
    }
    specs
}

/// The pool partitions' spare blocks (chip, block) and their substitutions, as the kernel's own rewrite lays them out.
fn pool_map(g: &Geometry, specs: &[(&str, u32, u32, u32, u32)]) -> (Vec<(u32, u32)>, Vec<u32>) {
    let ces = g.chips();
    let banks = ces * CAUS_PER_CE;
    let mut subs = Vec::new();
    let mut offset = 0;
    for &(_, typ, a, b, _) in specs {
        if typ == ENTRY_SLICE {
            offset += b;
        } else if typ == ENTRY_POOL {
            for slot in 0..a {
                for j in 0..b {
                    subs.push((offset + slot / ces + j) * ces + slot % ces);
                }
            }
            offset += a * b / ces;
        }
    }
    subs.extend([0; 4]); // the extra spares the kernel asks for on a PPN table
    let spares = (0..subs.len() as u32)
        .map(|i| {
            let bank = (banks - 1 + i) % banks;
            (bank % ces, (bank / ces + 1) * g.blocks_per_cau - 4 - (i + 1) / banks)
        })
        .collect();
    (spares, subs)
}

fn put32(page: &mut [u8], at: usize, v: u32) {
    page[at..at + 4].copy_from_slice(&v.to_le_bytes());
}

fn build_ptab(g: &Geometry) -> Vec<u8> {
    let mut page = vec![0u8; BOOT_PAGE_BYTES as usize];
    page[0..4].copy_from_slice(b"ndrG");
    for (i, v) in [1u32, 9, 1, 0].iter().enumerate() {
        put32(&mut page, 8 + 4 * i, *v);
    }
    let geo = [0, 0, 1, g.chips(), g.blocks_per_ce(), PAGES_PER_BLOCK, PAGE_BYTES / 512, META_BYTES];
    for (i, v) in geo.iter().enumerate() {
        put32(&mut page, 0x200 + 4 * i, *v);
    }
    page[0x220] = 7; // flags: 3 | 4 isPpn
    let mut cem = [0u8; 44];
    cem[0..4].copy_from_slice(&0xdead_cafeu32.to_le_bytes());
    let guard = fletcher16(&cem[..42]);
    cem[42..44].copy_from_slice(&guard.to_le_bytes());
    page[0x224..0x250].copy_from_slice(&cem);
    let specs = partition_specs(g);
    let (spares, subs) = pool_map(g, &specs);
    let mut spare = vec![0u8; 0x10c];
    spare[0..4].copy_from_slice(b"rAps");
    spare[4..6].copy_from_slice(&(spares.len() as u16).to_le_bytes());
    for (i, (ce, block)) in spares.iter().enumerate() {
        put32(&mut spare, 10 + 4 * i, block | ce << 24);
    }
    let guard = fletcher16(&spare[..0x10a]);
    spare[0x10a..0x10c].copy_from_slice(&guard.to_le_bytes());
    page[0x250..0x35c].copy_from_slice(&spare);
    let mut sub = vec![0u8; 0x4c];
    sub[0..4].copy_from_slice(b"sbus");
    sub[4..6].copy_from_slice(&(subs.len() as u16).to_le_bytes());
    sub[6] = 2;
    for (i, v) in subs.iter().enumerate() {
        sub[10 + i] = *v as u8;
    }
    let guard = fletcher16(&sub[..0x4a]);
    sub[0x4a..0x4c].copy_from_slice(&guard.to_le_bytes());
    page[0x35c..0x3a8].copy_from_slice(&sub);
    for i in 0..32 {
        let (name, typ, a, b, flags) = specs.get(i).copied().unwrap_or(("none", 0, 0, 1, 0x8000_0000));
        let at = 0x400 + 16 * i;
        put32(&mut page, at, u32::from_be_bytes(name.as_bytes().try_into().unwrap_or([0; 4])));
        put32(&mut page, at + 4, a);
        put32(&mut page, at + 8, b);
        put32(&mut page, at + 12, typ | flags);
    }
    page
}

/// A new page store in `dir`: geometry.json and the chip files, everything erased but the partition table.
pub fn write_store(dir: &Path, g: &Geometry) -> Result<()> {
    std::fs::create_dir_all(dir)?;
    std::fs::write(dir.join("geometry.json"), crate::lock::pretty(&g.json()))?;
    let ptab = build_ptab(g);
    let stride = g.stride() as usize;
    let mut record = vec![0xffu8; stride];
    record[..ptab.len()].copy_from_slice(&ptab);
    let stored: Vec<u8> = record.iter().map(|b| b ^ 0xff).collect();
    for ch in 0..CHANNELS {
        for ce in 0..CE_PER_CHANNEL {
            let path = dir.join(format!("ch{ch}-ce{ce}.pages"));
            let f = crate::posio::create_sparse(&path).map_err(|e| crate::internal!("{}: {e}", path.display()))?;
            f.set_len(g.chip_bytes()).map_err(|e| crate::internal!("{}: {e}", path.display()))?;
            for page in 0..2u64 {
                crate::posio::write_all_at(&f, &stored, page * g.stride())
                    .map_err(|e| crate::internal!("{}: {e}", path.display()))?;
            }
        }
    }
    Ok(())
}

/// markers.json: the host files behind the marker pages, by index (source 0 the system volume, 1 the data volume's
/// seed). Paths relative to the store, so the device folder can move.
pub fn write_markers(dir: &Path, sources: &[(&str, u64, &str)]) -> Result<()> {
    let list: Vec<Value> = sources.iter().map(|(p, bytes, what)| json!({"path": p, "bytes": bytes, "what": what})).collect();
    let v = json!({
        "format": "iphonemu-markers-v1", "magic": String::from_utf8_lossy(MARKER_MAGIC), "check": MARKER_CHECK,
        "block": BLOCK, "sources": list,
    });
    std::fs::write(dir.join("markers.json"), crate::lock::pretty(&v))?;
    Ok(())
}

/// The 4 KiB blocks of `image` that hold data, as runs (first block, count): its sparse extents, less the blocks there
/// that are all zeros. `all` takes every block (the system volume: every one is the guest's to read).
pub fn data_runs(image: &Path, all: bool) -> Result<Vec<(u32, u32)>> {
    let file = std::fs::File::open(image).map_err(|e| crate::internal!("{}: {e}", image.display()))?;
    let len = file.metadata()?.len();
    let blocks = len.div_ceil(BLOCK) as u32;
    if all {
        return Ok(vec![(0, blocks)]);
    }
    let extents = crate::k48nand_image::data_extents(&file).map_err(|e| crate::internal!("{}: {e}", image.display()))?;
    let mut runs: Vec<(u32, u32)> = Vec::new();
    let mut buf = vec![0u8; 1 << 20];
    for (start, end) in extents {
        let mut at = start / BLOCK * BLOCK;
        while at < end.min(len) {
            let n = ((end.min(len) - at).min(buf.len() as u64) as usize).div_ceil(BLOCK as usize) * BLOCK as usize;
            let n = n.min((len - at) as usize);
            crate::posio::read_exact_at(&file, &mut buf[..n], at).map_err(|e| crate::internal!("{}: {e}", image.display()))?;
            for (k, block) in buf[..n].chunks(BLOCK as usize).enumerate() {
                if block.iter().all(|&b| b == 0) {
                    continue;
                }
                let b = (at / BLOCK) as u32 + k as u32;
                match runs.last_mut() {
                    Some((first, count)) if *first + *count == b => *count += 1,
                    _ => runs.push((b, 1)),
                }
            }
            at += n as u64;
        }
    }
    Ok(runs)
}

/// A symbolic link of the seed, which it_install makes itself (its kernel won't read a link's target off the seed).
pub struct Link {
    pub path: String,
    pub target: String,
    pub uid: u32,
    pub gid: u32,
}

/// The seed's symbolic links (paths relative to its root).
pub fn seed_links(seed: &Path) -> Result<Vec<Link>> {
    let ed = crate::hfs::Editor::open_read_only(seed)?;
    let mut links = Vec::new();
    for path in ed.walk("")? {
        if let Some(meta) = ed.lstat(&path)?
            && matches!(meta.kind, crate::hfs::editor::Kind::Symlink)
        {
            links.push(Link { target: ed.read_link(&path)?, path, uid: meta.uid, gid: meta.gid });
        }
    }
    Ok(links)
}

/// it_install's table (its format is in qemu-ios contrib/it-install/it_install.c): the marker parameters, the
/// system partition, the seed's size, the runs of each source, and the seed's symbolic links.
pub fn install_table(system_blocks: u32, seed_blocks: u32, runs: &[(u32, u32, u32)], links: &[Link]) -> Vec<u8> {
    let mut t = b"ITINST02".to_vec();
    t.extend(MARKER_CHECK.to_le_bytes());
    t.extend(MARKER_MAGIC);
    for v in [SYSTEM_LBA, system_blocks, seed_blocks, runs.len() as u32] {
        t.extend(v.to_le_bytes());
    }
    for (source, first, count) in runs {
        for v in [*source, *first, *count] {
            t.extend(v.to_le_bytes());
        }
    }
    t.extend((links.len() as u32).to_le_bytes());
    for l in links {
        t.extend((l.path.len() as u16).to_le_bytes());
        t.extend((l.target.len() as u16).to_le_bytes());
        t.extend(l.uid.to_le_bytes());
        t.extend(l.gid.to_le_bytes());
        t.extend(l.path.as_bytes());
        t.extend(l.target.as_bytes());
    }
    t
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn geometry_scales() {
        let g = Geometry::for_gib(16).unwrap();
        assert_eq!((g.blocks_per_cau, g.block_bits), (4096, 12));
        assert_eq!(Geometry::for_gib(8).unwrap().disk_blocks(), 1_953_125);
        assert_eq!(g.disk_blocks(), 3_906_250);
    }

    #[test]
    fn table_is_little_endian() {
        let t = install_table(10, 20, &[(0, 0, 10)], &[]);
        assert_eq!(&t[..8], b"ITINST02");
        assert_eq!(t.len(), 44 + 12 + 4);
        assert_eq!(u32::from_le_bytes(t[32..36].try_into().unwrap()), 10);
    }
}
