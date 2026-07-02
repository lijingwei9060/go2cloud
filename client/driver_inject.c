/*
 * driver_inject.c — VirtIO driver pre-install for Windows KVM migration
 *
 * Uses devcon.exe (Microsoft Device Console) to install and probe VirtIO
 * drivers.  devcon install creates a phantom device node bound to the given
 * hardware ID — the driver loads on the source machine just enough to
 * register itself, then sits dormant until the VM boots on KVM where the
 * matching virtio PCI hardware actually exists.
 *
 * devcon.exe must be in the same directory as client.exe, or in d:\migrate\.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <sys/stat.h>

#include "log.h"
#include "driver_inject.h"

#ifdef _WIN32

/* ---- helpers ---- */

typedef LONG (WINAPI *RtlGetVersion_t)(PRTL_OSVERSIONINFOW);

static int get_windows_version(DWORD *major, DWORD *minor, DWORD *build)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return -1;
    RtlGetVersion_t fn = (RtlGetVersion_t)GetProcAddress(ntdll, "RtlGetVersion");
    if (!fn) return -1;
    RTL_OSVERSIONINFOW vi = {0};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return -1;
    *major = vi.dwMajorVersion;
    *minor = vi.dwMinorVersion;
    *build = vi.dwBuildNumber;
    return 0;
}

static int folder_matches_os(const char *folder)
{
    DWORD major = 0, minor = 0, build = 0;
    if (get_windows_version(&major, &minor, &build) != 0) return 0;
    (void)build;

    char lower[32];
    int i;
    for (i = 0; folder[i] && i < 31; i++)
        lower[i] = (char)tolower((unsigned char)folder[i]);
    lower[i] = '\0';

    if (major == 10) {
        return (strstr(lower, "w10")  || strstr(lower, "win10") ||
                strstr(lower, "w11")  || strstr(lower, "win11") ||
                strstr(lower, "2k16") || strstr(lower, "2016") ||
                strstr(lower, "2k19") || strstr(lower, "2019") ||
                strstr(lower, "2k22") || strstr(lower, "2022")) ? 1 : 0;
    }
    if (major == 6 && minor == 3)
        return (strstr(lower, "w8.1") || strstr(lower, "win8.1") ||
                strstr(lower, "2k12r2") || strstr(lower, "2012r2")) ? 1 : 0;
    if (major == 6 && minor == 2)
        return (strstr(lower, "w8") || strstr(lower, "win8") ||
                strstr(lower, "2k12") || strstr(lower, "2012")) ? 1 : 0;
    if (major == 6 && minor == 1)
        return (strstr(lower, "w7") || strstr(lower, "win7") ||
                strstr(lower, "2k8r2") || strstr(lower, "2008r2")) ? 1 : 0;
    return 0;
}

/*
 * Locate devcon.exe.  Tries:  current dir, D:\migrate\, PATH.
 * Writes the full path to buf and returns 0, or -1 if not found.
 */
static int find_devcon(char *buf, size_t buf_size)
{
    static const char *candidates[] = {
        "devcon.exe",
        "D:\\migrate\\devcon.exe",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (GetFileAttributesA(candidates[i]) != INVALID_FILE_ATTRIBUTES) {
            GetFullPathNameA(candidates[i], (DWORD)buf_size, buf, NULL);
            return 0;
        }
    }
    /* Last resort: search PATH */
    char path_test[MAX_PATH];
    DWORD len = SearchPathA(NULL, "devcon.exe", NULL, (DWORD)sizeof(path_test), path_test, NULL);
    if (len > 0 && len < sizeof(path_test)) {
        strncpy(buf, path_test, buf_size - 1);
        return 0;
    }
    return -1;
}

/*
 * Run devcon.exe with the given arguments.
 * Captures stdout into `out` (if non-NULL) up to `out_size` bytes.
 * Returns devcon's exit code, or -1 if the process could not be started.
 */
