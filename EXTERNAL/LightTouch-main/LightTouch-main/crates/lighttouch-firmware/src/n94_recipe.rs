//! N94Board: the iPhone 4S (n94ap, the A5, recipe "n94"; experimental), on the engine's -M iPhone-4S. Its machine
//! and the way it's installed come from iPhonEmu (https://github.com/kazin1898/iPhonEmu), which first booted it.
//!
//! What differs from the A4 boards (k48_recipe):
//! - The boot image: kboot.bin at the A5's DRAM base, with the store's PPN geometry in the device tree (kboot Extra).
//! - The NAND: a PPN page store (n94nand) the host can't write volumes into, since the 4S's FTL (AppleSwissPPNFTL)
//!   keeps its own layout. The volumes are built as for the A4s (system_edits::build_k48: system.hfs and the data
//!   volume's seed, seed.hfs) and kept beside the store, which serves them as marker pages.
//! - The install boot, in place of the keybag one-shot: the IPSW's restore ramdisk boots once on one core with
//!   it_install (guest tools, qemu-ios contrib/it-install) as restored_external. It writes the marker pages through the
//!   FTL, partitions the disk, formats the effaceable storage and the protected data volume, creates the system
//!   keybag, and copies the seed's files into the data volume. A failure fails the preparation.
//! - No seal boot: the install boot's halt flushes the FTL.
use crate::boot::{self, Board as Hardware, DeviceInfo};
use crate::create::{filled, Board, Context, Options};
use crate::error::{Code, Error, Result};
use crate::fitcheck;
use crate::hfs::{Editor, Volume};
use crate::identity::UnitIdentity;
use crate::n94nand::{self, Geometry};
use crate::oneshot::{self, Helper};
use crate::system_edits;
use crate::{files, k48_recipe, kboot};
use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// The install boot's helpers in the guest tools (built by qemu-ios contrib/it-install/build.py).
pub const INSTALL_HELPER: &str = "it_install-n94";
pub const KEYBAG_HELPER: &str = "it_keybag-n94";
/// Where they go on the install ramdisk: root's restored_external (rc.boot execs it), the touch flasher's slot (of no
/// use in the emulator; it_install calls it_keybag there) and it_install's table.
const RAMDISK_INSTALL: &str = "usr/local/bin/restored_external";
const RAMDISK_KEYBAG: &str = "usr/bin/TiSerialFlasher";
const RAMDISK_TABLE: &str = "usr/local/share/it_install.table";
pub const INSTALL_DONE: &str = "it_install: done";
/// The data volume's seed: what firmwarekit's bake puts on the data volume, which the install boot copies in. Room
/// for the jailbreak's /private/var (Cydia's stash) too; the file is sparse.
const SEED_BYTES: u64 = 1 << 30;
/// The volumes beside the store, as the markers name them (relative to nand/).
const SYSTEM_FILE: &str = "system.hfs";
const SEED_FILE: &str = "seed.hfs";

pub struct N94Board {
    recipe: crate::entry::Recipe,
    version: String,
    geometry: Geometry,
    helper: Option<Helper>,
    hardware: Option<DeviceInfo>,
    ident: Option<UnitIdentity>,
    built: Option<system_edits::Built>,
    ramdisk: Option<String>,
    /// The install boot's limit: it writes the system volume's markers (about 40 s for 6.1.3) and copies the seed.
    timeout: f64,
}

impl N94Board {
    pub fn new(o: &Options) -> Result<N94Board> {
        let e = &o.entry;
        let recipe = e.recipe.clone().ok_or_else(|| crate::unsupported!("{}: no recipe", e.id))?;
        Ok(N94Board {
            geometry: Geometry::for_storage(&recipe.storage)?,
            version: e.version.clone(),
            recipe,
            helper: None,
            hardware: None,
            ident: None,
            built: None,
            ramdisk: None,
            timeout: 1800.0,
        })
    }

    fn system_options(&self) -> system_edits::Options {
        let mut o = system_edits::Options::new(&self.recipe, &self.version);
        o.bluetooth = false; // nothing answers the HCI on uart3
        o.seal = false; // no seal boot: the install boot's halt flushes the FTL
        o.web_proxy = false; // no Wi-Fi model: nothing to proxy
        o
    }

