/*
 * syschk.c  --  pre-migration system environment check
 *
 * Each check function returns a severity code (SYSCHK_PASS/WARN/FAIL)
 * and fills the message buffer with a human-readable result.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winsvc.h>
#include <setupapi.h>
#endif

#include "log.h"
#include "syschk.h"
#include "volume.h"
#include "block_io.h"
#include "driver_inject.h"

/* ---- console setup ---- */

static void console_setup(void)
{
#ifdef _WIN32
    SetConsoleOutputCP(65001);
#endif
}

/* ---- individual checks ---- */

int check_admin_rights(char *msg, size_t msg_size)
{
#ifdef _WIN32
    BOOL is_admin = FALSE;
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    PSID admins_group = NULL;

    if (AllocateAndInitializeSid(&nt_auth, 2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &admins_group)) {
        CheckTokenMembership(NULL, admins_group, &is_admin);
        FreeSid(admins_group);
    }

    if (is_admin) {
        snprintf(msg, msg_size, "running with administrator privileges");
        return SYSCHK_PASS;
    } else {
        snprintf(msg, msg_size, "NOT running as administrator  --  PhysicalDrive access requires elevation");
        return SYSCHK_FAIL;
    }
#else
    /* Linux: check effective UID */
    if (geteuid() == 0) {
        snprintf(msg, msg_size, "running as root");
        return SYSCHK_PASS;
    } else {
        snprintf(msg, msg_size, "NOT running as root  --  disk access requires root");
        return SYSCHK_FAIL;
    }
#endif
}

int check_windows_version(char *msg, size_t msg_size)
{
#ifdef _WIN32
    DWORD major = 0, minor = 0, build = 0;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        snprintf(msg, msg_size, "cannot detect Windows version");
        return SYSCHK_WARN;
    }

    /* RtlGetVersion  --  returns actual version, ignoring manifest compat shims */
    typedef LONG (WINAPI *RtlGetVersion_t)(PRTL_OSVERSIONINFOW);
    RtlGetVersion_t fn = (RtlGetVersion_t)GetProcAddress(ntdll, "RtlGetVersion");
    if (!fn) {
        snprintf(msg, msg_size, "cannot detect Windows version");
        return SYSCHK_WARN;
    }

    RTL_OSVERSIONINFOW vi = {0};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) {
        snprintf(msg, msg_size, "RtlGetVersion failed");
        return SYSCHK_WARN;
    }
    major = vi.dwMajorVersion;
    minor = vi.dwMinorVersion;
    build = vi.dwBuildNumber;

    /* Supported: Windows 7+ (6.1+)  and  Server 2008 R2+  */
    if (major >= 10) {
        snprintf(msg, msg_size, "Windows %lu.%lu (build %lu)  --  supported", major, minor, build);
        return SYSCHK_PASS;
    } else if (major == 6 && minor >= 1) {
        snprintf(msg, msg_size, "Windows %lu.%lu (build %lu)  --  supported", major, minor, build);
        return SYSCHK_PASS;
    } else {
        snprintf(msg, msg_size, "Windows %lu.%lu (build %lu)  --  UNSUPPORTED (need 6.1+)", major, minor, build);
        return SYSCHK_FAIL;
    }
#else
    snprintf(msg, msg_size, "not running on Windows");
    return SYSCHK_INFO;
#endif
}

int check_vss_service(char *msg, size_t msg_size)
{
#ifdef _WIN32
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        snprintf(msg, msg_size, "cannot open Service Control Manager (err=%lu)", GetLastError());
        return SYSCHK_WARN;
    }

    SC_HANDLE svc = OpenServiceW(scm, L"VSS", SERVICE_QUERY_STATUS);
    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            snprintf(msg, msg_size, "VSS service not found  --  Volume Shadow Copy unavailable");
        } else {
            snprintf(msg, msg_size, "cannot open VSS service (err=%lu)", err);
        }
        CloseServiceHandle(scm);
        return SYSCHK_FAIL;
    }

    SERVICE_STATUS status;
    if (!QueryServiceStatus(svc, &status)) {
        snprintf(msg, msg_size, "cannot query VSS service status (err=%lu)", GetLastError());
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return SYSCHK_FAIL;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    switch (status.dwCurrentState) {
    case SERVICE_RUNNING:
        snprintf(msg, msg_size, "VSS service is running");
        return SYSCHK_PASS;
    case SERVICE_START_PENDING:
        snprintf(msg, msg_size, "VSS service is starting...");
        return SYSCHK_WARN;
    case SERVICE_STOPPED:
        /* VSS is a demand-start service — it starts on first request and stops when idle. Being stopped is normal. */
        snprintf(msg, msg_size, "VSS service is stopped (demand-start — will auto-start on first snapshot request)");
        return SYSCHK_PASS;
    default:
        snprintf(msg, msg_size, "VSS service state=%lu  --  unexpected", (unsigned long)status.dwCurrentState);
        return SYSCHK_WARN;
    }
#else
    snprintf(msg, msg_size, "not applicable on this platform");
    return SYSCHK_INFO;
#endif
}

