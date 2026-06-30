# go2tencentcloud 最终切换 (Cutover) 机制

## 1. 核心结论

go2tencentcloud 的最终切换 **不依赖源端关机**。官方文档明确说明"源端服务器无需停机"（在线热迁移）。数据一致性由 **VSS 快照 + 增量 hash 对比 + ctlEndIncremental/SERVER_DONE 协议** 保证，源端的写入与读取并发进行，变化的块通过 18 秒增量定时器补传。

源端关机不是数据同步机制的一部分——它是迁移完成后的可选手动操作（防止分裂脑）。

## 2. 完整事件链路

### 2.1 第一环 — VSS 快照 + 全量同步

```
Go 控制端                              C 客户端 (client.exe)
──────────                             ──────────
1. client.exe test_vss
   → 创建持久 VSS 快照

2. CreateProcess("client.exe <ip:port>")
                                       3. 从 VSS 快照逐块读取
                                       4. hash → Zstd → MsgPack → TCP send
                                       5. 收到 ACK → UPDATE T_BLOCK SET ack=1, hash=?
                                       6. sync_read_block_to_async_send2 完成
                                       7. timer_cb_real():
                                          a. 等所有块 ACK
                                          b. ctlEndIncremental
                                          c. 等 SERVER_DONE
                                          d. allDone = 1
                                       8. 18s 增量定时器: allDone=1
                                          → DELETE FROM T_BLOCK
                                          → exit(0)
```

### 2.2 第二环 — 增量定时器（18 秒）从 Live Disk 重读 + Hash 对比

来源：Ghidra 逆向 `sync_read_block_to_async_send2_1400061d0.c` (0x1400061d0)

```c
// 每 18 秒触发
void incremental_timer_cb(void) {
    for each disk {
        // 获取未 ACK 块: SELECT devno, offset, size FROM T_BLOCK WHERE ACK=0
        get_unacked_blocks(&block_list, disk, remote_id);

        // Fisher-Yates 随机打乱
        shuffle(block_list);

        for each block in block_list {
            // 背压: 队列深度 > 9 或内存 > 45MB → 暂停
            if (queue_depth > 9 || mem_high) goto retry;

            // 121 秒冷却: 同一块不频繁重读
            if (now - last_sent_time < 121s) continue;

            // ★ 从 live disk (PhysicalDrive) 重新读取块
            block_data = read_from_disk(disk, offset, size);

            // ★ 计算 hash → 对比 T_BLOCK 中的旧 hash
            new_hash = compute_hash(block_data);
            old_hash = SELECT HASH FROM T_BLOCK WHERE DEVNO=? AND OFFSET=?;

            if (new_hash == old_hash) {
                skip;  // 块未变化
            } else {
                // 重新压缩发送
                MsgPack_encode → Zstd_compress → TCP_send;
            }
        }
    }

    // 检查 allDone 标志
    if (allDone) {
        DELETE FROM T_BLOCK WHERE REMOTE_ID!='%s';
        exit(0);
    }
}
```

**关键点**：
- 读的是 **live disk（物理磁盘）**，不是 VSS 快照。因为快照数据是静态的，只有读 live disk 才能检测变化。
- 写入与读取**并发进行**，没有 FSCTL_LOCK_VOLUME 或 FSCTL_DISMOUNT_VOLUME
- 变化的块在下一轮 18 秒定时器中补传
- 121 秒冷却期防止同一块频繁重读
- 背压机制（队列 < 10）防止服务端过载

### 2.3 第三环 — Go 控制端监控 + 完成

来源：Go 二进制函数符号表

```
Go 控制端操作:
1. main.winExecClient       → CreateProcess("client.exe")
2. main.runClientWithExitCode → WaitForSingleObject(client_process)
3. client.exe 退出码 = 0   → 迁移成功
4. client.exe end_session   → 清理 T_BLOCK
5. client.exe vss_delete    → 清理 VSS 快照
6. 报告迁移成功
```

**源端不需要关机**。Go 控制端检测到 client.exe 正常退出后，迁移即完成。源端由用户自行决定保留（回滚）或关机。

## 3. 数据一致性：为什么不关机也能一致

