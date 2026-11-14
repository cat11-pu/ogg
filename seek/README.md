# Ogg 按时间戳跳页

在不改变 `ogg_sync_*` 流式接口的前提下，为 libogg 增加 page 级时间索引：
把页的绝对字节偏移和 granulepos 记录下来，查询目标 granulepos 后直接
从对应 `OggS` 边界继续 `ogg_sync_pageout()`。

Ogg 的“时间戳”就是页头里的 granulepos。libogg 不知道编码器的时间基
（采样率、帧率等），所以接口直接使用 granulepos，简体转换由应用层完成。

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

/* 顺序读页时逐页建立索引；offset 必须是该页 OggS 的绝对偏移。 */
int  ogg_seek_index_add_page(ogg_seek_index *idx,
                             ogg_int64_t offset, const ogg_page *og);

/* 已解析字段的底层入口。 */
int  ogg_seek_index_add(ogg_seek_index *idx, ogg_int64_t offset,
                        ogg_int64_t granulepos,
                        long serialno, long pageno);

/* 便利接口：扫描完整的内存码流，自动跳过前导垃圾和截断尾页。 */
int  ogg_seek_index_build(ogg_seek_index *idx,
                          const void *data, size_t bytes);

/* 成功返回 1；serial 不存在或空索引返回 0；参数非法返回 -1。 */
int  ogg_seek_index_find(const ogg_seek_index *idx, long serialno,
                         ogg_int64_t target_granulepos,
                         ogg_seekpoint *point);
```

典型用法：

```c
ogg_seekpoint sp;
if(ogg_seek_index_find(&idx, serialno, target, &sp) == 1){
  fseeko(fp, (off_t)sp.offset, SEEK_SET);  /* libogg 不做 I/O */
  ogg_sync_reset(&oy);
  /* 从新位置喂数据，继续 ogg_sync_pageout() */
}
```

## 选择规则与边界

索引按 serial 查询，避免多路复用流各自的时间基互相污染。granulepos=-1
的页仍会记录偏移，但不参与时间比较。

| 情况 | 返回页 |
| --- | --- |
| 目标恰好等于某页 granulepos | 该时间戳的**第一页**，不跳过同时间戳页 |
| 目标落在两页之间 | 最后一个 granulepos ≤ 目标的页（最多早一个 timed page） |
| 目标早于该流第一个 timed page | 该流第一个物理页，保留 BOS/头信息 |
| 目标等于或晚于最后一个 timed page | 最后一个 timed page |
| 流内没有任何 timed page | 第一个物理页 |
| serial 不存在 / 空索引 | 返回 0，`point` 不变 |
| 参数为空、负偏移、偏移非递增 | 返回 -1 |

精确命中时取“第一页”，而落在区间时取“最后一个不晚于目标的页”，二者
共同保证不会跳过目标时间戳上的数据；区间查询最多早一个 timed page。
跨页大包的中间页 granulepos 为 -1，它们是同一时间区间的 continuation
页，因此“一页”按 timed page 计，页数据仍逐字节一致。

## 一致性保证

索引记录的是通过 CRC 校验后页的 `OggS` 边界，查询不重新猜测页边界。
因此从返回偏移继续同步，得到的物理页与顺序读取到该页时完全相同，包括：

- 页头、segment table、页体和 CRC；
- continuation/BOS/EOS 标志；
- serialno、pageno、granulepos。

`src/seek_test.c` 生成 10 页码流，其中包含前导垃圾、尾部和 200000 字节
跨页大包（中间页 granulepos=-1），对 14 个目标时间逐一：

1. 用 97 字节小块顺序扫描，得到基线页序列；
2. 建索引并按目标跳转；
3. 从跳转偏移以 113 字节小块重新同步；
4. 对后续所有页做长度、字段和 `memcmp` 全字节比较。

## 一致性数据

运行：

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
./build/test_seek > seek/consistency.json
python3 -m json.tool seek/consistency.json
```

本次结果（`seek/consistency.json`）：

| 指标 | 值 |
| --- | --- |
| 码流页数 / 索引条目 | 10 / 10 |
| 跳转目标数 | 14 |
| 后续页逐字节比较总量 | 2,426,712 bytes |
| 页内容不一致 | 0 |
| 最大 timed page 提前量 | 1 |

代表性目标：

| target | 选中页 | 选中 granulepos | timed 提前量 | 页完全一致 |
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

同一测试还覆盖：serial 过滤（两路交错流分别跳转）、同 granulepos
重复页、空索引、未知 serial、负偏移、非递增偏移、无 timed page 回退，
以及截断尾页不影响已完整页的索引。

## 复杂度与限制

- 内存：每页一个 `ogg_seekpoint`。
- 建索引：顺序读一遍，O(n)；`build` 为便利接口，大文件建议边读边
  `add_page`。
- 查询：O(n) 扫描指定 serial；索引不要求多路复用页在时间上全局有序。
- 只做页级定位，不负责解码器状态回滚；若从跨页包中间开始，应用仍需
  按 codec 规则丢弃不完整包。
