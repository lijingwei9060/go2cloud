/*
 * vss.c — VSS 卷影复制服务模块实现 (仅 Windows)
 *
 * 通过 vssapi.dll 的 COM 接口创建磁盘快照。
 * 实现完整的 VSS 备份生命周期, 与 decompiled go2tencentcloud 一致。
 *
 * 依赖:
 *   - vssapi.dll     (系统自带, Windows Server / Pro)
 *   - ole32.dll      (COM 基础设施)
 *
 * 编译注意: 需要 vssapi.lib (Windows SDK 自带)。
 */

#include "vss.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "vssapi.lib")
#pragma comment(lib, "advapi32.lib")

/* 内部结构 */
struct vss_context {
    HMODULE              vssapi_dll;
    IVssBackupComponents *backup_components;
    LONG                 context;
    int                  initialized;
    int                  snapshot_set_started;
};

/* 动态加载的函数指针 */
typedef HRESULT (WINAPI *PFN_CreateVssBackupComponents)(
    IVssBackupComponents **ppBackup);

static PFN_CreateVssBackupComponents g_pfn_create = NULL;

vss_context_t *vss_init_ex(LONG context) {
    vss_context_t *ctx = (vss_context_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->context = context;

    /* 加载 vssapi.dll */
    ctx->vssapi_dll = LoadLibraryA("vssapi.dll");
    if (!ctx->vssapi_dll) {
        LOG_ERROR("vss_init_ex: LoadLibrary(vssapi.dll) failed: %lu",
                  GetLastError());
        free(ctx);
        return NULL;
    }

    g_pfn_create = (PFN_CreateVssBackupComponents)
        GetProcAddress(ctx->vssapi_dll, "CreateVssBackupComponentsInternal");
    if (!g_pfn_create) {
        g_pfn_create = (PFN_CreateVssBackupComponents)
            GetProcAddress(ctx->vssapi_dll, "CreateVssBackupComponents");
    }
    if (!g_pfn_create) {
        LOG_ERROR("vss_init_ex: GetProcAddress(CreateVssBackupComponentsInternal) failed");
        FreeLibrary(ctx->vssapi_dll);
        free(ctx);
        return NULL;
    }

    /* 初始化 COM (单线程公寓 — VSS 要求 STA) */
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        LOG_ERROR("vss_init_ex: CoInitializeEx failed: 0x%lx", (unsigned long)hr);
        FreeLibrary(ctx->vssapi_dll);
        free(ctx);
        return NULL;
    }

    /* 创建 VSS 备份组件 */
    hr = g_pfn_create(&ctx->backup_components);
    if (FAILED(hr)) {
        LOG_ERROR("vss_init_ex: CreateVssBackupComponents failed: 0x%lx",
                  (unsigned long)hr);
        CoUninitialize();
        FreeLibrary(ctx->vssapi_dll);
        free(ctx);
        return NULL;
    }

    /* 初始化备份 — 必须在 SetContext 之前 (与原始 go2tencentcloud 一致) */
    hr = ctx->backup_components->InitializeForBackup();
    if (FAILED(hr)) {
        LOG_ERROR("vss_init_ex: InitializeForBackup failed: 0x%lx",
                  (unsigned long)hr);
        ctx->backup_components->Release();
        CoUninitialize();
        FreeLibrary(ctx->vssapi_dll);
        free(ctx);
        return NULL;
    }

    /* 设置上下文 — InitializeForBackup 之后调用 (原始流程) */
    if (context != VSS_CTX_BACKUP) {
        hr = ctx->backup_components->SetContext(context);
        if (FAILED(hr)) {
            LOG_ERROR("vss_init_ex: SetContext(0x%lx) failed: 0x%lx",
                      (unsigned long)context, (unsigned long)hr);
            ctx->backup_components->Release();
            CoUninitialize();
            FreeLibrary(ctx->vssapi_dll);
            free(ctx);
            return NULL;
        }
    }

    ctx->initialized = 1;
    LOG_INFO("vss: initialized (context=0x%lx)", (unsigned long)context);
    return ctx;
}

vss_context_t *vss_init(void) {
    return vss_init_ex(VSS_CTX_BACKUP);
}

void vss_cleanup(vss_context_t *ctx) {
    if (!ctx) return;

    if (ctx->backup_components) {
        ctx->backup_components->Release();
    }

    CoUninitialize();
    if (ctx->vssapi_dll) {
        FreeLibrary(ctx->vssapi_dll);
    }
    free(ctx);
}