static int devcon_run(const char *args, char *out, size_t out_size)
{
    char devcon_path[MAX_PATH];
    if (find_devcon(devcon_path, sizeof(devcon_path)) != 0) {
        LOG_ERROR("driver: devcon.exe not found");
        return -1;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "\"%s\" %s", devcon_path, args);
    LOG_INFO("driver: %s", cmd);

    /* Create pipes for stdout capture */
    HANDLE h_read = NULL, h_write = NULL;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    if (out && !CreatePipe(&h_read, &h_write, &sa, 0)) {
        LOG_ERROR("driver: CreatePipe failed (err=%lu)", GetLastError());
        return -1;
    }

    /* Don't inherit the read end */
    if (h_read) SetHandleInformation(h_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out ? h_write : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = out ? h_write : GetStdHandle(STD_ERROR_HANDLE);

    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        LOG_ERROR("driver: CreateProcess failed (err=%lu)", GetLastError());
        if (h_write) CloseHandle(h_write);
        if (h_read)  CloseHandle(h_read);
        return -1;
    }

    if (h_write) CloseHandle(h_write);
    h_write = NULL;

    /* Read output */
    if (out && out_size > 0) {
        DWORD total = 0;
        char buf[256];
        DWORD nread;
        while (ReadFile(h_read, buf, sizeof(buf) - 1, &nread, NULL) && nread > 0) {
            if (total + nread >= out_size) nread = (DWORD)(out_size - total - 1);
            if (nread == 0) break;
            memcpy(out + total, buf, nread);
            total += nread;
        }
        out[total] = '\0';
    }
    if (h_read) CloseHandle(h_read);

    WaitForSingleObject(pi.hProcess, 30000); /* 30s timeout */
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (out && out[0]) {
        /* Trim trailing newlines */
        size_t len = strlen(out);
        while (len > 0 && (out[len-1] == '\r' || out[len-1] == '\n')) out[--len] = '\0';
        LOG_INFO("driver: stdout: %s", out);
    }
    LOG_INFO("driver: devcon exit code = %lu", exit_code);
    return (int)exit_code;
}

/* ---- INF search (unchanged from original — keeps driver_find_inf) ---- */

int driver_find_inf(const char *install_dir, const char *driver_name,
                    char *buf, size_t buf_size)
{
    DWORD major = 0, minor = 0, build = 0;
    get_windows_version(&major, &minor, &build);

    LOG_INFO("driver: finding INF for %s in %s (Windows %lu.%lu build %lu)",
             driver_name, install_dir, major, minor, build);

    char search_path[512];
    snprintf(search_path, sizeof(search_path), "%s\\*", install_dir);

    WIN32_FIND_DATAW fd;
    wchar_t wsearch[512];
    MultiByteToWideChar(CP_UTF8, 0, search_path, -1, wsearch, 512);

    HANDLE hfind = FindFirstFileW(wsearch, &fd);
    if (hfind == INVALID_HANDLE_VALUE) {
        char full[512];
        GetFullPathNameA(install_dir, sizeof(full), full, NULL);
        LOG_ERROR("driver: cannot enumerate %s (err=%lu) -- directory missing?", full, GetLastError());
        return -1;
    }

    char best_folder[128] = {0};
    int  best_score = -1;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        char name[64];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), NULL, NULL);

        int score = 0;
        if (folder_matches_os(name)) {
            score = 100;
        } else if (strstr(name, "w10") || strstr(name, "Win10") || strstr(name, "2k16")) {
            score = 50;
        } else if (strstr(name, "w8.1") || strstr(name, "Win8.1") || strstr(name, "2k12R2")) {
            score = 40;
        } else if (strstr(name, "w8") || strstr(name, "Win8") || strstr(name, "2k12")) {
            score = 30;
        } else if (strstr(name, "w7") || strstr(name, "Win7")) {
            score = 20;
        }

        if (score > best_score) {
            best_score = score;
            strncpy(best_folder, name, sizeof(best_folder) - 1);
        }
    } while (FindNextFileW(hfind, &fd));
    FindClose(hfind);

    if (best_folder[0] == '\0') {
        LOG_ERROR("driver: no version folder found in %s", install_dir);
        return -1;
    }

    LOG_INFO("driver: selected version folder '%s' (score=%d)", best_folder, best_score);

    char candidate_paths[2][512];
    snprintf(candidate_paths[0], 512, "%s\\%s\\amd64", install_dir, best_folder);
    snprintf(candidate_paths[1], 512, "%s\\%s", install_dir, best_folder);

    const char *inf_names[4];
    int ninf = 0;
    inf_names[ninf++] = NULL; /* placeholder */
    if (strcmp(driver_name, "viostor") == 0)
        inf_names[ninf++] = "vioscsi.inf";
    inf_names[ninf++] = "*.inf";

    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "%s.inf", driver_name);
    inf_names[0] = name_buf;

    for (int ni = 0; ni < ninf; ni++) {
        for (int i = 0; i < 2; i++) {
            char inf_search[512];
            snprintf(inf_search, sizeof(inf_search), "%s\\%s", candidate_paths[i], inf_names[ni]);

            MultiByteToWideChar(CP_UTF8, 0, inf_search, -1, wsearch, 512);
            WIN32_FIND_DATAW fd2;
            HANDLE hfind2 = FindFirstFileW(wsearch, &fd2);
            if (hfind2 != INVALID_HANDLE_VALUE) {
                WideCharToMultiByte(CP_UTF8, 0, fd2.cFileName, -1,
                                    buf + snprintf(buf, buf_size, "%s\\", candidate_paths[i]),
                                    (int)(buf_size - strlen(candidate_paths[i]) - 1), NULL, NULL);
                FindClose(hfind2);
                char full[512];
                if (GetFullPathNameA(buf, sizeof(full), full, NULL) > 0)
                    strncpy(buf, full, buf_size - 1);
                LOG_INFO("driver: found INF %s", buf);
                return 0;
            }
        }
    }

    LOG_ERROR("driver: no .inf file found in %s\\%s", install_dir, best_folder);
    return -1;
}

