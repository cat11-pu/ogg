/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE Ogg CONTAINER SOURCE CODE.              *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE Ogg CONTAINER SOURCE CODE IS COPYRIGHT (C) 1994-2026         *
 * by the Xiph.Org Foundation http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: timestamp-seek boundary and page-consistency test

 ********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogg/ogg.h>

typedef struct {
  unsigned char *data;
  size_t bytes;
  ogg_int64_t granulepos;
  long serialno;
  long pageno;
  int bos;
  int eos;
} test_page;

typedef struct {
  test_page *pages;
  size_t count;
  size_t capacity;
} page_list;

static int failures=0;
static int checks=0;
static size_t page_mismatches=0;

#define CHECK(cond,msg) do{ \
  checks++; \
  if(!(cond)){ \
    fprintf(stderr,"FAIL: %s (%s:%d)\n",msg,__FILE__,__LINE__); \
    failures++; \
  } \
}while(0)

static void list_clear(page_list *list){
  size_t i;
  for(i=0;i<list->count;i++)_ogg_free(list->pages[i].data);
  _ogg_free(list->pages);
  memset(list,0,sizeof(*list));
}

static int list_push(page_list *list,const ogg_page *og){
  test_page *page;
  size_t bytes;

  if(!og || !og->header || og->header_len<0 || og->body_len<0) return -1;
  if(list->count>=list->capacity){
    size_t capacity=list->capacity?list->capacity*2:16;
    test_page *pages;
    if(capacity<list->capacity) return -1;
    pages=_ogg_realloc(list->pages,capacity*sizeof(*pages));
    if(!pages) return -1;
    list->pages=pages;
    list->capacity=capacity;
  }

  bytes=(size_t)og->header_len+(size_t)og->body_len;
  page=&list->pages[list->count];
  page->data=_ogg_malloc(bytes?bytes:1);
  if(!page->data) return -1;
  if(og->header_len)
    memcpy(page->data,og->header,(size_t)og->header_len);
  if(og->body_len)
    memcpy(page->data+og->header_len,og->body,(size_t)og->body_len);
  page->bytes=bytes;
  page->granulepos=ogg_page_granulepos(og);
  page->serialno=ogg_page_serialno(og);
  page->pageno=ogg_page_pageno(og);
  page->bos=ogg_page_bos(og);
  page->eos=ogg_page_eos(og);
  list->count++;
  return 0;
}

static void fill_payload(unsigned char *data,long bytes,long seed){
  long i;
  for(i=0;i<bytes;i++)data[i]=(unsigned char)((seed+i*37)&0xff);
}

static int flush_pages(ogg_stream_state *os,page_list *list){
  ogg_page og;
  while(ogg_stream_flush(os,&og)){
    if(list_push(list,&og))return -1;
  }
  return 0;
}

static int generate_link(long serialno,
                         const ogg_int64_t *granulepos,
                         int packet_count,
                         int add_spanning_packet,
                         page_list *list){
  ogg_stream_state os;
  ogg_packet op;
  unsigned char payload[512];
  unsigned char *big=NULL;
  int i;

  if(ogg_stream_init(&os,serialno))return -1;

  for(i=0;i<packet_count;i++){
    long bytes=64+i*13;
    fill_payload(payload,bytes,serialno+i);
    memset(&op,0,sizeof(op));
    op.packet=payload;
    op.bytes=bytes;
    op.b_o_s=(i==0);
    op.e_o_s=(!add_spanning_packet && i==packet_count-1);
    op.granulepos=granulepos[i];
    op.packetno=i;
    if(ogg_stream_packetin(&os,&op)||flush_pages(&os,list)){
      ogg_stream_clear(&os);
      return -1;
    }
  }

  if(add_spanning_packet){
    long bytes=200000;
    big=_ogg_malloc((size_t)bytes);
    if(!big){
      ogg_stream_clear(&os);
      return -1;
    }
    fill_payload(big,bytes,serialno+999);
    memset(&op,0,sizeof(op));
    op.packet=big;
    op.bytes=bytes;
    op.b_o_s=0;
    op.e_o_s=1;
    op.granulepos=granulepos[packet_count-1]+1000;
    op.packetno=packet_count;
    if(ogg_stream_packetin(&os,&op)||flush_pages(&os,list)){
      _ogg_free(big);
      ogg_stream_clear(&os);
      return -1;
    }
    _ogg_free(big);
  }

  ogg_stream_clear(&os);
  return 0;
}