    /// The kboot extras: the store's geometry, on one core (the install boot) or both.
    fn extra(&self, install: bool) -> kboot::Extra {
        kboot::Extra {
            ppn: Some(self.geometry.clone()),
            cpus: if install { 1 } else { 2 },
            hide: if install { vec!["arm-io/ae2"] } else { Vec::new() },
        }
    }
}

fn read(path: &Path) -> Result<Vec<u8>> {
    std::fs::read(path).map_err(|e| crate::internal!("{}: {e}", path.display()))
}

/// A private copy of the restore ramdisk with the install helpers and the table in it.
fn install_ramdisk(src: &Path, tools: &Path, table: &[u8], work: &Path) -> Result<PathBuf> {
    let rd = work.join("install-ramdisk.dmg");
    std::fs::copy(src, &rd)?;
    let install = read(&tools.join(INSTALL_HELPER))?;
    let keybag = read(&tools.join(KEYBAG_HELPER))?;
    let room = install.len() + keybag.len() + table.len() + (1 << 20);
    let size = std::fs::metadata(&rd)?.len();
    Editor::grow(&rd, (size + room as u64 + 4095) / 4096 * 4096)?;
    let mut ed = Editor::open(&rd)?;
    for dir in ["usr/local/bin", "usr/local/share", "usr/bin"] {
        ed.mkdirs(dir)?;
    }
    ed.write(RAMDISK_INSTALL, install, Some(0o755))?;
    ed.write(RAMDISK_KEYBAG, keybag, Some(0o755))?;
    ed.write(RAMDISK_TABLE, table.to_vec(), Some(0o644))?;
    ed.commit()?;
    Volume::open(&rd, true)?.set_owner(&[RAMDISK_INSTALL, RAMDISK_KEYBAG, RAMDISK_TABLE], 0, 0, None, None)?;
    Ok(rd)
}

/// Clears kHFSContentProtectionBit in a volume's two headers (at 1024 and 1024 before its end). The seed is only read
/// from, by it_install: a protected volume mounted with no keys of its own is refused (mount_hfs: Invalid argument),
/// and the data volume it's copied into gets its protection as the install makes it.
fn unprotect(image: &Path) -> Result<()> {
    let f = std::fs::File::options().read(true).write(true).open(image).map_err(|e| crate::internal!("{}: {e}", image.display()))?;
    let len = f.metadata()?.len();
    for header in [1024, len - 1024] {
        let mut attributes = [0u8; 4];
        crate::posio::read_exact_at(&f, &mut attributes, header + 4).map_err(|e| crate::internal!("{}: {e}", image.display()))?;
        let value = u32::from_be_bytes(attributes) & !(1 << 30);
        crate::posio::write_all_at(&f, &value.to_be_bytes(), header + 4).map_err(|e| crate::internal!("{}: {e}", image.display()))?;
    }
    Ok(())
}

impl Board for N94Board {
    fn arch(&self) -> &str {
        "armv7"
    }
    fn seed_prefix(&self) -> String {
        "iphone4s".into()
    }
    fn boot_step(&self) -> String {
        "Writing the identity and boot image".into()
    }
    fn volumes_step(&self) -> String {
        "Building the system and data volumes".into()
    }
    /// The install boot runs in the keybag step's place: it creates the keybag among the rest.
    fn keybag_step(&self) -> String {
        "Installing iOS on the NAND".into()
    }
    fn data_protection(&self) -> bool {
        true
    }
    fn needs_seal(&self) -> bool {
        false
    }
    /// The store stays a directory of chip files: the engine's A5 model reads them, not a container.
    fn packs_store(&self) -> bool {
        false
    }
    fn shipped(&self) -> Vec<String> {
        vec!["kboot.bin".into(), SYSTEM_FILE.into(), SEED_FILE.into()]
    }

    fn check(&mut self, c: &mut Context) -> Result<()> {
        for tool in [INSTALL_HELPER, KEYBAG_HELPER] {
            if !c.o.guest_tools.join(tool).is_file() {
                return Err(crate::internal!("guest helper {tool} missing from {}", c.o.guest_tools.display()));
            }
        }
        let helper = c.o.helper.clone();
        self.hardware = Some(helper.hardware(Hardware::N94)?);
        self.helper = Some(helper);
        Ok(())
    }