/* ---- public API using devcon ---- */

int driver_inject_install(const char *inf_path, const char *hwid, int *reboot_required)
{
    if (reboot_required) *reboot_required = 0;

    if (GetFileAttributesA(inf_path) == INVALID_FILE_ATTRIBUTES) {
        LOG_ERROR("driver: INF file not found: %s", inf_path);
        return -1;
    }

    char args[768];
    snprintf(args, sizeof(args), "install \"%s\" %s", inf_path, hwid);
    int rc = devcon_run(args, NULL, 0);

    if (rc == 0) {
        LOG_INFO("driver: devcon install succeeded for %s", hwid);
        return 0;
    }

    /* devcon returns exit code 1 on reboot-required success */
    /* devcon returns exit code 2 on "already installed" */
    if (rc == 1) {
        LOG_INFO("driver: devcon install succeeded (reboot may be required)");
        if (reboot_required) *reboot_required = 1;
        return 0;
    }
    if (rc == 2) {
        LOG_INFO("driver: driver appears to be already installed");
        return 0;
    }

    LOG_ERROR("driver: devcon install failed (exit code %d)", rc);
    return -1;
}

int driver_inject_probe(const char *hwid)
{
    if (!hwid) return 0;

    /*
     * Use a partial HWID match: strip the SUBSYS and REV parts.
     * e.g. "PCI\VEN_1AF4&DEV_1001*" matches any subsystem/revision variant.
     */
    char short_hwid[128];
    strncpy(short_hwid, hwid, sizeof(short_hwid) - 1);
    /* Truncate at SUBSYS — keep only VEN+DEV for a looser match */
    char *subsys = strstr(short_hwid, "&SUBSYS");
    if (subsys) *subsys = '\0';

    char args[256];
    char out[512] = {0};
    snprintf(args, sizeof(args), "find \"%s*\"", short_hwid);
    int rc = devcon_run(args, out, sizeof(out));

    /* devcon find returns 0 and prints device info if match found */
    if (rc == 0 && out[0] != '\0') {
        LOG_INFO("driver: devcon found matching device for %s", short_hwid);
        return 1;
    }

    LOG_INFO("driver: no matching device for %s", short_hwid);
    return 0;
}

