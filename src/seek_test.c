/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE Ogg CONTAINER SOURCE CODE.              *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE Ogg CONTAINER SOURCE CODE IS (C) COPYRIGHT 1994-2026         *
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
} test_page;

typedef struct {
  test_page *pages;
  size_t count;
  size_t capacity;
} test_pages;

static int failures=0;

#define CHECK(cond,msg) do{ \
  if(!(cond)){ \
    fprintf(stderr,"FAIL: %s (%s:%d)\n",msg,__FILE__,__LINE__); \
    failures++; \
  } \
}while(0)

static void pages_clear(test_pages *list){
  size_t i;
  for(i=0;i<list->count;i++)_ogg_free(list->pages[i].data);
  _ogg_free(list->pages);
  memset(list,0,sizeof(*list));
}

static int pages_push(test_pages *list,const ogg_page *og){
  test_page *page;
  size_t bytes;

  if(list->count>=list->capacity){
    size_t capacity=list->capacity?list->capacity*2:16;
    test_page *pages=_ogg_realloc(list->pages,capacity*sizeof(*pages));
    if(!pages)return -1;
    list->pages=pages;
    list->capacity=capacity;
  }

  bytes=(size_t)og->header_len+(size_t)og->body_len;
  page=&list->pages[list->count];
  page->data=_ogg_malloc(bytes?bytes:1);
  if(!page->data)return -1;
  memcpy(page->data,og->header,(size_t)og->header_len);
  memcpy(page->data+og->header_len,og->body,(size_t)og->body_len);
  page->bytes=bytes;
  page->granulepos=ogg_page_granulepos(og);
  page->serialno=ogg_page_serialno(og);
  page->pageno=ogg_page_pageno(og);
  list->count++;
  return 0;
}

static void fill_payload(unsigned char *data,long bytes,long seed){
  long i;
  for(i=0;i<bytes;i++)data[i]=(unsigned char)((seed+i*37)&0xff);
}

static int flush_pages(ogg_stream_state *os,test_pages *list){
  ogg_page og;
  while(ogg_stream_flush(os,&og)){
    if(pages_push(list,&og))return -1;
  }
  return 0;
}

static int generate_stream(long serialno,
                           const ogg_int64_t *granulepos,
                           int packet_count,
                           int add_spanning_packet,
                           test_pages *list){
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
                        const test_pages *list){
  size_t i;
  for(i=0;i<list->count;i++){
    if(append_bytes(data,bytes,list->pages[i].data,list->pages[i].bytes))
      return -1;
  }
  return 0;
}