static int append_bytes(unsigned char **data,size_t *bytes,
                        const void *src,size_t add){
  unsigned char *p;
  if(!add)return 0;
  p=_ogg_realloc(*data,*bytes+add);
  if(!p)return -1;
  memcpy(p+*bytes,src,add);
  *data=p;
  *bytes+=add;
  return 0;
}

static int append_pages(unsigned char **data,size_t *bytes,
                        const page_list *list){
  size_t i;
  for(i=0;i<list->count;i++){
    if(append_bytes(data,bytes,list->pages[i].data,list->pages[i].bytes))
      return -1;
  }
  return 0;
}

static int sequential_scan(const unsigned char *data,size_t bytes,
                           int chunk_size,page_list *list){
  ogg_sync_state oy;
  size_t fed=0;
  int result=0;

  if(ogg_sync_init(&oy))return -1;

  for(;;){
    ogg_page og;
    int ret=ogg_sync_pageout(&oy,&og);
    if(ret>0){
      if(list_push(list,&og)){
        result=-1;
        break;
      }
    }else if(ret<0){
      continue;
    }else{
      if(fed>=bytes)break;
      {
        size_t chunk=bytes-fed;
        char *buffer;
        if(chunk>(size_t)chunk_size)chunk=(size_t)chunk_size;
        buffer=ogg_sync_buffer(&oy,(long)chunk);
        if(!buffer){
          result=-1;
          break;
        }
        memcpy(buffer,data+fed,chunk);
        if(ogg_sync_wrote(&oy,(long)chunk)){
          result=-1;
          break;
        }
        fed+=chunk;
      }
    }
  }

  ogg_sync_clear(&oy);
  return result;
}

static int compare_page(const test_page *a,const test_page *b){
  return a->bytes==b->bytes &&
         a->granulepos==b->granulepos &&
         a->serialno==b->serialno &&
         a->pageno==b->pageno &&
         a->bos==b->bos &&
         a->eos==b->eos &&
         !memcmp(a->data,b->data,a->bytes);
}

static size_t page_offset(const page_list *list,size_t index){
  size_t i,offset=0;
  for(i=0;i<index;i++)offset+=list->pages[i].bytes;
  return offset;
}

static int page_at_offset(const page_list *list,size_t base,
                          ogg_int64_t offset,size_t *index){
  size_t i,at=0;
  for(i=0;i<list->count;i++){
    if((ogg_int64_t)(base+at)==offset){
      *index=i;
      return 1;
    }
    at+=list->pages[i].bytes;
  }
  return 0;
}

static int timed_at_or_after(const page_list *list,ogg_int64_t target,
                             size_t *index){
  size_t i;
  for(i=0;i<list->count;i++){
    if(list->pages[i].granulepos>=target){
      *index=i;
      return 1;
    }
  }
  return 0;
}

static int last_timed_page(const page_list *list,size_t *index){
  size_t i;
  int found=0;
  for(i=0;i<list->count;i++){
    if(list->pages[i].granulepos>=0){
      *index=i;
      found=1;
    }
  }
  return found;
}

static int count_timed_between(const page_list *list,size_t a,size_t b){
  size_t i;
  int count=0;
  for(i=a+1;i<=b && i<list->count;i++)
    if(list->pages[i].granulepos>=0)count++;
  return count;
}

