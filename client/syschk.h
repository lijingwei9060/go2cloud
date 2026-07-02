/*
 * syschk.h — pre-migration system environment check
 *
 * Validates the source machine is ready for migration by checking:
 *   - Administrator privileges
 *   - Windows version support
 *   - VSS (Volume Shadow Copy) service status
 *   - Disk enumeration and accessibility
 *   - System disk free space
 *   - VirtIO driver pre-install status
 *   - BIOS / firmware type (UEFI or Legacy)
 *   - Basic network connectivity (optional)
 *
 * Each check returns a result with PASS/WARN/FAIL and a message.
 * Run system_check_all() before starting a migration to catch issues early.
 */

#ifndef CLIENT_SYSCHK_H
#define CLIENT_SYSCHK_H

#ifdef __cplusplus
extern "C" {
#endif

#define SYSCHK_MAX_CHECKS  16

/* Check severity */
#define SYSCHK_PASS  0  /* green: everything ok */
#define SYSCHK_WARN  1  /* yellow: may cause issues */
#define SYSCHK_FAIL  2  /* red: migration will likely fail */
#define SYSCHK_INFO  3  /* informational only */

/* One check result */
typedef struct {
    const char *label;       /* short label, e.g. "Admin rights" */
    int         severity;    /* SYSCHK_PASS / WARN / FAIL / INFO */
    char        message[512];/* detail message */
} syschk_item_t;

/* Collection of check results */
typedef struct {
    syschk_item_t items[SYSCHK_MAX_CHECKS];
    int           count;
    int           pass_count;
    int           warn_count;
    int           fail_count;
} syschk_result_t;

/*
 * Run all system checks.
 *
 * driver_dir: path to VirtIO driver directory (can be NULL to skip driver check)
 *
 * Returns 0 if all checks pass, -1 if any check fails.
 * Detailed results are written to the result struct and to the log.
 */
int system_check_run(syschk_result_t *result, const char *driver_dir);

/*
 * Print a formatted summary of all checks to stdout.
 */
void system_check_print(const syschk_result_t *result);

/*
 * Individual check functions — useful for targeted verification.
 * Each returns SYSCHK_PASS (0), SYSCHK_WARN (1), or SYSCHK_FAIL (2).
 */

int check_admin_rights(char *msg, size_t msg_size);
int check_windows_version(char *msg, size_t msg_size);
int check_vss_service(char *msg, size_t msg_size);
int check_disks(char *msg, size_t msg_size);
int check_disk_space(char *msg, size_t msg_size);
int check_bios_mode(char *msg, size_t msg_size);
int check_virtio_drivers(const char *driver_dir, char *msg, size_t msg_size);
int check_network(const char *server_ip, int port, char *msg, size_t msg_size);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_SYSCHK_H */