static int sequential_scan(const unsigned char *data,size_t bytes,
                           int chunk_size,test_pages *list){
  ogg_sync_state oy;
  size_t fed=0;
  int result=0;

  if(ogg_sync_init(&oy))return -1;

  for(;;){
    ogg_page og;
    int ret;

    if(fed<bytes){
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

    ret=ogg_sync_pageout(&oy,&og);
    if(ret>0){
      if(pages_push(list,&og)){
        result=-1;
        break;
      }
    }else if(ret<0){
      continue;
    }else if(fed>=bytes){
      break;
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
         !memcmp(a->data,b->data,a->bytes);
}

static size_t page_offset(const test_pages *list,size_t index){
  size_t i,offset=0;
  for(i=0;i<index;i++)offset+=list->pages[i].bytes;
  return offset;
}

static int verify_suffix(const unsigned char *buffer,size_t buffer_bytes,
                         size_t page_base,
                         const test_pages *baseline,size_t start,
                         int chunk_size,size_t *bytes_compared){
  test_pages got;
  size_t i;
  int result=0;
  size_t offset=page_base+page_offset(baseline,start);

  memset(&got,0,sizeof(got));
  if(sequential_scan(buffer+offset,buffer_bytes-offset,
                     chunk_size,&got)){
    pages_clear(&got);
    return -1;
  }

  if(got.count!=baseline->count-start){
    fprintf(stderr,"suffix page count: got %lu expected %lu\n",
            (unsigned long)got.count,
            (unsigned long)(baseline->count-start));
    result=-1;
  }else{
    for(i=0;i<got.count;i++){
      if(!compare_page(&got.pages[i],&baseline->pages[start+i])){
        fprintf(stderr,"suffix page %lu differs\n",(unsigned long)i);
        result=-1;
        break;
      }
      *bytes_compared+=got.pages[i].bytes;
    }
  }
  pages_clear(&got);
  return result;
}

static int find_page_index(const test_pages *list,ogg_int64_t base,
                           ogg_int64_t offset,
                           size_t *index){
  size_t i,at=0;
  for(i=0;i<list->count;i++){
    if(base+(ogg_int64_t)at==offset){
      *index=i;
      return 1;
    }
    at+=list->pages[i].bytes;
  }
  return 0;
}

static int timed_at_or_after(const test_pages *list,ogg_int64_t target,
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

static int last_timed_page(const test_pages *list,size_t *index){
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

static int count_timed_between(const test_pages *list,size_t a,size_t b){
  size_t i;
  int count=0;
  for(i=a+1;i<=b && i<list->count;i++)
    if(list->pages[i].granulepos>=0)count++;
  return count;
}

static int test_main_stream(void){
  static const ogg_int64_t gps[6]={0,1000,2000,3000,4000,5000};
  static const ogg_int64_t targets[]={
    -10000,0,1,999,1000,1500,2999,3000,4000,5000,5500,5999,6000,7000
  };
  static const char junk[]="JUNK-leading-garbage";
  static const char tail[]="partial-tail";
  test_pages encoded;
  test_pages baseline;
  unsigned char *buffer=NULL;
  size_t buffer_bytes=0;
  ogg_seek_index index;
  size_t i;
  size_t seek_cases=0;
  size_t page_bytes_compared=0;
  int max_timed_distance=0;

  memset(&encoded,0,sizeof(encoded));
  memset(&baseline,0,sizeof(baseline));
  memset(&index,0,sizeof(index));

  CHECK(ogg_seek_index_init(&index)==0,"index init");
  if(generate_stream(0x12345678,gps,6,1,&encoded)){
    fprintf(stderr,"generate_stream failed\n");
    failures++;
    goto done;
  }
  CHECK(encoded.count>=9,"stream has spanning pages");
  if(append_bytes(&buffer,&buffer_bytes,junk,sizeof(junk)-1)||
     append_pages(&buffer,&buffer_bytes,&encoded)||
     append_bytes(&buffer,&buffer_bytes,tail,sizeof(tail)-1)){
    failures++;
    goto done;
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
    CHECK(index.points[i].granulepos==encoded.pages[i].granulepos,
          "index granulepos");
    CHECK(index.points[i].serialno==encoded.pages[i].serialno,
          "index serialno");
  }

  printf("{\"stream\":\"single\",\"pages\":%lu,\"index_entries\":%lu,"
         "\"cases\":[",
         (unsigned long)baseline.count,(unsigned long)index.count);

  for(i=0;i<sizeof(targets)/sizeof(targets[0]);i++){
    ogg_seekpoint point;
    size_t target_page=0;
    size_t selected;
    size_t case_bytes=0;
    int timed_distance=0;
    int case_ok;
    int found=ogg_seek_index_find(&index,0x12345678,targets[i],&point);

    CHECK(found==1,"lookup finds stream");
    if(found!=1)continue;
    CHECK(point.serialno==0x12345678,"lookup keeps serial");
    CHECK(find_page_index(&encoded,(ogg_int64_t)(sizeof(junk)-1),
                          point.offset,&selected),
          "lookup returns exact page boundary");
    if(!find_page_index(&encoded,(ogg_int64_t)(sizeof(junk)-1),
                        point.offset,&selected))continue;

    if(!timed_at_or_after(&encoded,targets[i],&target_page))
      CHECK(last_timed_page(&encoded,&target_page),
            "stream has a timed page");

    if(selected<=target_page){
      timed_distance=count_timed_between(&encoded,selected,target_page);
      if(timed_distance>max_timed_distance)max_timed_distance=timed_distance;
      CHECK(timed_distance<=1,"seek is at most one timed page early");
    }else{
      CHECK(0,"seek never skips past the target page");
    }

    case_ok=!verify_suffix(buffer,buffer_bytes,sizeof(junk)-1,
                           &baseline,selected,113,&case_bytes);
    page_bytes_compared+=case_bytes;
    if(!case_ok){
      fprintf(stderr,"suffix mismatch for target %lld\n",
              (long long)targets[i]);
      failures++;
    }
    printf("%s{\"target\":%lld,\"page\":%lu,\"pageno\":%ld,"
           "\"granulepos\":%lld,\"offset\":%lld,\"timed_distance\":%d,"
           "\"suffix_pages\":%lu,\"suffix_bytes\":%lu,\"page_exact\":%s}",
           seek_cases?",":"",(long long)targets[i],
           (unsigned long)selected,(long)encoded.pages[selected].pageno,
           (long long)encoded.pages[selected].granulepos,
           (long long)point.offset,timed_distance,
           (unsigned long)(baseline.count-selected),
           (unsigned long)case_bytes,case_ok?"true":"false");
    seek_cases++;
  }

  {
    ogg_seekpoint point;
    ogg_seekpoint saved=index.points[0];

    index.points[0].granulepos=-1;
    CHECK(ogg_seek_index_find(&index,0x12345678,-1,&point)==1,
          "below-first target falls back to first page");
    CHECK(point.offset==saved.offset,"fallback offset is first page");
    index.points[0]=saved;
  }

  printf("],\"seek_cases\":%lu,\"page_bytes_compared\":%lu,"
         "\"max_timed_page_distance\":%d}\n",
         (unsigned long)seek_cases,(unsigned long)page_bytes_compared,
         max_timed_distance);

 done:
  ogg_seek_index_clear(&index);
  pages_clear(&baseline);
  pages_clear(&encoded);
  _ogg_free(buffer);
  return failures;
}

static int test_boundaries(void){
  ogg_seek_index index;
  ogg_seekpoint point;
  ogg_page bogus;
  int before=failures;

  memset(&index,0,sizeof(index));
  memset(&bogus,0,sizeof(bogus));
  CHECK(ogg_seek_index_init(&index)==0,"boundary init");
  CHECK(ogg_seek_index_find(&index,7,0,&point)==0,"empty index");
  CHECK(ogg_seek_index_add(&index,100,-1,7,1)==0,"add untimed page");
  CHECK(ogg_seek_index_add(&index,100,0,7,2)==-1,"reject duplicate offset");
  CHECK(ogg_seek_index_add(&index,50,0,7,3)==-1,"reject decreasing offset");
  CHECK(ogg_seek_index_add_page(&index,200,&bogus)==-1,
        "reject page without header");
  CHECK(ogg_seek_index_find(&index,8,0,&point)==0,"unknown serial");
  CHECK(ogg_seek_index_find(&index,7,99999,&point)==1,"untimed-only fallback");
  CHECK(point.offset==100 && point.granulepos==-1,
        "untimed-only fallback returns first");
  CHECK(ogg_seek_index_find(NULL,7,0,&point)==-1,"null index");
  CHECK(ogg_seek_index_find(&index,7,0,NULL)==-1,"null output");
  CHECK(ogg_seek_index_add(&index,-1,0,7,0)==-1,"negative offset");
  ogg_seek_index_clear(&index);
  CHECK(index.points==NULL && index.count==0,"clear resets index");

  CHECK(ogg_seek_index_init(&index)==0,"duplicate-timestamp init");
  CHECK(ogg_seek_index_add(&index,100,100,7,1)==0,"dup add 1");
  CHECK(ogg_seek_index_add(&index,200,100,7,2)==0,"dup add 2");
  CHECK(ogg_seek_index_add(&index,300,200,7,3)==0,"dup add 3");
  CHECK(ogg_seek_index_find(&index,7,100,&point)==1,"exact duplicate lookup");
  CHECK(point.offset==100,"exact duplicate picks first page");
  CHECK(ogg_seek_index_find(&index,7,150,&point)==1,"between lookup");
  CHECK(point.offset==200,"between picks last page at or below target");
  CHECK(ogg_seek_index_find(&index,7,200,&point)==1,"exact later lookup");
  CHECK(point.offset==300,"exact later picks that page");
  CHECK(ogg_seek_index_find(&index,7,250,&point)==1,"after-last lookup");
  CHECK(point.offset==300,"after-last picks last timed page");
  ogg_seek_index_clear(&index);

  return failures-before;
}

static int test_interleaved(void){
  static const ogg_int64_t gps_a[6]={0,1000,2000,3000,4000,5000};
  static const ogg_int64_t gps_b[4]={0,10000,20000,30000};
  test_pages a,b;
  unsigned char *buffer=NULL;
  size_t buffer_bytes=0;
  ogg_seek_index index;
  ogg_seekpoint point;
  size_t i;
  int before=failures;

  memset(&a,0,sizeof(a));
  memset(&b,0,sizeof(b));
  memset(&index,0,sizeof(index));
  CHECK(ogg_seek_index_init(&index)==0,"interleave init");
  if(generate_stream(0x11111111,gps_a,6,0,&a) ||
     generate_stream(0x22222222,gps_b,4,0,&b)){
    failures++;
    goto done;
  }

  for(i=0;i<a.count || i<b.count;i++){
    if(i<a.count && append_bytes(&buffer,&buffer_bytes,
                                 a.pages[i].data,a.pages[i].bytes)){
      failures++;
      goto done;
    }
    if(i<b.count && append_bytes(&buffer,&buffer_bytes,
                                 b.pages[i].data,b.pages[i].bytes)){
      failures++;
      goto done;
    }
  }

  CHECK(ogg_seek_index_build(&index,buffer,buffer_bytes)==0,
        "interleave build");
  CHECK(ogg_seek_index_find(&index,0x11111111,1500,&point)==1,
        "interleave lookup A");
  CHECK(point.serialno==0x11111111 && point.granulepos==1000,
        "interleave A picks its own page");
  CHECK(ogg_seek_index_find(&index,0x22222222,1500,&point)==1,
        "interleave lookup B");
  CHECK(point.serialno==0x22222222 && point.granulepos==0,
        "interleave B first page fallback");
  CHECK(ogg_seek_index_find(&index,0x22222222,25000,&point)==1,
        "interleave B later lookup");
  CHECK(point.serialno==0x22222222 && point.granulepos==20000,
        "interleave B picks its own page");

 done:
  ogg_seek_index_clear(&index);
  pages_clear(&a);
  pages_clear(&b);
  _ogg_free(buffer);
  return failures-before;
}

int main(void){
  test_main_stream();
  test_boundaries();
  test_interleaved();

  if(failures){
    fprintf(stderr,"seek tests failed: %d\n",failures);
    return 1;
  }
  return 0;
}
