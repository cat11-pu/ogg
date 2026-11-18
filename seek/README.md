# Ogg 按时间戳定位

`ogg_seek_index_*` 在不改变 `ogg_sync_*` 流式接口的前提下，为 libogg
增加 page 级时间索引：记录每个物理页的绝对字节偏移和 granulepos，
查询目标 granulepos 后返回最近的页首边界，应用从该偏移继续
`ogg_sync_pageout()` 即可。

Ogg 的“时间”来自页头 granulepos，具体时间基（采样率、帧率等）由 codec
决定，因此接口直接使用 granulepos，不做秒/毫秒换算。

## 接口

```c
#include <ogg/ogg.h>

typedef struct {
  ogg_int64_t offset;      /* 页起始 OggS 的绝对字节偏移 */
  ogg_int64_t granulepos;  /* 页 granulepos，-1 表示本页无包结束 */
  long        serialno;
  long        pageno;
} ogg_seekpoint;

typedef struct {
  ogg_seekpoint *points;
  size_t         count;
  size_t         capacity;
} ogg_seek_index;

int  ogg_seek_index_init(ogg_seek_index *idx);
void ogg_seek_index_clear(ogg_seek_index *idx);

int  ogg_seek_index_add_page(ogg_seek_index *idx,
                             ogg_int64_t offset, const ogg_page *og);
int  ogg_seek_index_add(ogg_seek_index *idx, ogg_int64_t offset,
                        ogg_int64_t granulepos,
                        long serialno, long pageno);
int  ogg_seek_index_build(ogg_seek_index *idx,
                          const void *data, size_t bytes);

int  ogg_seek_index_find(const ogg_seek_index *idx, long serialno,
                         ogg_int64_t target_granulepos,
                         ogg_seekpoint *point);
```

`find` 返回：

- `1`：找到，`*point` 是目标页边界。
- `0`：索引为空或 `serialno` 不存在，`*point` 不变。
- `-1`：参数非法。

典型用法：

```c
ogg_seekpoint sp;

if(ogg_seek_index_find(&idx, serialno, target_granulepos, &sp) == 1){
  fseeko(fp, (off_t)sp.offset, SEEK_SET); /* libogg 不做 I/O */
  ogg_sync_reset(&oy);
  /* 从新位置喂数据，继续 ogg_sync_pageout() */
}
```

## 建索引

- 顺序读取时，对每个通过 `ogg_sync_pageout()` 得到的页调用
  `ogg_seek_index_add_page()`；`offset` 必须是该页 `OggS` 的绝对偏移。
- `ogg_seek_index_build()` 扫描完整内存码流，自动跳过前导垃圾和截断尾页。
- 偏移必须严格递增；重复或负偏移返回 `-1`。
- `build` 成功时原子替换旧索引；失败时旧索引保持不变。
- 索引按 `serialno` 查询，因此同一个索引可以同时容纳复用交错流和链式流。

## 选择规则

| 情况 | 返回页 |
| --- | --- |
| 目标恰好等于某页 granulepos | 该时间戳的第一页，不跳过同时间戳页 |
| 目标落在两页之间 | 最后一个 granulepos ≤ 目标的页 |
| 目标早于该流第一个 timed page | 该流第一个物理页，保留 BOS/头信息 |
| 目标等于或晚于最后一个 timed page | 最后一个 timed page |
| 流内没有任何 timed page | 该流第一个物理页 |
| `granulepos=-1` 的页 | 建索引，但不参与时间比较 |
| serial 不存在 / 空索引 | 返回 `0`，`point` 不变 |
| 参数为空、负偏移、偏移非递增 | 返回 `-1` |

因此区间查询最多早一个 timed page，不会跳过目标时间戳上的数据。
跨页大包的中间页 `granulepos=-1`，它们是同一时间区间的 continuation
页；页边界仍逐字节可恢复。

### 链式流