static int expected_page(const page_list *list,ogg_int64_t target,
                         size_t *index){
  size_t i;
  int first_ge=-1;
  int last_lt=-1;
  int have_timed=0;

  for(i=0;i<list->count;i++){
    if(list->pages[i].granulepos>=0){
      have_timed=1;
      if(list->pages[i].granulepos<target)
        last_lt=(int)i;
      else if(first_ge<0)
        first_ge=(int)i;
    }
  }

  if(!have_timed){
    *index=0;
    return 1;
  }
  if(first_ge>=0 && list->pages[first_ge].granulepos==target){
    *index=(size_t)first_ge;
    return 1;
  }
  if(last_lt>=0){
    *index=(size_t)last_lt;
    return 1;
  }
  *index=0;
  return 1;
}

static int verify_suffix(const unsigned char *buffer,size_t buffer_bytes,
                         size_t page_base,
                         const page_list *baseline,size_t start,
                         int chunk_size,size_t *bytes_compared){
  page_list got;
  size_t i;
  int result=0;
  ogg_int64_t offset=(ogg_int64_t)(page_base+page_offset(baseline,start));

  memset(&got,0,sizeof(got));
  if(offset<0 || (size_t)offset>buffer_bytes){
    list_clear(&got);
    page_mismatches++;
    return -1;
  }
  if(sequential_scan(buffer+(size_t)offset,buffer_bytes-(size_t)offset,
                     chunk_size,&got)){
    list_clear(&got);
    return -1;
  }

  if(got.count!=baseline->count-start){
    fprintf(stderr,"suffix page count: got %lu expected %lu\n",
            (unsigned long)got.count,
            (unsigned long)(baseline->count-start));
    result=-1;
    page_mismatches++;
  }else{
    for(i=0;i<got.count;i++){
      if(!compare_page(&got.pages[i],&baseline->pages[start+i])){
        fprintf(stderr,"suffix page %lu differs\n",(unsigned long)i);
        result=-1;
        page_mismatches++;
        break;
      }
      *bytes_compared+=got.pages[i].bytes;
    }
  }
  list_clear(&got);
  return result;
}

static void print_common_case(ogg_int64_t target,size_t page,
                              const test_page *selected,
                              ogg_int64_t offset,int timed_distance,
                              size_t suffix_pages,size_t suffix_bytes,
                              int page_exact,int first){
  printf("%s{\"target\":%lld,\"page\":%lu,\"pageno\":%ld,"
         "\"granulepos\":%lld,\"offset\":%lld,\"timed_distance\":%d,"
         "\"suffix_pages\":%lu,\"suffix_bytes\":%lu,\"page_exact\":%s}",
         first?"":",",(long long)target,(unsigned long)page,
         (long)selected->pageno,(long long)selected->granulepos,
         (long long)offset,timed_distance,(unsigned long)suffix_pages,
         (unsigned long)suffix_bytes,page_exact?"true":"false");
}