int driver_inject_virtio(const char *driver_dir, driver_result_t results[3])
{
    static const char *names[] = {"viostor", "netkvm", "qxldod"};
    static const char *hwids[] = {
        "PCI\\VEN_1AF4&DEV_1001&SUBSYS_00021AF4&REV_00",
        "PCI\\VEN_1AF4&DEV_1000&SUBSYS_00011AF4&REV_00",
        "PCI\\VEN_1B36&DEV_0100&SUBSYS_11001AF4"
    };

    int all_ok = 1;

    for (int i = 0; i < 3; i++) {
        driver_result_t *r = &results[i];
        memset(r, 0, sizeof(*r));
        r->name = names[i];
        r->hwid = hwids[i];

        /* Find the INF */
        char install_dir[512];
        snprintf(install_dir, sizeof(install_dir), "%s\\%s\\Install", driver_dir, names[i]);

        char inf_path[512];
        if (driver_find_inf(install_dir, names[i], inf_path, sizeof(inf_path)) != 0) {
            snprintf(r->error, sizeof(r->error), "cannot find INF for %s", names[i]);
            LOG_ERROR("driver: %s", r->error);
            all_ok = 0;
            continue;
        }

        /* Check if already installed */
        int probe = driver_inject_probe(hwids[i]);
        if (probe < 0) {
            snprintf(r->error, sizeof(r->error), "probe failed");
            all_ok = 0;
            continue;
        }
        r->installed = probe;

        if (!probe) {
            /* Not yet installed — do it now */
            int reboot = 0;
            if (driver_inject_install(inf_path, hwids[i], &reboot) == 0) {
                r->just_installed = 1;
                r->reboot_required = reboot;
                /* Verify the install took */
                int probe2 = driver_inject_probe(hwids[i]);
                if (probe2 > 0) r->installed = 1;
            } else {
                snprintf(r->error, sizeof(r->error), "devcon install failed");
                all_ok = 0;
            }
        }
    }

    return all_ok ? 0 : -1;
}

int driver_check_virtio(const char *driver_dir, driver_result_t results[3])
{
    static const char *names[] = {"viostor", "netkvm", "qxldod"};
    static const char *hwids[] = {
        "PCI\\VEN_1AF4&DEV_1001&SUBSYS_00021AF4&REV_00",
        "PCI\\VEN_1AF4&DEV_1000&SUBSYS_00011AF4&REV_00",
        "PCI\\VEN_1B36&DEV_0100&SUBSYS_11001AF4"
    };

    int all_installed = 1;
    (void)driver_dir; /* keep for API compat — INF path not needed for probe */

    for (int i = 0; i < 3; i++) {
        driver_result_t *r = &results[i];
        memset(r, 0, sizeof(*r));
        r->name = names[i];
        r->hwid = hwids[i];

        int probe = driver_inject_probe(hwids[i]);
        if (probe < 0) {
            snprintf(r->error, sizeof(r->error), "probe failed");
            all_installed = 0;
            continue;
        }
        r->installed = probe;
        if (!probe) {
            snprintf(r->error, sizeof(r->error), "not installed");
            all_installed = 0;
        }
    }

    return all_installed ? 0 : -1;
}

#else /* !_WIN32 — stub implementations */

int driver_find_inf(const char *install_dir, const char *driver_name,
                    char *buf, size_t buf_size)
{
    (void)install_dir; (void)driver_name; (void)buf; (void)buf_size;
    LOG_WARN("driver: not supported on this platform");
    return -1;
}

int driver_inject_install(const char *inf_path, const char *hwid, int *reboot_required)
{
    (void)inf_path; (void)hwid;
    if (reboot_required) *reboot_required = 0;
    LOG_WARN("driver: driver injection is Windows-only");
    return -1;
}

int driver_inject_probe(const char *hwid)
{
    (void)hwid;
    return 0;
}

int driver_inject_virtio(const char *driver_dir, driver_result_t results[3])
{
    (void)driver_dir;
    memset(results, 0, sizeof(driver_result_t) * 3);
    for (int i = 0; i < 3; i++) {
        results[i].name = (const char*[]){"viostor","netkvm","qxldod"}[i];
        snprintf(results[i].error, sizeof(results[i].error), "platform not supported");
    }
    LOG_WARN("driver: driver injection is Windows-only");
    return -1;
}

int driver_check_virtio(const char *driver_dir, driver_result_t results[3])
{
    (void)driver_dir;
    memset(results, 0, sizeof(driver_result_t) * 3);
    for (int i = 0; i < 3; i++) {
        results[i].name = (const char*[]){"viostor","netkvm","qxldod"}[i];
        snprintf(results[i].error, sizeof(results[i].error), "platform not supported");
    }
    LOG_WARN("driver: driver injection is Windows-only");
    return -1;
}

#endif /* _WIN32 */