int check_disks(char *msg, size_t msg_size)
{
    volume_list_t vol_list;
    if (volume_enumerate(&vol_list) != 0 || vol_list.count == 0) {
        snprintf(msg, msg_size, "no fixed disks found  --  nothing to migrate");
        return SYSCHK_FAIL;
    }

    /* Try opening each disk to verify accessibility */
    int accessible = 0;
    int inaccessible = 0;
    char detail[512] = {0};
    size_t pos = 0;

    for (int i = 0; i < vol_list.count; i++) {
        volume_info_t *vol = &vol_list.volumes[i];
        block_reader_t *r = block_reader_open(vol->disk_path);
        if (r) {
            uint64_t size_gb = block_reader_size(r) / (1024ULL * 1024 * 1024);
            if (pos < sizeof(detail) - 60) {
                pos += snprintf(detail + pos, sizeof(detail) - pos,
                                "%s%s(%.2f GB) ", vol->name[0] ? vol->name : vol->disk_path,
                                vol->name[0] ? " " : "",
                                (double)size_gb);
            }
            block_reader_close(r);
            accessible++;
        } else {
            LOG_WARN("syschk: cannot open %s", vol->disk_path);
            inaccessible++;
        }
    }

    if (inaccessible == 0) {
        snprintf(msg, msg_size, "%d disk(s) accessible: %s", vol_list.count, detail);
        return SYSCHK_PASS;
    } else if (accessible > 0) {
        snprintf(msg, msg_size, "%d/%d disk(s) accessible (%d failed). %s",
                 accessible, vol_list.count, inaccessible, detail);
        return SYSCHK_WARN;
    } else {
        snprintf(msg, msg_size, "cannot access any of %d disk(s)", vol_list.count);
        return SYSCHK_FAIL;
    }
}

int check_disk_space(char *msg, size_t msg_size)
{
#ifdef _WIN32
    char sys_drive[8] = {0};
    if (GetEnvironmentVariableA("SystemDrive", sys_drive, sizeof(sys_drive)) == 0) {
        strcpy(sys_drive, "C:\\");
    } else {
        /* Append trailing backslash if missing (SystemDrive returns "C:" without \) */
        size_t len = strlen(sys_drive);
        if (len > 0 && sys_drive[len - 1] != '\\') {
            strcat(sys_drive, "\\");
        }
    }

    ULARGE_INTEGER free_bytes, total_bytes, total_free_bytes;
    if (!GetDiskFreeSpaceExA(sys_drive, &free_bytes, &total_bytes, &total_free_bytes)) {
        snprintf(msg, msg_size, "cannot query free space on %s (err=%lu)", sys_drive, GetLastError());
        return SYSCHK_WARN;
    }

    uint64_t free_gb  = free_bytes.QuadPart / (1024ULL * 1024 * 1024);
    uint64_t total_gb = total_bytes.QuadPart / (1024ULL * 1024 * 1024);

    /* Recommend at least 1 GB free for SQLite database + logs */
    if (free_gb >= 2) {
        snprintf(msg, msg_size, "%s: %llu GB free / %llu GB total  --  adequate", sys_drive, free_gb, total_gb);
        return SYSCHK_PASS;
    } else if (free_gb >= 1) {
        snprintf(msg, msg_size, "%s: %llu GB free / %llu GB total  --  low (recommend 2+ GB)", sys_drive, free_gb, total_gb);
        return SYSCHK_WARN;
    } else {
        snprintf(msg, msg_size, "%s: %llu GB free / %llu GB total  --  critically low, may cause SQLite errors", sys_drive, free_gb, total_gb);
        return SYSCHK_FAIL;
    }
#else
    snprintf(msg, msg_size, "disk space check not implemented for this platform");
    return SYSCHK_INFO;
#endif
}