    fn identity(&mut self, _c: &mut Context, seed: &str, path: &Path) -> Result<Option<String>> {
        // A phone: lockdownd and MobileGestalt hash the IMEI into the UDID.
        let ident = UnitIdentity::synthesize(seed, &self.recipe.storage, Some(kboot::Board::n94().model_number))?.adding_imei(seed);
        ident.write(path)?;
        let udid = ident.udid().map(str::to_string);
        self.ident = Some(ident);
        Ok(udid)
    }

    fn boot_files(&mut self, c: &mut Context) -> Result<()> {
        let ident = self.ident.as_ref().ok_or_else(|| filled("the identity"))?;
        let args = k48_recipe::boot_args(c.cydia.is_some());
        let kernel = read(&c.dec_file("kernelcache.mach")?)?;
        fitcheck::check_boot_args(&c.fit, Some(&kernel), &args)?;
        let mut boot_kernel = None;
        if c.cydia.is_some() {
            c.fit.check(fitcheck::sandbox_patch(Some(&kernel)), false, "left out")?;
            let log = |s: &str| c.log(s);
            boot_kernel = Some(k48_recipe::jailbreak_kernel(&kernel, &log));
        }
        let image = kboot::make_extra(c.decrypted()?, ident, &args, None, boot_kernel.as_deref(), &self.extra(false))?;
        std::fs::write(c.file("kboot.bin"), image)?;
        Ok(())
    }

    fn volumes(&mut self, c: &mut Context) -> Result<()> {
        // The system partition: at least what a restore gives this unit (MinimumSystemPartition + padding).
        let stock = c.restore.as_ref().and_then(|r| r.system_partition_mib(&self.recipe.storage)).unwrap_or(0);
        let system_mib = self.recipe.system_mib.max(stock);
        c.log(&format!("system partition {system_mib} MiB (catalog {}, restore {stock})", self.recipe.system_mib));
        let mut options = self.system_options();
        options.cydia = c.cydia.clone();
        options.language = c.o.language.clone();
        options.keyboards = c.o.keyboards.clone();
        let uuid_hash = files::sha256(format!("n94 data volume {}", c.seed).as_bytes());
        let mut uuid = [0u8; 8];
        uuid.copy_from_slice(&hex::decode(&uuid_hash[..16]).unwrap_or_default());
        let built = system_edits::build_k48(
            &c.dec_file("rootfs.dmg")?,
            &c.work,
            system_mib as u64 * (1 << 20),
            SEED_BYTES,
            &options,
            &c.o.guest_tools,
            None,
            Some(read(&c.dec_file("kernelcache.mach")?)?),
            Some(uuid),
            &c.fit,
            c.now,
            &|l| c.log(l),
        )?;
        for n in &built.notes {
            (c.emit)(crate::progress::Event::Warning(n.clone()));
        }
        c.activation = built.activation.clone();
        c.guest_package = built.guest_package.clone();
        c.engine = built.engine.clone();
        self.built = Some(built);
        Ok(())
    }

    /// The page store (blank but for its partition table) and the volumes beside it, which its markers name.
    fn store(&mut self, c: &mut Context) -> Result<()> {
        let built = self.built.as_ref().ok_or_else(|| filled("the volumes"))?;
        n94nand::write_store(&c.nand, &self.geometry)?;
        std::fs::rename(&built.system, c.file(SYSTEM_FILE))?;
        std::fs::rename(&built.data, c.file(SEED_FILE))?;
        unprotect(&c.file(SEED_FILE))?;
        let size = |n: &str| std::fs::metadata(c.file(n)).map(|m| m.len());
        n94nand::write_markers(
            &c.nand,
            &[
                (&format!("../{SYSTEM_FILE}"), size(SYSTEM_FILE)?, "system volume"),
                (&format!("../{SEED_FILE}"), size(SEED_FILE)?, "data volume seed"),
            ],
        )?;
        Ok(())
    }

