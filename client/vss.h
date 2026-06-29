/*
 * vss.h — VSS (卷影复制服务) 模块 (仅 Windows)
 *
 * 封装 Windows VSS COM 接口, 用于创建源卷的快照。
 * 快照确保迁移过程中磁盘数据的一致性, 避免读取正在写入的数据。
 *
 * VSS 生命周期 (与 decompiled go2tencentcloud 一致):
 *   1. CreateVssBackupComponents   — 创建 VSS 备份组件
 *   2. InitializeForBackup         — 初始化备份
 *   3. SetBackupState              — 设置备份状态 (选择组件)
 *   4. GatherWriterMetadata        — 收集写入器元数据
 *   5. AddComponent                — 添加卷组件
 *   6. StartSnapshotSet            — 启动快照集
 *   7. AddToSnapshotSet            — 添加卷到快照集
 *   8. SetBackupState(BACKUP)      — 设置为备份状态
 *   9. PrepareForBackup            — 准备备份
 *  10. DoSnapshotSet               — 执行快照 (提交)
 *  11. GetSnapshotProperties       — 获取快照属性 (路径)
 *  12. 读取快照数据...
 *  13. BackupComplete              — 标记备份完成 (不删除快照)
 */

#ifndef CLIENT_VSS_H
#define CLIENT_VSS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VSS_MAX_VOLUMES      16      /* 最多快照卷数 */
#define VSS_SNAPSHOT_PATH_MAX 512    /* 快照路径最大长度 */

/* 一个卷的快照信息 */
typedef struct {
    char     original_volume[64];             /* 原始卷路径 (如 C:) */
    char     snapshot_path[VSS_SNAPSHOT_PATH_MAX]; /* 快照设备路径 */
    uint64_t snapshot_id[2];                  /* VSS 快照 ID (GUID 128-bit) */
    int      valid;                           /* 1 = 有效快照 */
} vss_snapshot_t;

/* VSS 上下文 */
typedef struct vss_context vss_context_t;

/*
 * 初始化 VSS 子系统。
 *
 * 加载 vssapi.dll, 获取函数指针。
 *
 * 返回不透明上下文, 失败返回 NULL。
 */
vss_context_t *vss_init(void);

/* 清理 VSS 子系统 (不删除快照) */
void vss_cleanup(vss_context_t *ctx);

/*
 * 为指定卷列表创建快照。
 *
 * ctx:       VSS 上下文
 * volumes:   要快照的卷路径数组 (如 {"C:", "D:"}), NULL 结尾
 * snapshots: [out] 快照信息数组
 *
 * 返回成功创建的快照数, 错误返回 -1。
 *
 * 注意: 此函数是同步的, 在所有卷快照完成后返回。
 *       快照数据通过 snapshot_path 访问。
 */
int vss_create_snapshots(vss_context_t *ctx,
                         const char *volumes[],
                         vss_snapshot_t *snapshots);

/*
 * 标记备份完成 (通知 VSS 写入器备份已正常结束)。
 * 不删除快照 — 快照在程序退出后由系统清理。
 *
 * 应在所有数据读取完成后调用。
 */
int vss_backup_complete(vss_context_t *ctx);

/*
 * 获取快照对应的原始磁盘路径。
 *
 * VSS 快照通过卷 GUID 路径访问, 这里映射到对应的磁盘偏移。
 * 如果需要块级访问, 使用快照的设备路径 (如 \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN)。
 */
const char *vss_snapshot_device_path(const vss_snapshot_t *snap);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_VSS_H */