static void test_single(size_t *bytes_compared,int *max_timed_distance){
  static const ogg_int64_t gps[6]={0,1000,2000,3000,4000,5000};
  static const ogg_int64_t targets[]={
    -10000,0,1,999,1000,1500,2999,3000,4000,5000,5500,5999,6000,7000
  };
  static const char junk[]="JUNK-leading-garbage";
  page_list encoded;
  page_list baseline;
  unsigned char *buffer=NULL;
  size_t buffer_bytes=0;
  ogg_seek_index index;
  size_t i;
  size_t case_count=0;
  size_t single_bytes=0;
  int max_distance=0;
  int printed=0;

  memset(&encoded,0,sizeof(encoded));
  memset(&baseline,0,sizeof(baseline));
  memset(&index,0,sizeof(index));

  CHECK(ogg_seek_index_init(&index)==0,"single index init");
  if(generate_link(0x12345678,gps,6,1,&encoded)){
    fprintf(stderr,"generate_link failed\n");
    failures++;
    goto done;
  }
  CHECK(encoded.count>=9,"stream has spanning pages");
  if(append_bytes(&buffer,&buffer_bytes,junk,sizeof(junk)-1)||
     append_pages(&buffer,&buffer_bytes,&encoded)){
    failures++;
    goto done;
  }

  /* Append a valid page prefix and then stop.  It must be ignored as a
     truncated tail by both the scanner and the index builder. */
  if(encoded.count){
    size_t trunc=encoded.pages[encoded.count-1].bytes;
    if(trunc>19)trunc=19;
    if(append_bytes(&buffer,&buffer_bytes,
                    encoded.pages[encoded.count-1].data,trunc)){
      failures++;
      goto done;
    }
  }

  if(sequential_scan(buffer,buffer_bytes,97,&baseline)){
    fprintf(stderr,"sequential_scan failed\n");
    failures++;
    goto done;
  }
  CHECK(baseline.count==encoded.count,"sequential scan finds every page");
  for(i=0;i<baseline.count && i<encoded.count;i++)
    CHECK(compare_page(&baseline.pages[i],&encoded.pages[i]),
          "sequential scan page bytes");

  CHECK(ogg_seek_index_build(&index,buffer,buffer_bytes)==0,"index build");
  CHECK(index.count==encoded.count,"index covers every complete page");
  for(i=0;i<index.count && i<encoded.count;i++){
    CHECK(index.points[i].offset==
          (ogg_int64_t)(sizeof(junk)-1+page_offset(&encoded,i)),
          "index offset is capture boundary");
    CHECK(index.points[i].granulepos==encoded.pages[i].granulepos,
          "index granulepos");
    CHECK(index.points[i].serialno==encoded.pages[i].serialno,
          "index serialno");
  }

  printf("{\"pages\":%lu,\"index_entries\":%lu,\"cases\":[",
         (unsigned long)baseline.count,(unsigned long)index.count);
  printed=1;

  for(i=0;i<sizeof(targets)/sizeof(targets[0]);i++){
    ogg_seekpoint point;
    size_t selected=0;
    size_t target_page=0;
    size_t expected=0;
    size_t case_bytes=0;
    int timed_distance=0;
    int case_ok;
    int found=ogg_seek_index_find(&index,0x12345678,targets[i],&point);

    CHECK(found==1,"single lookup finds stream");
    if(found!=1)continue;
    CHECK(point.serialno==0x12345678,"single lookup keeps serial");
    CHECK(page_at_offset(&encoded,sizeof(junk)-1,point.offset,&selected),
          "single lookup returns capture boundary");
    if(!page_at_offset(&encoded,sizeof(junk)-1,point.offset,&selected))
      continue;
    CHECK(expected_page(&encoded,targets[i],&expected)==1,
          "expected page computed");
    CHECK(selected==expected,"single lookup follows boundary rule");

    if(!timed_at_or_after(&encoded,targets[i],&target_page))
      CHECK(last_timed_page(&encoded,&target_page),
            "stream has a timed page");

    if(selected<=target_page){
      timed_distance=count_timed_between(&encoded,selected,target_page);
      if(timed_distance>max_distance)max_distance=timed_distance;
      CHECK(timed_distance<=1,"seek is at most one timed page early");
    }else{
      CHECK(0,"seek never skips past the target page");
    }

    case_ok=!verify_suffix(buffer,buffer_bytes,sizeof(junk)-1,
                           &baseline,selected,113,&case_bytes);
    CHECK(case_ok,"single suffix is byte-identical");
    single_bytes+=case_bytes;
    print_common_case(targets[i],selected,&encoded.pages[selected],
                      point.offset,timed_distance,
                      baseline.count-selected,case_bytes,case_ok,
                      case_count==0);
    case_count++;
  }

  printf("],\"max_timed_page_distance\":%d}",max_distance);
  *bytes_compared=single_bytes;
  *max_timed_distance=max_distance;

 done:
  if(!printed)
    printf("{\"pages\":0,\"index_entries\":0,\"cases\":[]}");
  ogg_seek_index_clear(&index);
  list_clear(&baseline);
  list_clear(&encoded);
  _ogg_free(buffer);
}