```
时间 →
              VSS 快照点
                  │
  Live Disk:  [W1][W2]...[Wn][Wn+1][Wn+2]...
                  │        │
  全量同步:   ├─ 读 VSS ──┤
  (快照数据)   │  snapshot  │
              │            │
  增量同步:               ├─ 读 live disk ──┤
  (live disk)              │  hash 对比       │
                           │  变化 → 重发     │
                           │  不变 → 跳过     │
                           │           ┌──────┤
                           │           │ 最后一轮:
                           │           │ 全部 hash 匹配
                           │           │ → 无未 ACK 块
                           │           │ → ctlEndIncremental
                           │           │ → SERVER_DONE
                           │           │ → exit(0)
```

### 3.1 cras-consistent（崩溃一致性）

源端在迁移过程中持续写入。最终数据状态等效于**源端突然断电**时磁盘的状态：
- 写入完成的数据块：在最后一轮增量中被捕获
- 正在写入的数据块：VSS 快照中可能不完整，目标端 OS 启动时通过 NTFS journal replay 修复
- 未刷新的缓存：VSS Writer Freeze/Thaw 在 60 秒内刷新应用缓存

**目标端首次启动 = Windows 断电恢复**。NTFS 日志重放 + CHKDSK 确保文件系统一致性。

### 3.2 三层一致性保证

| 层次 | 机制 | 负责组件 | 代码位置 |
|------|------|----------|----------|
| 应用一致性 | VSS Writer Freeze/Thaw (< 60s) | C 客户端 | `initialize_vss.c` |
| 块级变化检测 | live disk hash 对比 T_BLOCK | C 客户端 18s 定时器 | 0x1400061d0 |
| 服务端持久化 | ctlEndIncremental → SERVER_DONE | C 客户端 + 服务端 | `read_callback.c` |
| OS 级恢复 | NTFS journal replay + CHKDSK | 目标端 Windows | 首次启动自动执行 |

## 4. 回答：Go 控制端调用关机后自己也被杀了，谁执行最后增量同步？

**这个问题的前提是错的。** 增量同步在关机**之前**完成，关机（如果有）发生在增量同步成功之后。正确时序：

```
18s timer: [... 增量轮1 ...] [... 增量轮2 ...] [... 最后一轮: 无变化块 ...]
                                                                     │
timer_cb_real:          等 ACK → ctlEndIncremental → SERVER_DONE → allDone=1
                                                                     │
18s timer:                                       检测 allDone → exit(0)
                                                                     │
Go 控制端:                    WaitForSingleObject → 退出码0 → 报告成功
                                                                     │
【此时迁移已完成。源端持续运行】                                           │
                                                                     │
【可选】用户决定关机 / Go 调用 ExitWindowsEx → 源端断电（防止分裂脑）
```

**切换 (cutover) 的本质不是"关机→同步"，而是"同步→确认→可选关机"。**

## 5. 官方文档印证

- 腾讯云官方文档明确："源端服务器无需停机"（热迁移）
- "支持断点续传"——暂停后可从中断点继续
- 增量同步是可选功能，持续执行直到用户手动停止
- 迁移完成标志是目标 CVM 能正常启动运行（非源端关机）

## 6. 关键代码引用

| 组件 | 位置 | 功能 |
|------|------|------|
| C 客户端 — 全量同步 | Ghidra 0x140005210 | 从 VSS 或 PhysicalDrive 读块，规划并发送 |
| C 客户端 — 增量定时器 | Ghidra 0x1400061d0 | 每 18s 从 live disk 重读，hash 对比，补传 |
| C 客户端 — timer_cb_real | Ghidra 0x140006c60 | 等 ACK → ctlEndIncremental → 等 SERVER_DONE → allDone |
| C 客户端 — read_callback | Ghidra 0x140003aa0 | 处理 RESPONSE_BINLOG / RESPONSE_SERVER_DONE |
| Go 控制端 — winExecClient | main.winExecClient | CreateProcess 启动 client.exe |
| Go 控制端 — runClientWithExitCode | main.runClientWithExitCode | WaitForSingleObject 等 client 退出 |
| Go 控制端 — ClientCheckPromptMaybeExit | main.ClientCheckPromptMaybeExit | 交互模式：检查是否需要退出 |
