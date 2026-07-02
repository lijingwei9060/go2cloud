/*
 * driver_inject.h — VirtIO driver pre-install for Windows KVM migration
 *
 * Pre-stages VirtIO drivers (viostor, netkvm, qxldod) into the Windows
 * driver store so that the target VM boots without 0x7B BSOD when it
 * encounters KVM virtio hardware.
 *
 * Uses Windows Setup API (setupapi.dll) — no external tools needed.
 * All functions are no-ops on non-Windows platforms.
 */

#ifndef CLIENT_DRIVER_INJECT_H
#define CLIENT_DRIVER_INJECT_H

#ifdef __cplusplus
extern "C" {
#endif

/* One driver's install/check result */
typedef struct {
    const char *name;          /* e.g. "viostor" */
    const char *hwid;          /* e.g. "PCI\\VEN_1AF4&DEV_1001&..." */
    int         installed;     /* 1 = already in driver store before call */
    int         just_installed;/* 1 = freshly installed by this call */
    int         reboot_required;
    char        error[256];
} driver_result_t;

/*
 * Install a driver via devcon.exe for a specific hardware ID.
 *
 * inf_path:        path to the .inf file
 * hwid:            hardware ID (e.g. "PCI\\VEN_1AF4&DEV_1001&SUBSYS_00021AF4&REV_00")
 * reboot_required: [out] 1 if reboot needed to complete install
 *
 * Uses: devcon install <inf> <hwid>
 * Idempotent: returns success if driver is already installed.
 */
int driver_inject_install(const char *inf_path, const char *hwid, int *reboot_required);

/*
 * Check whether a driver is installed for a given hardware ID.
 *
 * hwid: hardware ID (partial match — VEN+DEV prefix is used)
 *
 * Uses: devcon find <hwid_prefix>*
 * Returns 1 if installed, 0 if not installed, -1 on error.
 */
int driver_inject_probe(const char *hwid);

/*
 * Install all three VirtIO drivers from a driver directory.
 *
 * driver_dir: path containing viostor/, netkvm/, qxldod/ subdirectories
 *             each with version-specific install folders (e.g. w10/amd64/).
 * results:    [out] array of 3 driver_result_t, filled per-driver
 *
 * Returns 0 if all three are now in the store, -1 if any failed.
 */
int driver_inject_virtio(const char *driver_dir, driver_result_t results[3]);

/*
 * Check all three VirtIO drivers without installing.
 *
 * driver_dir: same layout as driver_inject_virtio()
 * results:    [out] array of 3 driver_result_t
 *
 * Returns 0 if all three are already installed, -1 if any missing or error.
 */
int driver_check_virtio(const char *driver_dir, driver_result_t results[3]);

/*
 * Find the correct driver INF for the current Windows version.
 *
 * Given a driver install directory (e.g. "drivers/viostor/Install"), searches
 * for the best-matching Windows version folder and returns the path to the
 * INF file inside.  Prefers <driver_name>.inf over arbitrary *.inf files.
 *
 * install_dir: e.g. "drivers/viostor/Install"
 * driver_name: e.g. "viostor" — used to prefer <name>.inf over *.inf glob
 * buf:         output buffer for full INF path
 * buf_size:    size of buf
 *
 * Returns 0 on success, -1 if no matching INF found.
 */
int driver_find_inf(const char *install_dir, const char *driver_name,
                    char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_DRIVER_INJECT_H */
