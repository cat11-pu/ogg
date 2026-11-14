# Ogg 解析器健壮性测试报告

- 被测对象：libogg（仓库 `src/framing.c`、`src/bitwise.c`，
  commit `06a5e02`）的公共解码 API。
- 测试日期：2026-11-14，Ubuntu 26.04，GCC 15.2.0。
- 检测工具：AddressSanitizer（越界/释放后使用）、UndefinedBehaviorSanitizer
  （未定义行为，`-fno-sanitize-recover=all`）、LeakSanitizer（内存泄漏）。
- 语料规模：2000 个畸形码流（5 类 × 400）+ 200 个合法码流，
  外加 3000 个随机模糊缓冲、禁用 CRC 的二次构建全量复跑。

## 结论

| 指标 | 目标 | 实测 |
| --- | --- | --- |
| 崩溃（ASan/UBSan 非零退出） | 0 | **0** |
| 堆/栈越界读写 | 0 | **0** |
| 内存泄漏（LSan） | 0 | **0** |
| 未定义行为 | 0 | **0** |
| 畸形码流失败带偏移+原因 | 2000/2000 | **2000/2000** |
| 合法码流与参考实现一致 | 200/200 | **200/200** |
| 随机模糊（3 个种子） | 全部干净 | **3000/3000** |

libogg 在本测试覆盖的输入空间内未发现可触发的内存安全缺陷。
测试过程中发现并修复的两处越界均位于**测试驱动自身**（见下节），
不属于库代码；这两处被保留为驱动修复记录，用于佐证 sanitizer 的
有效性。

## 方法

### 驱动

`fuzz_parse.c` 把输入按 2–6 KiB 分块（约 1/17 的块为 1–8 字节边界
探测）循环调用 `ogg_sync_buffer`/`ogg_sync_wrote`，再通过
`ogg_sync_pageout` 取页、按 serial 多路复用到多个
`ogg_stream_state`，最后 `ogg_stream_packetout` 排空数据包，
覆盖多逻辑流交错、跨页续包、页号跳变等路径。所有资源在退出前
经 `ogg_stream_clear`/`ogg_sync_clear`/`free` 释放，由 LSan 验证。

驱动在库处理前先做一遍独立的逐捕获点结构诊断，对每个 `OggS`
候选输出一行 `DIAG offset=… reason=… detail='…'`；sync 层重同步、
stream 层拒绝等库行为另以 `SYNC`/`STREAM` 行记录，确保每个失败
都可定位到字节偏移与具体原因。

### 参考实现

`ogg_model.py` 仅依据 RFC 3533 实现：CRC-32（多项式 `0x04C11DB7`，
初值/异或出 0）、页扫描、段表分包与续包/丢页恢复规则。合法码流
的页偏移、页号、body 长度、包字节数、BOS/EOS 标志在驱动与参考
实现之间逐字段比对。

### 畸形码流五族（每族 400 个，确定性种子 `0x0663602`）

| 族 | 变异手法 | 诊断原因 |
| --- | --- | --- |
| 截断 | 在捕获字、27 字节页头、段表、页尾任意位置切断 | `truncated_header` / `page_length_mismatch` |
| 页长错位 | 膨胀段表项/段数后重算 CRC，或在声明 body 内截断 | `page_length_mismatch` |
| 校验和错误 | 翻转页头/body/CRC 字段，强制写入计算 CRC 的补码 | `checksum_mismatch` |
| 版本号非法 | 版本字节置 1/2/0x7F/0xFF 并重算 CRC | `illegal_version`（库 `ogg_stream_pagein` 返回 -1） |
| 循环页链 | 重复页号、回退跳转、u32 回绕后重复、serial 复用、自环 | `cyclic_page_chain` |

### 加固轮次

- `-DDISABLE_CRC` 构建：绕过 CRC 闸门，2000 个畸形码流全部干净，
  覆盖正常构建中不可达的页长度/页链后续路径。
- 随机模糊：3 个种子各 1000 个缓冲（纯随机垃圾、变异合法页、
  随机拼接与截断），ASan/UBSan/LSan 下零失败。
- 检测器有效性探针：独立构造的 123 字节泄漏样例被 LSan 捕获、
  1 字节堆越界样例被 ASan 捕获，证明“零泄漏/零越界”结论可信。
- 库自带 `test_framing`、`test_bitwise` 自测全部通过。

## 测试驱动自身缺陷的发现与修复（非库代码）

1. **诊断器堆越界读（10 个用例触发）**：段表计数被恶意放大后，
   驱动仍按声明项数读取段表，越过实际缓冲。修复为先验证
   `27 + nseg` 是否完整，再读段表项。
2. **页偏移计算失效**：初版用 `og.header - oy.data` 计算页偏移，
   但 `ogg_sync_buffer` 扩容会 `realloc` 移动基址，导致输出偏移
   错误（在 166 个合法流的参考比对中暴露）。修复为基于
   `ogg_sync_state` 的 `fill/returned` 记账推导绝对偏移。

## 最小反例（`minimal/`，由 `make_minimal.py` 生成）

| 文件 | 字节数 | 偏移 | 原因 |
| --- | --- | --- | --- |
| `01_truncation.ogg` | 10 | 0 | `truncated_header`（完整捕获字后页头被切断） |
| `02_page_length_mismatch.ogg` | 28 | 0 | `page_length_mismatch`（段表声明 200 字节 body，实际 0） |
| `03_bad_checksum.ogg` | 29 | 0 | `checksum_mismatch`（单字节 CRC 字段翻转） |
| `04_illegal_version.ogg` | 29 | 0 | `illegal_version`（版本=1，CRC 已重算为有效） |
| `05_cyclic_page_chain.ogg` | 58 | 29 | `cyclic_page_chain`（两页页号均为 0，第二页重复） |

每个反例均可用 `./fuzz_parse minimal/<file>` 复现，输出含
`DIAG offset=… reason=…` 且进程以 `DONE status=clean` 退出。

## 产物

- `out/summary.json` — 聚合统计与崩溃/不一致清单（本次均为空）。
- `out/run_results.jsonl` — 2200 个用例的逐条结果（页/包计数、
  原因列表、参考比对结论）。
- `out/final_run.log` — 最终一次全量运行日志。
- `cases/manifest.tsv` — 全部语料的文件名、族标签、字节数。