int check_bios_mode(char *msg, size_t msg_size)
{
#ifdef _WIN32
    /* Try GetFirmwareType (Win8+/Server2012+) */
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        typedef BOOL (WINAPI *GFT_t)(FIRMWARE_TYPE*);
        GFT_t gft = (GFT_t)GetProcAddress(k32, "GetFirmwareType");
        if (gft) {
            FIRMWARE_TYPE ft = FirmwareTypeUnknown;
            if (gft(&ft)) {
                if (ft == FirmwareTypeUefi) {
                    snprintf(msg, msg_size, "UEFI firmware detected  --  target VM must use UEFI boot");
                    return SYSCHK_PASS;
                } else if (ft == FirmwareTypeBios) {
                    snprintf(msg, msg_size, "Legacy BIOS detected  --  target VM must use BIOS boot");
                    return SYSCHK_PASS;
                }
            }
        }
    }

    /* Fallback: try to access UEFI variables */
    HANDLE h = CreateFileW(
        L"\\\\?\\GLOBALROOT\\Device\\UefiRuntimeService",
        0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, 0, NULL);

    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        snprintf(msg, msg_size, "UEFI firmware detected (via runtime service access)");
        return SYSCHK_PASS;
    }

    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND) {
        snprintf(msg, msg_size, "Legacy BIOS detected (no UEFI runtime)");
        return SYSCHK_PASS;
    }

    snprintf(msg, msg_size, "firmware type could not be determined");
    return SYSCHK_WARN;
#else
    /* Linux: check /sys/firmware/efi */
    FILE *fp = fopen("/sys/firmware/efi", "r");
    if (fp) {
        fclose(fp);
        snprintf(msg, msg_size, "UEFI firmware detected");
    } else {
        snprintf(msg, msg_size, "Legacy BIOS or unknown firmware");
    }
    return SYSCHK_PASS;
#endif
}

int check_virtio_drivers(const char *driver_dir, char *msg, size_t msg_size)
{
    if (!driver_dir) {
        snprintf(msg, msg_size, "no driver directory specified  --  skipped");
        return SYSCHK_INFO;
    }

    driver_result_t results[3];
    driver_check_virtio(driver_dir, results);

    int missing_critical = 0; /* viostor or netkvm missing */
    int missing_qxldod   = 0;
    int installed        = 0;

    for (int i = 0; i < 3; i++) {
        if (results[i].installed) {
            installed++;
        } else if (strcmp(results[i].name, "qxldod") == 0) {
            missing_qxldod = 1; /* non-critical: only affects VNC console */
        } else {
            missing_critical = 1; /* viostor/netkvm: critical for boot/network */
        }
    }

    if (installed == 3) {
        snprintf(msg, msg_size, "all 3 VirtIO drivers are pre-installed");
        return SYSCHK_PASS;
    }

    if (missing_critical && missing_qxldod) {
        snprintf(msg, msg_size, "viostor+netkvm missing (will BSOD), qxldod missing (VNC console only). Run: inject_driver");
        return SYSCHK_FAIL;
    }
    if (missing_critical) {
        snprintf(msg, msg_size, "viostor or netkvm missing  --  VM will BSOD or lose network. Run: inject_driver");
        return SYSCHK_FAIL;
    }
    /* only qxldod missing */
    snprintf(msg, msg_size, "qxldod missing  --  VNC console may not display correctly (non-critical)");
    return SYSCHK_WARN;
}

int check_network(const char *server_ip, int port, char *msg, size_t msg_size)
{
    if (!server_ip) {
        snprintf(msg, msg_size, "no target server specified  --  skipped");
        return SYSCHK_INFO;
    }

#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        snprintf(msg, msg_size, "Winsock init failed");
        return SYSCHK_FAIL;
    }
#endif

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        snprintf(msg, msg_size, "cannot create socket");
#ifdef _WIN32
        WSACleanup();