static void test_chained(size_t *bytes_compared,int *max_timed_distance){
  static const ogg_int64_t gps_a[6]={0,1000,2000,3000,4000,5000};
  static const ogg_int64_t gps_b[6]={0,1000,2000,3000,4000,5000};
  static const ogg_int64_t targets[]={-1,0,1,999,1000,1500,5000,7500};
  page_list a,b;
  page_list encoded;
  page_list baseline;
  unsigned char *buffer=NULL;
  size_t buffer_bytes=0;
  ogg_seek_index index;
  size_t base_b;
  size_t i;
  size_t case_count=0;
  size_t chain_bytes=0;
  int max_distance=0;
  int printed=0;

  memset(&a,0,sizeof(a));
  memset(&b,0,sizeof(b));
  memset(&encoded,0,sizeof(encoded));
  memset(&baseline,0,sizeof(baseline));
  memset(&index,0,sizeof(index));

  CHECK(ogg_seek_index_init(&index)==0,"chain index init");
  if(generate_link(0x11111111,gps_a,6,0,&a) ||
     generate_link(0x22222222,gps_b,6,0,&b)){
    fprintf(stderr,"chained generate_link failed\n");
    failures++;
    goto done;
  }
  CHECK(a.count && a.pages[0].bos && a.pages[a.count-1].eos,
        "link A is a complete logical stream");
  CHECK(b.count && b.pages[0].bos && b.pages[b.count-1].eos,
        "link B is a complete logical stream");
  if(append_pages(&buffer,&buffer_bytes,&a) ||
     append_pages(&buffer,&buffer_bytes,&b)){
    failures++;
    goto done;
  }
  if(sequential_scan(buffer,buffer_bytes,89,&baseline)){
    failures++;
    goto done;
  }
  CHECK(ogg_seek_index_build(&index,buffer,buffer_bytes)==0,
        "chain index build");
  CHECK(index.count==a.count+b.count,"chain index covers both links");
  CHECK(baseline.count==a.count+b.count,"chain scan covers both links");
  base_b=page_offset(&baseline,a.count);

  printf("{\"links\":2,\"pages\":%lu,\"index_entries\":%lu,\"cases\":[",
         (unsigned long)baseline.count,(unsigned long)index.count);
  printed=1;

  for(i=0;i<sizeof(targets)/sizeof(targets[0]);i++){
    int link;
    for(link=0;link<2;link++){
      const page_list *source=link?&b:&a;
      long serial=link?0x22222222:0x11111111;
      size_t expected=0;
      size_t global_expected;
      size_t target_page=0;
      size_t selected=0;
      size_t case_bytes=0;
      int timed_distance=0;
      int case_ok;
      ogg_seekpoint point;
      int found=ogg_seek_index_find(&index,serial,targets[i],&point);

      CHECK(expected_page(source,targets[i],&expected)==1,
            "chain expected page computed");
      global_expected=(link?a.count:0)+expected;
      CHECK(found==1,"chain lookup finds link");
      if(found!=1)continue;
      CHECK(point.serialno==serial,"chain lookup keeps link serial");
      CHECK(page_at_offset(&baseline,0,point.offset,&selected),
            "chain lookup returns capture boundary");
      if(!page_at_offset(&baseline,0,point.offset,&selected))continue;
      CHECK(selected==global_expected,"chain lookup selects own link");
      CHECK(baseline.pages[selected].serialno==serial,
            "chain selected page has requested serial");
      if(link)
        CHECK(point.offset>=(ogg_int64_t)base_b,
              "link B offset is in second physical segment");
      else
        CHECK(point.offset<(ogg_int64_t)base_b,
              "link A offset is in first physical segment");

      if(!timed_at_or_after(source,targets[i],&target_page))
        CHECK(last_timed_page(source,&target_page),
              "chain has a timed page");
      if(expected<=target_page)
        timed_distance=count_timed_between(source,expected,target_page);
      if(timed_distance>max_distance)max_distance=timed_distance;
      CHECK(timed_distance<=1,"chain seek is at most one page early");

      case_ok=!verify_suffix(buffer,buffer_bytes,0,&baseline,selected,
                             107,&case_bytes);
      CHECK(case_ok,"chain suffix is byte-identical");
      chain_bytes+=case_bytes;
      print_common_case(targets[i],selected,&baseline.pages[selected],
                        point.offset,timed_distance,
                        baseline.count-selected,case_bytes,case_ok,
                        case_count==0);
      case_count++;
    }
  }

  printf("],\"max_timed_page_distance\":%d}",max_distance);
  *bytes_compared=chain_bytes;
  *max_timed_distance=max_distance;

 done:
  if(!printed)
    printf("{\"links\":2,\"pages\":0,\"index_entries\":0,\"cases\":[]}");
  ogg_seek_index_clear(&index);
  list_clear(&baseline);
  list_clear(&encoded);
  list_clear(&a);
  list_clear(&b);
  _ogg_free(buffer);
}