链式 Ogg 由多个逻辑流依次拼接，规范要求全物理流内 `serialno` 唯一。
把整条链的页都加入同一个索引后，用目标链路的 `serialno` 查询即可。
不同链路可以从相同的 granulepos 重新开始，索引不会把它们混在一起。

```c
/* link A: serial A, granulepos 0,1000,... */
/* link B: serial B, granulepos 0,1000,... */
ogg_seek_index_find(&idx, serial_b, 1500, &sp); /* 只命中 link B */
```

## 一致性保证

索引记录的是通过 CRC 校验后页的 `OggS` 边界，查询不重新猜测页边界。
从返回偏移重新同步，得到的物理页与顺序读取到该页时完全相同，包括：

- 页头、segment table、页体和 CRC；
- continuation/BOS/EOS 标志；
- serialno、pageno、granulepos。

`src/seek_test.c` 的验证流程：

1. 用小块顺序扫描得到基线页序列；
2. 建索引并按目标跳转；
3. 从返回偏移重新同步；
4. 对后续所有页做长度、字段和 `memcmp` 全字节比较。

覆盖用例包含前导垃圾、200000 字节跨页大包、截断尾页、重复时间戳、
目标越界、无 timed page、多路复用交错流和两段链式流。

## 一致性数据

运行：

```sh
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
cmake --build build -j
./build/test_seek > seek/consistency.json
ctest --test-dir build --output-on-failure
```

本次结果（`seek/consistency.json`）：

| 指标 | 值 |
| --- | ---: |
| 单流页数 / 索引条目 | 10 / 10 |
| 单流查询目标 | 14 |
| 链式流链路 / 页数 / 索引条目 | 2 / 12 / 12 |
| 链式流查询目标 | 16 |
| 后续页逐字节比较总量 | 2,441,912 bytes |
| 页内容不一致 | 0 |
| 最大 timed page 提前量 | 1 |
| 连续跳转 | 1,000 |
| 连续跳转成功 | 1,000 |
| 跳转期间索引条目 / 容量变化 | 0 |
| 1000 次跳转后逐页比较总量 | 3,370,791 bytes |
| 边界断言 | 52 |
| 失败断言 | 0 |

代表性目标：

| target | 页 | granulepos | timed 提前量 | 字节一致 |
| ---: | ---: | ---: | ---: | :---: |
| -10000 | 0 | 0 | 0 | yes |
| 0 | 0 | 0 | 0 | yes |
| 999 | 0 | 0 | 1 | yes |
| 1000 | 1 | 1000 | 0 | yes |
| 1500 | 1 | 1000 | 1 | yes |
| 3000 | 3 | 3000 | 0 | yes |
| 5500 | 5 | 5000 | 1 | yes |
| 6000 | 9 | 6000 | 0 | yes |
| 7000 | 9 | 6000 | 0 | yes |

链式流中 `serial A` 和 `serial B` 都查询 1500 时，分别返回各自链路的
granulepos=1000 页；从 A 的返回偏移继续读，会无缝读到 A 剩余页和整个 B。

## 1000 次跳转与泄漏检查

测试在 32 页的两段链式索引上交替查询两个 serial、1000 个不同目标。
每次查询后都从返回偏移重新同步，并把后续所有页与基线逐字节比较
（共 3,370,791 bytes）。`find` 本身不分配内存，循环前后
`points/count/capacity` 完全相同。

ASan/LSan：

```sh
cmake -S . -B build-asan -G Ninja -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address -fno-omit-frame-pointer -g' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address'
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./build-asan/test_seek
```

结果：退出码 `0`，stderr 为空，LeakSanitizer 无泄漏报告。

## 复杂度与限制

- 内存：每页一个 `ogg_seekpoint`。
- 建索引：顺序读一遍，O(n)；大文件建议边读边 `add_page`。
- 查询：O(n) 扫描指定 serial，不分配内存，不修改索引。
- 只做页级定位；跨页包或 codec 的 pre-roll 策略仍由应用层处理。