    /// The install boot (see the module's notes). One attempt: a panic, a FAIL or no halt fails the preparation.
    fn keybag(&mut self, c: &mut Context) -> Result<()> {
        let system_blocks = (std::fs::metadata(c.file(SYSTEM_FILE))?.len() / n94nand::BLOCK) as u32;
        let seed_blocks = (std::fs::metadata(c.file(SEED_FILE))?.len() / n94nand::BLOCK) as u32;
        let disk = self.geometry.disk_blocks();
        if disk < (n94nand::SYSTEM_LBA + system_blocks + seed_blocks) as u64 + 65536 {
            return Err(crate::unsupported!("{}: the volumes don't fit a {} store", c.e().id, self.recipe.storage));
        }
        let mut runs: Vec<(u32, u32, u32)> =
            n94nand::data_runs(&c.file(SYSTEM_FILE), true)?.into_iter().map(|(f, n)| (0, f, n)).collect();
        runs.extend(n94nand::data_runs(&c.file(SEED_FILE), false)?.into_iter().map(|(f, n)| (1, f, n)));
        c.log(&format!("install table: system {system_blocks} blocks, seed {seed_blocks} blocks, {} runs", runs.len()));
        let links = n94nand::seed_links(&c.file(SEED_FILE))?;
        let table = n94nand::install_table(system_blocks, seed_blocks, &runs, &links);

        let (source, name) = k48_recipe::keybag_ramdisk(c)?;
        self.ramdisk = Some(name);
        let rd = install_ramdisk(&source, &c.o.guest_tools, &table, &c.work)?;
        let ident = self.ident.as_ref().ok_or_else(|| filled("the identity"))?;
        let kboot_install = c.work.join("kboot-install.bin");
        // nand-enable-reformat=1: the FTL formats the blank store rather than refusing it
        let args = format!("{} nand-enable-reformat=1", kboot::DEFAULT_BOOT_ARGS);
        std::fs::write(&kboot_install, kboot::make_extra(c.decrypted()?, ident, &args, Some(&rd), None, &self.extra(true))?)?;

        let hardware = self.hardware.as_ref().ok_or_else(|| filled("the board's hardware"))?;
        let serial = c.work.join("install.log");
        let d = boot::N94 { kboot: kboot_install.to_string_lossy().into_owned(), nand: c.nand.to_string_lossy().into_owned(), overlay: None, cpus: 1 };
        let config = boot::n94(&d, hardware, &format!("file:{}", serial.display()));
        let helper = self.helper.as_ref().ok_or_else(|| filled("the helper"))?;
        let (r, text) = oneshot::run(helper, &config.argv, &serial, Some("panic("), None, None, self.timeout, &|l| c.log(l), None)?;
        for line in text.lines().filter(|l| l.contains("it_install:") || l.contains("it_keybag:")) {
            c.log(line);
        }
        if !(r.exited && text.contains(INSTALL_DONE)) {
            let why = text
                .lines()
                .find_map(|l| {
                    l.find("it_install: FAIL").or_else(|| l.find("panic(")).map(|i| l[i..].chars().take(160).collect::<String>())
                })
                .unwrap_or_else(|| if r.exited { "halted before it_install finished" } else { "no halt" }.to_string());
            return Err(Error { code: Code::OneshotFailed, message: format!("install boot: {why}"), piece: None });
        }
        let _ = std::fs::remove_file(&rd);
        let _ = std::fs::remove_file(&kboot_install);
        Ok(())
    }

    fn lock(&mut self, c: &Context) -> Result<Value> {
        let built = self.built.as_ref().ok_or_else(|| filled("the volumes"))?;
        let record = |n: &str| -> Result<Value> { Ok(json!({"path": n, "sha256": files::sha256_file(&c.file(n))?})) };
        let files_map: serde_json::Map<String, Value> = c.nand_hashes.iter().map(|(f, h)| (f.clone(), Value::String(h.clone()))).collect();
        let helper = &c.o.helper_path;
        Ok(json!({
            "boot_strategy": "kboot",
            "tool": {
                "helper": helper.to_string_lossy(), "helper_sha256": files::sha256_file(helper)?,
                "built": {"OpenGLES": built.engine.clone().map(Value::String).unwrap_or(Value::Null)},
            },
            "inputs": {
                "kernelcache": "kernelcache.mach", "devicetree": "DeviceTree.bin",
                "restore_ramdisk": self.ramdisk.clone().map(Value::String).unwrap_or(Value::Null),
            },
            "outputs": {
                "nand": {"files": files_map},
                "kboot": record("kboot.bin")?,
                "system": record(SYSTEM_FILE)?,
                "seed": record(SEED_FILE)?,
            },
            "skip_setup": self.system_options().skip_setup,
            // The engine's iPhone-4S takes no per-unit options (no modem, no Wi-Fi model).
            "machine": BTreeMap::<String, String>::new(),
        }))
    }
}