static void test_boundaries(void){
  int before_checks=checks;
  int before_failures=failures;
  ogg_seek_index index;
  ogg_seekpoint point;
  ogg_seekpoint saved;
  ogg_page bogus;
  page_list one;
  unsigned char *buffer=NULL;
  size_t buffer_bytes=0;

  memset(&index,0,sizeof(index));
  memset(&point,0,sizeof(point));
  memset(&bogus,0,sizeof(bogus));
  memset(&one,0,sizeof(one));

  printf("{\"empty_index_find\":0,\"cases\":[");

  CHECK(ogg_seek_index_init(NULL)==-1,"null index init");
  CHECK(ogg_seek_index_init(&index)==0,"boundary init");
  CHECK(ogg_seek_index_find(&index,7,0,&point)==0,"empty index");
  CHECK(ogg_seek_index_find(NULL,7,0,&point)==-1,"null index find");
  CHECK(ogg_seek_index_find(&index,7,0,NULL)==-1,"null output");
  CHECK(ogg_seek_index_add(NULL,0,0,7,0)==-1,"null index add");
  CHECK(ogg_seek_index_add_page(NULL,0,&bogus)==-1,"null index add page");
  CHECK(ogg_seek_index_build(NULL,NULL,0)==-1,"null index build");
  CHECK(ogg_seek_index_add(&index,-1,0,7,0)==-1,"negative offset");
  CHECK(ogg_seek_index_add_page(&index,100,&bogus)==-1,
        "page without header");

  CHECK(ogg_seek_index_add(&index,100,100,7,1)==0,"add first page");
  CHECK(ogg_seek_index_add(&index,200,100,7,2)==0,"add duplicate timestamp");
  CHECK(ogg_seek_index_add(&index,300,200,7,3)==0,"add later page");
  CHECK(ogg_seek_index_add(&index,300,300,7,4)==-1,
        "reject duplicate offset");
  CHECK(ogg_seek_index_add(&index,50,300,7,5)==-1,
        "reject decreasing offset");

  memset(&point,0x5a,sizeof(point));
  saved=point;
  CHECK(ogg_seek_index_find(&index,8,0,&point)==0,"unknown serial");
  CHECK(!memcmp(&point,&saved,sizeof(point)),
        "miss leaves output untouched");
  CHECK(ogg_seek_index_find(&index,7,100,&point)==1,"exact timestamp");
  CHECK(point.offset==100,"exact timestamp picks first duplicate");
  CHECK(ogg_seek_index_find(&index,7,150,&point)==1,"between timestamps");
  CHECK(point.offset==200,"between picks last page at or below target");
  CHECK(ogg_seek_index_find(&index,7,200,&point)==1,"exact later timestamp");
  CHECK(point.offset==300,"exact later picks that page");
  CHECK(ogg_seek_index_find(&index,7,250,&point)==1,"after last timestamp");
  CHECK(point.offset==300,"after last picks last timed page");
  CHECK(ogg_seek_index_find(&index,7,-1,&point)==1,
        "negative target before first timestamp");
  CHECK(point.offset==100,"negative target falls back to first page");

  CHECK(ogg_seek_index_add(&index,350,-1,7,6)==0,"add untimed page");
  CHECK(ogg_seek_index_add(&index,400,300,7,7)==0,"add re-timed page");
  CHECK(ogg_seek_index_find(&index,7,250,&point)==1,
        "untimed pages do not change comparison");
  CHECK(point.offset==300,"untimed page skipped by comparison");

  CHECK(ogg_seek_index_build(&index,NULL,1)==-1,"null data with bytes");
  CHECK(index.count==5,"failed build leaves previous index");
  CHECK(ogg_seek_index_build(&index,NULL,0)==0,"empty build");
  CHECK(index.count==0,"empty build clears index");
  CHECK(ogg_seek_index_find(&index,7,0,&point)==0,"empty after build");

  CHECK(ogg_seek_index_init(&index)==0,"untimed index init");
  CHECK(ogg_seek_index_add(&index,10,-1,9,1)==0,"untimed add 1");
  CHECK(ogg_seek_index_add(&index,20,-1,9,2)==0,"untimed add 2");
  CHECK(ogg_seek_index_find(&index,9,99999,&point)==1,
        "untimed-only stream lookup");
  CHECK(point.offset==10,"untimed-only stream returns first page");
  ogg_seek_index_clear(&index);

  {
    static const ogg_int64_t one_gp[2]={0,1234};
    ogg_page og;
    size_t header_bytes;

    CHECK(generate_link(0x77777777,one_gp,2,0,&one)==0,
          "generate page for add_page");
    CHECK(one.count==2,"two generated pages");
    if(one.count==2){
      header_bytes=27+(size_t)one.pages[1].data[26];
      CHECK(header_bytes<one.pages[1].bytes,"valid generated header");
      og.header=one.pages[1].data;
      og.header_len=(long)header_bytes;
      og.body=one.pages[1].data+header_bytes;
      og.body_len=(long)(one.pages[1].bytes-header_bytes);
      CHECK(ogg_seek_index_init(&index)==0,"add_page init");
      CHECK(ogg_seek_index_add_page(&index,777,&og)==0,
            "streaming add_page");
      CHECK(index.count==1,"add_page creates one entry");
      CHECK(index.points[0].offset==777 &&
            index.points[0].granulepos==1234 &&
            index.points[0].serialno==0x77777777 &&
            index.points[0].pageno==1,
            "add_page records page fields");
      ogg_seek_index_clear(&index);
    }
    list_clear(&one);
  }

  CHECK(ogg_seek_index_init(&index)==0,"truncated build init");
  CHECK(append_bytes(&buffer,&buffer_bytes,"OggS",4)==0,"truncated buffer");
  CHECK(ogg_seek_index_build(&index,buffer,buffer_bytes)==0,
        "truncated capture alone is ignored");
  CHECK(index.count==0,"truncated capture creates no entry");
  ogg_seek_index_clear(&index);
  list_clear(&one);
  _ogg_free(buffer);

  printf("],\"checks\":%d,\"failures\":%d}",
         checks-before_checks,failures-before_failures);
}