int vss_create_snapshots(vss_context_t *ctx,
                         const char *volumes[],
                         vss_snapshot_t *snapshots) {
    if (!ctx || !ctx->initialized || !volumes || !snapshots) {
        LOG_ERROR("vss_create_snapshots: invalid parameters");
        return -1;
    }

    IVssBackupComponents *bc = ctx->backup_components;
    HRESULT hr;
    int snapshot_count = 0;

    /* 持久快照 (VSS_CTX_CLIENT_ACCESSIBLE) 跳过写入器元数据收集 */
    int persistent = (ctx->context & VSS_VOLSNAP_ATTR_NO_WRITERS) != 0;

    /* SetBackupState — 原始代码在持久模式下也调用 (bSelectComponents=FALSE, bBootableSystemState=TRUE) */
    hr = bc->SetBackupState(FALSE, TRUE, VSS_BT_COPY, FALSE);
    if (FAILED(hr)) {
        LOG_ERROR("vss: SetBackupState failed: 0x%lx", (unsigned long)hr);
        return -1;
    }

    if (!persistent) {
        /* GatherWriterMetadata */
        IVssAsync *gather_async = NULL;
        hr = bc->GatherWriterMetadata(&gather_async);
        if (FAILED(hr)) {
            LOG_WARN("vss: GatherWriterMetadata failed: 0x%lx", (unsigned long)hr);
        }
        if (gather_async) {
            gather_async->Wait(30000);
            gather_async->Release();
        }
    }

    /* StartSnapshotSet */
    VSS_ID snapshot_set_id;
    hr = bc->StartSnapshotSet(&snapshot_set_id);
    if (FAILED(hr)) {
        LOG_ERROR("vss: StartSnapshotSet failed: 0x%lx", (unsigned long)hr);
        return -1;
    }
    ctx->snapshot_set_started = 1;

    /* AddToSnapshotSet */
    VSS_ID snapshot_ids[VSS_MAX_VOLUMES];
    int added_count = 0;

    for (int i = 0; i < VSS_MAX_VOLUMES && volumes[i] != NULL; i++) {
        WCHAR wide_vol[64];
        char vol_with_slash[64];
        snprintf(vol_with_slash, sizeof(vol_with_slash), "%s\\", volumes[i]);
        MultiByteToWideChar(CP_ACP, 0, vol_with_slash, -1, wide_vol, 64);

        VSS_ID snapshot_id;
        hr = bc->AddToSnapshotSet(wide_vol, GUID_NULL, &snapshot_id);
        if (FAILED(hr)) {
            LOG_WARN("vss: AddToSnapshotSet(%s) failed: 0x%lx",
                     volumes[i], (unsigned long)hr);
            continue;
        }

        memcpy(&snapshot_ids[added_count], &snapshot_id, sizeof(VSS_ID));
        added_count++;
        LOG_DEBUG("vss: added %s to snapshot set", volumes[i]);
    }

    if (added_count == 0) {
        LOG_ERROR("vss: no volumes added to snapshot set");
        return -1;
    }

    if (!persistent) {
        /* PrepareForBackup */
        IVssAsync *prepare_async = NULL;
        hr = bc->PrepareForBackup(&prepare_async);
        if (FAILED(hr)) {
            LOG_ERROR("vss: PrepareForBackup failed: 0x%lx", (unsigned long)hr);
            return -1;
        }
        if (prepare_async) {
            prepare_async->Wait(30000);
            prepare_async->Release();
        }
    }

    /* DoSnapshotSet */
    IVssAsync *do_async = NULL;
    hr = bc->DoSnapshotSet(&do_async);
    if (FAILED(hr)) {
        LOG_ERROR("vss: DoSnapshotSet failed: 0x%lx", (unsigned long)hr);
        return -1;
    }
    if (do_async) {
        do_async->Wait(0xFFFFFFFF);
        do_async->Release();
    }

    LOG_INFO("vss: snapshot set created (%d volumes), querying properties...",
             added_count);

    /* 查询快照属性 (带重试, 匹配原始) */
    for (int i = 0; i < added_count; i++) {
        VSS_SNAPSHOT_PROP snapshot_prop;
        memset(&snapshot_prop, 0, sizeof(snapshot_prop));

        int retry = 0;
        hr = bc->GetSnapshotProperties(snapshot_ids[i], &snapshot_prop);
        while (FAILED(hr) && retry < 20) {
            Sleep(10);
            hr = bc->GetSnapshotProperties(snapshot_ids[i], &snapshot_prop);
            retry++;
        }

        if (FAILED(hr)) {
            LOG_WARN("vss: GetSnapshotProperties failed for snapshot %d (0x%lx)",
                     i, (unsigned long)hr);
            continue;
        }

        vss_snapshot_t *snap = &snapshots[snapshot_count];
        memset(snap, 0, sizeof(*snap));

        WideCharToMultiByte(CP_ACP, 0,
                            snapshot_prop.m_pwszOriginalVolumeName, -1,
                            snap->original_volume,
                            sizeof(snap->original_volume), NULL, NULL);

        WideCharToMultiByte(CP_ACP, 0,
                            snapshot_prop.m_pwszSnapshotDeviceObject, -1,
                            snap->snapshot_path,
                            sizeof(snap->snapshot_path), NULL, NULL);

        memcpy(snap->snapshot_id, &snapshot_ids[i], sizeof(VSS_ID));
        snap->valid = 1;

        LOG_INFO("vss: snapshot[%d] %s -> %s",
                 snapshot_count, snap->original_volume, snap->snapshot_path);

        VssFreeSnapshotProperties(&snapshot_prop);
        snapshot_count++;
    }

    return snapshot_count;
}