#endif
        return SYSCHK_FAIL;
    }

    /* Set non-blocking for a quick timeout */
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = inet_addr(server_ip);

    connect(sock, (struct sockaddr*)&addr, sizeof(addr));

    /* Wait up to 3 seconds */
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(sock, &fdset);
    struct timeval tv = {3, 0};

    int sel_rc = select((int)(sock + 1), NULL, &fdset, NULL, &tv);

    int result = SYSCHK_PASS;
    if (sel_rc > 0) {
        /* Check for SO_ERROR */
        int so_error = 0;
        int len = sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
        if (so_error == 0) {
            snprintf(msg, msg_size, "TCP connect to %s:%d succeeded", server_ip, port);
        } else {
            snprintf(msg, msg_size, "TCP connect to %s:%d failed  --  check firewall and server availability", server_ip, port);
            result = SYSCHK_FAIL;
        }
    } else if (sel_rc == 0) {
        snprintf(msg, msg_size, "TCP connect to %s:%d timed out (3s)  --  check firewall and network", server_ip, port);
        result = SYSCHK_FAIL;
    } else {
        snprintf(msg, msg_size, "select error on %s:%d", server_ip, port);
        result = SYSCHK_FAIL;
    }

#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif
    return result;
}

/* ---- batch runner ---- */

int system_check_run(syschk_result_t *result, const char *driver_dir)
{
    if (!result) return -1;

    memset(result, 0, sizeof(*result));
    syschk_item_t *it;

    LOG_INFO("========== system environment check start ==========");

    /* 1. Admin rights */
    it = &result->items[result->count++];
    it->label = "Admin rights";
    it->severity = check_admin_rights(it->message, sizeof(it->message));
    LOG_INFO("check [%s]: %s", it->label, it->message);

    /* 2. Windows version */
    it = &result->items[result->count++];
    it->label = "OS version";
    it->severity = check_windows_version(it->message, sizeof(it->message));
    LOG_INFO("check [%s]: %s", it->label, it->message);

    /* 3. VSS service */
    it = &result->items[result->count++];
    it->label = "VSS service";
    it->severity = check_vss_service(it->message, sizeof(it->message));
    LOG_INFO("check [%s]: %s", it->label, it->message);

    /* 4. Disk accessibility */
    it = &result->items[result->count++];
    it->label = "Disk access";
    it->severity = check_disks(it->message, sizeof(it->message));
    LOG_INFO("check [%s]: %s", it->label, it->message);

    /* 5. System disk space */
    it = &result->items[result->count++];
    it->label = "Disk space";
    it->severity = check_disk_space(it->message, sizeof(it->message));
    LOG_INFO("check [%s]: %s", it->label, it->message);

    /* 6. BIOS / UEFI mode */
    it = &result->items[result->count++];
    it->label = "BIOS mode";
    it->severity = check_bios_mode(it->message, sizeof(it->message));
    LOG_INFO("check [%s]: %s", it->label, it->message);

    /* 7. VirtIO drivers (if driver_dir provided) */
    if (driver_dir) {
        it = &result->items[result->count++];
        it->label = "VirtIO drivers";
        it->severity = check_virtio_drivers(driver_dir, it->message, sizeof(it->message));
        LOG_INFO("check [%s]: %s", it->label, it->message);
    }

    /* Tally */
    int rc = 0;
    for (int i = 0; i < result->count; i++) {
        switch (result->items[i].severity) {
        case SYSCHK_PASS: result->pass_count++; break;
        case SYSCHK_WARN: result->warn_count++; break;
        case SYSCHK_FAIL: result->fail_count++; rc = -1; break;
        }
    }

    LOG_INFO("========== system check complete: %d pass, %d warn, %d fail ==========",
             result->pass_count, result->warn_count, result->fail_count);

    return rc;
}

void system_check_print(const syschk_result_t *result)
{
    if (!result) return;

    console_setup();

    printf("\n");
    printf("  System Environment Check\n");
    printf("  ========================\n\n");

    for (int i = 0; i < result->count; i++) {
        const syschk_item_t *it = &result->items[i];
        const char *icon;

        switch (it->severity) {
        case SYSCHK_PASS: icon = "[PASS]"; break;
        case SYSCHK_WARN: icon = "[WARN]"; break;
        case SYSCHK_FAIL: icon = "[FAIL]"; break;
        default:          icon = "[INFO]"; break;
        }

        printf("  %s %-18s %s\n", icon, it->label, it->message);
    }

    printf("\n");
    printf("  Summary: %d pass, %d warn, %d fail\n",
           result->pass_count, result->warn_count, result->fail_count);

    if (result->fail_count > 0) {
        printf("\n  FAIL -- %d check(s) failed. Fix issues above before migration.\n\n",
               result->fail_count);
    } else if (result->warn_count > 0) {
        printf("\n  WARN -- %d warning(s). Review before proceeding.\n\n",
               result->warn_count);
    } else {
        printf("\n  ALL PASS -- system is ready for migration.\n\n");
    }
}