static void test_seek_loop(void){
  static const ogg_int64_t gps[16]={
    0,1000,2000,3000,4000,5000,6000,7000,
    8000,9000,10000,11000,12000,13000,14000,15000
  };
  page_list a,b,all;
  unsigned char *buffer=NULL;
  size_t buffer_bytes=0;
  ogg_seek_index index;
  ogg_seekpoint *saved_pages;
  size_t saved_count;
  size_t saved_capacity;
  int before_failures=failures;
  int i;
  int ok=0;
  int printed=0;
  size_t loop_bytes=0;

  memset(&a,0,sizeof(a));
  memset(&b,0,sizeof(b));
  memset(&all,0,sizeof(all));
  memset(&index,0,sizeof(index));

  if(generate_link(0x33333333,gps,16,0,&a) ||
     generate_link(0x44444444,gps,16,0,&b) ||
     append_pages(&buffer,&buffer_bytes,&a) ||
     append_pages(&buffer,&buffer_bytes,&b) ||
     sequential_scan(buffer,buffer_bytes,127,&all) ||
     ogg_seek_index_build(&index,buffer,buffer_bytes)){
    failures++;
    goto done;
  }

  saved_pages=index.points;
  saved_count=index.count;
  saved_capacity=index.capacity;

  for(i=0;i<1000;i++){
    size_t selected=0;
    size_t case_bytes=0;
    ogg_int64_t target=(ogg_int64_t)((i*97)%17000);
    long serial=(i&1)?0x44444444:0x33333333;
    ogg_seekpoint point;
    int found=ogg_seek_index_find(&index,serial,target,&point);

    if(found!=1 ||
       point.serialno!=serial ||
       !page_at_offset(&all,0,point.offset,&selected) ||
       all.pages[selected].serialno!=serial){
      failures++;
      break;
    }
    if(verify_suffix(buffer,buffer_bytes,0,&all,selected,127,&case_bytes)){
      failures++;
      break;
    }
    loop_bytes+=case_bytes;
    ok++;
  }

  CHECK(index.points==saved_pages,"seek loop does not move storage");
  CHECK(index.count==saved_count,"seek loop does not change entry count");
  CHECK(index.capacity==saved_capacity,"seek loop does not grow index");
  CHECK(ok==1000,"1000 consecutive lookups succeed");

  printf("{\"iterations\":1000,\"successful\":%d,\"failures\":%d,"
         "\"index_entries_before\":%lu,\"index_entries_after\":%lu,"
         "\"capacity_unchanged\":true,\"bytes_compared\":%lu}",
         ok,failures-before_failures,(unsigned long)saved_count,
         (unsigned long)index.count,(unsigned long)loop_bytes);
  printed=1;

 done:
  if(!printed)
    printf("{\"iterations\":1000,\"successful\":0,\"failures\":1,"
           "\"index_entries_before\":0,\"index_entries_after\":0,"
           "\"capacity_unchanged\":false,\"bytes_compared\":0}");
  ogg_seek_index_clear(&index);
  list_clear(&all);
  list_clear(&a);
  list_clear(&b);
  _ogg_free(buffer);
}

int main(void){
  size_t single_bytes=0;
  size_t chain_bytes=0;
  int single_distance=0;
  int chain_distance=0;

  printf("{\n\"single\":");
  test_single(&single_bytes,&single_distance);
  printf(",\n\"chained\":");
  test_chained(&chain_bytes,&chain_distance);
  printf(",\n\"boundaries\":");
  test_boundaries();
  printf(",\n\"seek_loop\":");
  test_seek_loop();
  printf(",\n\"summary\":{\"checks\":%d,\"failures\":%d,"
         "\"page_mismatches\":%lu,\"single_bytes_compared\":%lu,"
         "\"chained_bytes_compared\":%lu,\"page_bytes_compared\":%lu,"
         "\"max_timed_page_distance\":%d,\"leak_test\":\"ASan/LSan\"}}\n",
         checks,failures,(unsigned long)page_mismatches,
         (unsigned long)single_bytes,(unsigned long)chain_bytes,
         (unsigned long)(single_bytes+chain_bytes),
         single_distance>chain_distance?single_distance:chain_distance);

  if(failures){
    fprintf(stderr,"seek tests failed: %d\n",failures);
    return 1;
  }
  return 0;
}
