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
    int                  initialized;
    int                  snapshot_set_started;
};

/* 动态加载的函数指针 (保持与原工具一致的延迟加载方式) */
typedef HRESULT (WINAPI *PFN_CreateVssBackupComponents)(
    IVssBackupComponents **ppBackup);

static PFN_CreateVssBackupComponents g_pfn_create = NULL;

vss_context_t *vss_init(void) {
    vss_context_t *ctx = (vss_context_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    /* 加载 vssapi.dll */
    ctx->vssapi_dll = LoadLibraryA("vssapi.dll");
    if (!ctx->vssapi_dll) {
        LOG_ERROR("vss_init: LoadLibrary(vssapi.dll) failed: %lu",
                  GetLastError());
        free(ctx);
        return NULL;
    }

    g_pfn_create = (PFN_CreateVssBackupComponents)
        GetProcAddress(ctx->vssapi_dll, "CreateVssBackupComponents");
    if (!g_pfn_create) {
        LOG_ERROR("vss_init: GetProcAddress(CreateVssBackupComponents) failed");
        FreeLibrary(ctx->vssapi_dll);
        free(ctx);
        return NULL;
    }

    /* 初始化 COM (多线程) */
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        LOG_ERROR("vss_init: CoInitializeEx failed: 0x%lx", (unsigned long)hr);
        FreeLibrary(ctx->vssapi_dll);
        free(ctx);
        return NULL;
    }

    /* 创建 VSS 备份组件 */
    hr = g_pfn_create(&ctx->backup_components);
    if (FAILED(hr)) {
        LOG_ERROR("vss_init: CreateVssBackupComponents failed: 0x%lx",
                  (unsigned long)hr);
        CoUninitialize();
        FreeLibrary(ctx->vssapi_dll);
        free(ctx);
        return NULL;
    }

    /* 初始化备份 */
    hr = ctx->backup_components->InitializeForBackup();
    if (FAILED(hr)) {
        LOG_ERROR("vss_init: InitializeForBackup failed: 0x%lx",
                  (unsigned long)hr);
        ctx->backup_components->Release();
        CoUninitialize();
        FreeLibrary(ctx->vssapi_dll);
        free(ctx);
        return NULL;
    }

    /* 设置为选择组件模式 */
    hr = ctx->backup_components->SetBackupState(
        TRUE,   /* 选择组件 */
        FALSE,  /* 不是 BootableSystemState */
        VSS_BT_COPY,  /* 拷贝备份 (不截断日志) */
        FALSE   /* 不部分文件支持 */
    );
    if (FAILED(hr)) {
        LOG_WARN("vss_init: SetBackupState failed: 0x%lx", (unsigned long)hr);
        /* 非致命, 继续 */
    }

    ctx->initialized = 1;
    LOG_INFO("vss: initialized successfully");
    return ctx;
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

    /* 第 4 步: 收集写入器元数据 */
    hr = bc->GatherWriterMetadata(NULL);
    if (FAILED(hr)) {
        LOG_WARN("vss: GatherWriterMetadata failed: 0x%lx", (unsigned long)hr);
        /* 非致命: 可能无写入器, 继续 */
    }

    /* 第 5-6 步: 启动快照集 */
    VSS_ID snapshot_set_id;
    hr = bc->StartSnapshotSet(&snapshot_set_id);
    if (FAILED(hr)) {
        LOG_ERROR("vss: StartSnapshotSet failed: 0x%lx", (unsigned long)hr);
        return -1;
    }
    ctx->snapshot_set_started = 1;

    /* 第 7 步: 为每个卷添加到快照集 */
    VSS_ID snapshot_ids[VSS_MAX_VOLUMES];
    int added_count = 0;

    for (int i = 0; i < VSS_MAX_VOLUMES && volumes[i] != NULL; i++) {
        /* 将卷路径转为宽字符 */
        WCHAR wide_vol[64];
        MultiByteToWideChar(CP_ACP, 0, volumes[i], -1, wide_vol, 64);

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

    /* 第 8 步: 设置为备份状态 */
    bc->SetBackupState(FALSE, FALSE, VSS_BT_COPY, FALSE);

    /* 第 9 步: 准备备份 (异步, 最长 30 秒) */
    IVssAsync *async_op = NULL;
    hr = bc->PrepareForBackup(&async_op);
    if (FAILED(hr)) {
        LOG_ERROR("vss: PrepareForBackup failed: 0x%lx", (unsigned long)hr);
        return -1;
    }

    if (async_op) {
        async_op->Wait(30000);  /* 30 秒超时 */
        async_op->Release();
    }

    /* 第 10 步: 执行快照 */
    hr = bc->DoSnapshotSet(NULL);
    if (FAILED(hr)) {
        LOG_ERROR("vss: DoSnapshotSet failed: 0x%lx", (unsigned long)hr);
        return -1;
    }

    LOG_INFO("vss: snapshot set created (%d volumes), querying properties...",
             added_count);

    /* 第 11 步: 查询快照属性 */
    for (int i = 0; i < added_count; i++) {
        VSS_SNAPSHOT_PROP snapshot_prop;
        hr = bc->GetSnapshotProperties(snapshot_ids[i], &snapshot_prop);
        if (FAILED(hr)) {
            LOG_WARN("vss: GetSnapshotProperties failed for snapshot %d", i);
            continue;
        }

        /* 保存快照信息 */
        vss_snapshot_t *snap = &snapshots[snapshot_count];
        memset(snap, 0, sizeof(*snap));

        /* 原始卷名 (宽 → 窄) */
        WideCharToMultiByte(CP_ACP, 0,
                            snapshot_prop.m_pwszOriginalVolumeName, -1,
                            snap->original_volume,
                            sizeof(snap->original_volume), NULL, NULL);

        /* 快照设备路径 */
        WideCharToMultiByte(CP_ACP, 0,
                            snapshot_prop.m_pwszSnapshotDeviceObject, -1,
                            snap->snapshot_path,
                            sizeof(snap->snapshot_path), NULL, NULL);

        memcpy(snap->snapshot_id, &snapshot_ids[i], sizeof(VSS_ID));
        snap->valid = 1;

        LOG_INFO("vss: snapshot[%d] %s → %s",
                 snapshot_count, snap->original_volume, snap->snapshot_path);

        /* 释放快照属性 */
        VssFreeSnapshotProperties(&snapshot_prop);

        snapshot_count++;
    }

    return snapshot_count;
}

int vss_backup_complete(vss_context_t *ctx) {
    if (!ctx || !ctx->backup_components) return -1;

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

#endif /* _WIN32 */
