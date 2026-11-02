/*
 * it_seal -- one clean shutdown for a freshly built NAND store, then gone.
 *
 * A store fresh from ipad1_nand.py has no YAFTL context ("CXT is not valid .
 * Performing full NAND R/O restore", ~13 s on every boot). The FTL writes one
 * when the kernel halts cleanly, but the stock power-off (SpringBoard's slider)
 * never completes on ipad1 because it waits on Bluetooth. So `ipad1_rootfs.py
 * bake --seal` installs this job for the one sealing boot (imgtools/ipad1_seal.py):
 * it lets the boot settle, deletes itself, and halts through reboot(2), which
 * syncs, unmounts and closes the FTL. docs/research/userland-boot.md.
 * 7.x keeps its root read-only, so the job cannot delete itself there; a marker
 * on the data volume makes the seal happen once all the same, and every later boot's
 * run ends at once. It ends through _exit: the entry is main itself, with no crt1 to
 * return to (contrib/armv6-toolchain/README.md), and a main that returned jumped into
 * whatever dyld last called, which 7.x's dyld reported as a stack buffer overrun.
 * The seal also waits out the first boot's data migration (DataMigrator, while
 * com.apple.DataMigration.plist exists; 6.x moves the preinstalled assets in, then
 * writes Assets/preinstall-is-complete.plist): a halt during it leaves every boot of
 * the base to start it over, behind the Apple logo's progress bar. Then SpringBoard's
 * first launch lays out the Home screen (IconState.plist): a base sealed before it
 * spends every first boot behind the progress bar for a further 90 s. The seal halts
 * a few seconds after both, instead of after fixed sleeps (40 s, then 60 s past the
 * migration): n81ap 6.1.6 on Linux had both done 94 s in, and halted at 154 s. Each
 * wait is capped so a release that keeps the plist, or writes no IconState, still seals.
 * A system with a DataMigrator job migrates on this boot, before SpringBoard's first launch:
 * there the Home screen's layout is the sign the migration is over, and its wait is as long
 * as the migration's. DataMigrator can finish before it_seal starts, or start after it (and
 * FirmwareKit now installs the preinstalled assets itself), so its plist alone tells neither.
 */

extern unsigned int sleep(unsigned int);
extern int unlink(const char *);
extern int access(const char *, int);
extern int open(const char *, int, ...);
extern void sync(void);
extern int reboot(int);
extern long write(int, const void *, unsigned long);
extern void _exit(int);

#define RB_HALT 0x08
#define O_WRONLY 0x0001
#define O_CREAT 0x0200
#define O_EXCL 0x0800
#define MIGRATION "/private/var/mobile/Library/Preferences/com.apple.DataMigration.plist"
#define PREINSTALLED "/System/Library/PreinstalledAssets"
#define PREINSTALL_DONE "/private/var/mobile/Library/Assets/preinstall-is-complete.plist"
#define ICON_STATE "/private/var/mobile/Library/SpringBoard/IconState.plist"
#define MIGRATOR_JOB "/System/Library/LaunchDaemons/com.apple.datamigrator.plist"
#define MIGRATION_WAIT 600          /* seconds, at most, for the migration */
#define HOME_WAIT 60                /* seconds, at most, past it for the Home screen's layout */

/* The migration runs, or is still to come: on a slow host it_seal can get here before DataMigrator has
 * written its plist, so a system with preinstalled assets also waits for their completion marker. */
static int migrating(void)
{
    return access(MIGRATION, 0) == 0 ||
           (access(PREINSTALLED, 0) == 0 && access(PREINSTALL_DONE, 0) != 0);
}

int main(void)
{
    static const char msg[] = "it_seal: halting to seal the NAND store\n";

    if (unlink("/System/Library/LaunchDaemons/com.qemu.it-seal.plist") != 0 &&
        open("/private/var/.it_sealed", O_WRONLY | O_CREAT | O_EXCL, 0644) < 0)
        _exit(0);                   /* read-only root, already sealed */
    int waited = 0;
    for (; waited < MIGRATION_WAIT && migrating(); waited += 2)
        sleep(2);
    int home = access(MIGRATOR_JOB, 0) == 0 ? MIGRATION_WAIT : HOME_WAIT;
    for (waited = 0; waited < home && (migrating() || access(ICON_STATE, 0) != 0); waited += 2)
        sleep(2);
    sleep(5);                       /* the writes behind it settled */
    unlink("/usr/local/bin/it_seal");
    sync();
    write(2, msg, sizeof(msg) - 1);
    reboot(RB_HALT);
    _exit(1);
}
