# 单页损坏重同步复现与修复（libogg）

验证目标：码流中间任意一页损坏时，解码器按页头（`OggS` capture pattern）
重新同步，**只跳过并计数坏页**，后续页继续解码；恢复后页序与校验均正确，
即“坏一页只丢一页数据”。

## 背景：libogg 的两层容错机制

libogg 本身已经具备恢复能力，关键在解码侧是否正确处理返回值：

1. 同步层 `ogg_sync_pageout()`
   - 内部 `ogg_sync_pageseek()` 对候选整页重算 Ogg 页 CRC（`src/framing.c`），
     CRC 不匹配则判定为坏页/误捕获，从下一个字节起用 `memchr` 搜索下一个
     `'O'`，直到找到下一个 CRC 合法的 `OggS` 页头。
   - 返回值：`1` 得到一页；`0` 数据不足；`-1` 发生一次重同步（丢弃了若干字节）。
   - **`-1` 是“可恢复提示”，不是致命错误**，必须继续循环泵数据。
2. 流层 `ogg_stream_pagein()`
   - 用页头第 18–21 字节的页序号（page sequence）检测缺口，回滚上一页未完成
     的分包，并在分段表插入 `0x400` 丢包标记。
   - 随后 `ogg_stream_packetout()` 对该标记返回 `-1`（hole marker），消费后
     继续，包序号自动对齐。

## 缺陷解码器的典型错误

`decode_naive.c` 把 `ogg_sync_pageout()` 的 `-1` 当致命错误直接退出。
坏页之后整段码流（本例 24 页中的后 16 页）全部丢失。

## 修复要点（见 `decode_robust.c`）

1. 重同步（`ogg_sync_pageseek()` 返回负值，等价于 `ogg_sync_pageout()` 的
   `-1`）：这是可恢复提示，累计搜索跳数与丢弃字节数后继续循环，绝不退出。
   直接用底层 `pageseek` 是为了统计每一次字节级跳转，`pageout` 会把首次之后
   的跳转吞在内部循环里。
2. 用页序号差值统计丢失页数：`lost += pageno - prev_pageno - 1`，
   并记录首个丢失页序号与缺口大小。
3. `packetout == -1`：识别为流层 hole marker，计数并继续。
4. 双重校验恢复正确性：
   - Ogg 页 CRC 已由同步层保证（只有 CRC 合法的页才会被吐出）；
   - 每个负载帧自带 CRC-32 与应用层帧序号（`repro_util.h`），用于独立验证
     恢复后校验和与页序（帧序号严格递增，坏点处允许正好跳过缺失序号）。
5. 支持任意小块喂入（`--chunksize`），页可跨多次读取边界，恢复逻辑不变。

## 文件

- `gen_stream.c`：生成每页一个独立包的干净 Ogg 码流（`clean.ogg`）。
- `corrupt_stream.c`：在**不改变文件长度和后续页偏移**的前提下损坏指定页，
  支持 `body`（翻转页体 16 字节，页头仍为 `OggS`）与 `header`（破坏 capture
  pattern）两种模型。
- `decode_naive.c`：有缺陷的参考解码器（遇 `-1` 中止）。
- `decode_robust.c`：修复后的解码器，输出人类可读或 `--json` 恢复统计。
- `run_repro.sh`：一键构建并跑完全部场景。

## 运行

```sh
cmake -S . -B build && cmake --build build   # 先编出 build/libogg.a
cd repro && ./run_repro.sh
```

## 恢复统计（24 页，损坏第 8 页，body / header 两种模型结果一致）

| 指标 | 值 |
| --- | --- |
| 重同步（坏页）次数 `resync_events` | 1 |
| 页搜索跳数 `seek_hops` | 2（页内若干 `'O'` 造成的中间误捕获，均被 CRC 否决） |
| 重同步丢弃字节 `skipped_bytes` | 148（恰好等于坏页整页：28 字节页头/段表 + 120 字节页体） |
| 通过 Ogg CRC 的页 `pages_crc_ok` | 23 |
| 丢失页数 `pages_lost` | **1** |
| 流层 hole marker `packet_holes` | 1 |
| 恢复的负载帧 `packets_ok` | 23 |
| 负载 CRC 失败 `payload_bad` | 0 |
| 首个丢失页序号 | 8（页序 7 → 9，gap=2） |
| 恢复后页序 | OK（帧序号严格递增） |
| 恢复后校验 | OK（Ogg 页 CRC + 帧 CRC-32 全通过） |

对照：朴素解码器在同一码流上仅解出前 8 页即退出（退出码 3）。

附加场景：
- `--chunksize 33` 小块流式喂入，统计与整文件读入完全一致。
- 同时损坏第 5、17 两页：`resync_events=2`、`seek_hops=2`、
  `skipped_bytes=296`、`pages_lost=2`、`pages_crc_ok=22`、
  `packet_holes=2`，其余指标全部 OK，证明计数可推广到多页。

说明：本次未修改 libogg 库源码（`src/framing.c` 的重同步/CRC/缺口机制经
`-D_V_SELFTEST` 自测全部通过）；修复落在解码应用侧对同步层/流层返回值的
正确处理上。
