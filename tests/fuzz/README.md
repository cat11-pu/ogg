# Ogg 解析器模糊测试套件

针对 libogg 公共解码路径（`ogg_sync_*` → `ogg_sync_pageout` →
`ogg_stream_pagein` → `ogg_stream_packetout`）的黑盒/灰盒健壮性测试，
覆盖畸形码流的零崩溃/零越界/零泄漏目标，以及合法码流与独立参考
实现的结果一致性。

## 目录结构

- `fuzz_parse.c` — C 驱动：分块喂入 sync 层、多 serial 解复用、
  逐页结构诊断（偏移+原因）、逐包输出，全程走真实库代码。
- `ogg_model.py` — 按 RFC 3533 独立编写的 Ogg 页构造器、CRC-32、
  扫描器与分包参考实现（未复制 libogg 代码）。
- `generate.py` — 生成 200 个合法码流（1–3 条逻辑流交错、跨页
  大包、BOS/EOS、前导/尾部垃圾）和 5 类各 400 个畸形码流。
- `fuzz_random.py` — 不同种子的额外随机模糊（纯垃圾、变异页、
  随机截断、页汤），每轮 1000 个缓冲。
- `run_all.py` — 并行运行全部 2200 个语料，解析驱动输出，与参考
  实现逐项比对，产出 `out/run_results.jsonl` 与 `out/summary.json`。
- `make_minimal.py` — 为 5 类故障各生成一个手工最小反例到
  `minimal/`。
- `build.sh` — 用 GCC 的 ASan+UBSan（含 LSan）编译库源码与驱动，
  另产出一个 `-DDISABLE_CRC` 版本用于覆盖 CRC 闸门之后的解析路径。

## 复现步骤

```sh
# 1. 基线构建（生成 config_types.h，并跑库自带自测）
cmake -S . -B build-baseline -DBUILD_TESTING=ON
cmake --build build-baseline -j
./build-baseline/test_framing && ./build-baseline/test_bitwise

# 2. 构建带 sanitizer 的驱动
cd tests/fuzz && ./build.sh

# 3. 生成语料（200 合法 + 2000 畸形）
python3 generate.py

# 4. 全量运行与参考比对
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
python3 run_all.py

# 5. 额外随机模糊（3 个种子，共 3000 缓冲）
python3 fuzz_random.py 1000 0x5EED5EED
python3 fuzz_random.py 1000 0x0BADC0DE
python3 fuzz_random.py 1000 0x1234ABCD

# 6. 最小反例
python3 make_minimal.py
for f in minimal/*; do ./fuzz_parse "$f"; done
```

## 故障原因词汇表

每条失败诊断都带字节偏移 `offset` 与下列 `reason`：

- `truncated_header` — 捕获字不完整或 27 字节页头/段表被截断。
- `page_length_mismatch` — 段表声明的页长超出实际缓冲字节数。
- `checksum_mismatch` — RFC 3533 CRC-32 重算与存储值不符。
- `illegal_version` — 页头版本字节非 0（RFC 仅定义 0）。
- `illegal_flags` — 标志位含保留位（附带在诊断中）。
- `cyclic_page_chain` — 同一 serial 的页号重复或非单调（含回绕后重复）。