int vss_backup_complete(vss_context_t *ctx) {
    if (!ctx || !ctx->backup_components) return -1;

    /* 持久快照无写入器参与, 跳过 BackupComplete (原始行为) */
    if (ctx->context & VSS_VOLSNAP_ATTR_NO_WRITERS) {
        LOG_DEBUG("vss: skip BackupComplete for persistent snapshot");
        return 0;
    }

    IVssAsync *async_op = NULL;
    HRESULT hr = ctx->backup_components->BackupComplete(&async_op);
    if (FAILED(hr)) {
        LOG_WARN("vss: BackupComplete failed: 0x%lx", (unsigned long)hr);
        return -1;
    }

    if (async_op) {
        async_op->Wait(10000);
        async_op->Release();
    }

    LOG_INFO("vss: backup complete notified");
    return 0;
}

const char *vss_snapshot_device_path(const vss_snapshot_t *snap) {
    return snap->valid ? snap->snapshot_path : NULL;
}

int vss_query_snapshots(vss_snapshot_info_t *info, int max_count) {
    if (!info || max_count <= 0) return -1;

    vss_context_t *ctx = vss_init_ex(0x1d /* VSS_CTX_CLIENT_ACCESSIBLE */);
    if (!ctx) {
        LOG_ERROR("vss_query: init failed");
        return -1;
    }

    IVssEnumObject *pEnum = NULL;
    HRESULT hr = ctx->backup_components->Query(
        GUID_NULL, VSS_OBJECT_NONE, VSS_OBJECT_SNAPSHOT, &pEnum);
    if (FAILED(hr)) {
        LOG_ERROR("vss_query: Query failed: 0x%lx", (unsigned long)hr);
        vss_cleanup(ctx);
        return -1;
    }

    int count = 0;
    while (count < max_count) {
        VSS_OBJECT_PROP objProp;
        ULONG fetched = 0;
        hr = pEnum->Next(1, &objProp, &fetched);
        if (FAILED(hr) || fetched == 0) break;

        VSS_SNAPSHOT_PROP *snap = &objProp.Obj.Snap;

        /* GUID → string */
        char guid_str[64];
        snprintf(guid_str, sizeof(guid_str),
                 "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                 snap->m_SnapshotId.Data1,
                 snap->m_SnapshotId.Data2,
                 snap->m_SnapshotId.Data3,
                 snap->m_SnapshotId.Data4[0],
                 snap->m_SnapshotId.Data4[1],
                 snap->m_SnapshotId.Data4[2],
                 snap->m_SnapshotId.Data4[3],
                 snap->m_SnapshotId.Data4[4],
                 snap->m_SnapshotId.Data4[5],
                 snap->m_SnapshotId.Data4[6],
                 snap->m_SnapshotId.Data4[7]);
        strncpy(info[count].snapshot_id_str, guid_str,
                sizeof(info[count].snapshot_id_str) - 1);

        /* 原始卷名 */
        if (snap->m_pwszOriginalVolumeName) {
            WideCharToMultiByte(CP_ACP, 0,
                                snap->m_pwszOriginalVolumeName, -1,
                                info[count].original_volume,
                                sizeof(info[count].original_volume), NULL, NULL);
        } else {
            info[count].original_volume[0] = '\0';
        }

        /* 快照设备路径 */
        if (snap->m_pwszSnapshotDeviceObject) {
            WideCharToMultiByte(CP_ACP, 0,
                                snap->m_pwszSnapshotDeviceObject, -1,
                                info[count].snapshot_path,
                                sizeof(info[count].snapshot_path), NULL, NULL);
        } else {
            info[count].snapshot_path[0] = '\0';
        }

        /* 创建时间 (VSS_TIMESTAMP = LONGLONG, 100ns since 1601-01-01, same as FILETIME) */
        LONGLONG ts = snap->m_tsCreationTimestamp;
        FILETIME ft;
        ft.dwLowDateTime  = (DWORD)(ts & 0xFFFFFFFF);
        ft.dwHighDateTime = (DWORD)(ts >> 32);
        SYSTEMTIME st;
        if (FileTimeToSystemTime(&ft, &st)) {
            snprintf(info[count].creation_time,
                     sizeof(info[count].creation_time),
                     "%04d-%02d-%02d %02d:%02d:%02d",
                     st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond);
        } else {
            info[count].creation_time[0] = '\0';
        }

        /* 原始 GUID */
        memcpy(&info[count].snapshot_id, &snap->m_SnapshotId, sizeof(VSS_ID));

        count++;
    }

    pEnum->Release();
    vss_cleanup(ctx);
    LOG_INFO("vss_query: found %d snapshots", count);
    return count;
}

int vss_delete_snapshot(const char *snapshot_id_str) {
    if (!snapshot_id_str) return -1;

    vss_context_t *ctx = vss_init_ex(0x1d /* VSS_CTX_CLIENT_ACCESSIBLE */);
    if (!ctx) {
        LOG_ERROR("vss_delete: init failed");
        return -1;
    }

    /* 解析 GUID 字符串 (auto-add braces if missing) */
    char guid_buf[64];
    if (snapshot_id_str[0] == '{') {
        strncpy(guid_buf, snapshot_id_str, sizeof(guid_buf) - 1);
        guid_buf[sizeof(guid_buf) - 1] = '\0';
    } else {
        snprintf(guid_buf, sizeof(guid_buf), "{%s}", snapshot_id_str);
    }

    WCHAR wide_guid[64];
    MultiByteToWideChar(CP_ACP, 0, guid_buf, -1, wide_guid, 64);

    VSS_ID snapshot_id;
    HRESULT hr = CLSIDFromString(wide_guid, (LPCLSID)&snapshot_id);
    if (FAILED(hr)) {
        LOG_ERROR("vss_delete: invalid GUID: %s", snapshot_id_str);
        vss_cleanup(ctx);
        return -1;
    }

    LONG deleted = 0;
    VSS_ID failed_id;
    hr = ctx->backup_components->DeleteSnapshots(
        snapshot_id, VSS_OBJECT_SNAPSHOT, TRUE, &deleted, &failed_id);
    if (FAILED(hr)) {
        LOG_ERROR("vss_delete: DeleteSnapshots failed: 0x%lx", (unsigned long)hr);
        vss_cleanup(ctx);
        return -1;
    }

    vss_cleanup(ctx);
    LOG_INFO("vss_delete: deleted %ld snapshot(s)", deleted);
    return (int)deleted;
}

int vss_delete_all_snapshots(void) {
    vss_context_t *ctx = vss_init_ex(0x1d /* VSS_CTX_CLIENT_ACCESSIBLE */);
    if (!ctx) {
        LOG_ERROR("vss_delete_all: init failed");
        return -1;
    }

    LONG deleted = 0;
    VSS_ID failed_id;
    HRESULT hr = ctx->backup_components->DeleteSnapshots(
        GUID_NULL, VSS_OBJECT_SNAPSHOT, TRUE, &deleted, &failed_id);
    if (FAILED(hr)) {
        LOG_ERROR("vss_delete_all: DeleteSnapshots failed: 0x%lx", (unsigned long)hr);
        vss_cleanup(ctx);
        return -1;
    }

    vss_cleanup(ctx);
    LOG_INFO("vss_delete_all: deleted %ld snapshot(s)", deleted);
    return (int)deleted;
}

#else  /* !_WIN32 — 桩实现 */

vss_context_t *vss_init(void) {
    LOG_WARN("vss: not supported on this platform");
    return NULL;
}

void vss_cleanup(vss_context_t *ctx) {
    (void)ctx;
}

int vss_create_snapshots(vss_context_t *ctx,
                         const char *volumes[],
                         vss_snapshot_t *snapshots) {
    (void)ctx;
    (void)volumes;
    (void)snapshots;
    LOG_ERROR("vss: not supported on this platform");
    return -1;
}

int vss_backup_complete(vss_context_t *ctx) {
    (void)ctx;
    return -1;
}

const char *vss_snapshot_device_path(const vss_snapshot_t *snap) {
    (void)snap;
    return NULL;
}

vss_context_t *vss_init_ex(LONG context) {
    (void)context;
    LOG_WARN("vss: not supported on this platform");
    return NULL;
}

int vss_query_snapshots(vss_snapshot_info_t *info, int max_count) {
    (void)info;
    (void)max_count;
    LOG_WARN("vss: not supported on this platform");
    return -1;
}

int vss_delete_snapshot(const char *snapshot_id_str) {
    (void)snapshot_id_str;
    LOG_WARN("vss: not supported on this platform");
    return -1;
}

int vss_delete_all_snapshots(void) {
    LOG_WARN("vss: not supported on this platform");
    return -1;
}

#endif /* _WIN32 */
