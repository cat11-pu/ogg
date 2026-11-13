#include <ogg/ogg.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>

/* Deterministic PRNG so both builds see byte-identical page streams. */
static uint32_t rng_state;
static uint32_t xr_next(void){
  uint32_t x=rng_state;
  x^=x<<13;
  x^=x>>17;
  x^=x<<5;
  rng_state=x;
  return x;
}

static double now_sec(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC,&ts);
  return (double)ts.tv_sec+(double)ts.tv_nsec*1e-9;
}

static int cmp_double(const void *a,const void *b){
  double da=*(const double *)a, db=*(const double *)b;
  return (da>db)-(da<db);
}

typedef struct {
  unsigned char *data;   /* concatenated, contiguous pages */
  long           size;   /* total bytes                          */
  long           cap;
  int            npages;
  long          *plen;   /* per-page total length (header+body)  */
  int            nalloc;
  int            npkts;   /* packets encoded                     */
} page_blob;

static void blob_init(page_blob *b){
  b->nalloc=1024;
  b->data=malloc(1u<<20);
  b->plen=malloc((size_t)b->nalloc*sizeof(*b->plen));
  b->size=0; b->cap=1u<<20; b->npages=0; b->npkts=0;
}

static void blob_free(page_blob *b){
  free(b->data); free(b->plen);
}

static void blob_append(page_blob *b,const unsigned char *p,long n){
  if(b->size+n>b->cap){
    while(b->size+n>b->cap) b->cap*=2;
    b->data=realloc(b->data,(size_t)b->cap);
  }
  memcpy(b->data+b->size,p,n);
  b->size+=n;
}

static void blob_page_end(page_blob *b,long page_len){
  if(b->npages>=b->nalloc){
    b->nalloc*=2;
    b->plen=realloc(b->plen,(size_t)b->nalloc*sizeof(*b->plen));
  }
  b->plen[b->npages++]=page_len;
}

/* Encode npages one-packet pages with the given body size. */
static void build_pages(page_blob *b,int npages,long body_size){
  ogg_stream_state os;
  ogg_packet op;
  ogg_page og;
  long i;

  unsigned char *body=malloc((size_t)(body_size?body_size:1));
  for(i=0;i<body_size;i++) body[i]=(unsigned char)xr_next();

  ogg_stream_init(&os,0x1a2b3c4d);
  memset(&op,0,sizeof(op));
  op.packet=body;
  op.bytes=body_size;
  op.b_o_s=1;

  for(i=0;i<npages;i++){
    op.b_o_s=(i==0);
    op.e_o_s=(i==npages-1);
    op.granulepos=(int64_t)(i+1)*1000;
    op.packetno=i;
    ogg_stream_packetin(&os,&op);
    b->npkts++;
    while(ogg_stream_pageout(&os,&og)){
      blob_append(b,og.header,og.header_len);
      blob_append(b,og.body,og.body_len);
      blob_page_end(b,og.header_len+og.body_len);
    }
  }
  if(ogg_stream_flush(&os,&og)){
    blob_append(b,og.header,og.header_len);
    blob_append(b,og.body,og.body_len);
    blob_page_end(b,og.header_len+og.body_len);
  }
  ogg_stream_clear(&os);
  free(body);
}

/* FNV-1a 64-bit, used to fingerprint packet payloads. */
static uint64_t fnv(const unsigned char *p,long n){
  uint64_t h=1469598103934665603ULL;
  long i;
  for(i=0;i<n;i++){ h^=p[i]; h*=1099511628211ULL; }
  return h;
}

static void run_check(void){
  /* Mixed body sizes: tiny, segment-table boundaries, multi-segment, max. */
  static const long sizes[]={0,1,63,254,255,256,509,510,511,1000,4096,
                             20000,65025};
  int n=(int)(sizeof(sizes)/sizeof(sizes[0]));
  int si;
  page_blob b;
  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_page og;
  ogg_packet op;
  int idx=0;

  rng_state=0xdeadbeefu;
  blob_init(&b);
  for(si=0;si<n;si++) build_pages(&b,4,sizes[si]);

  ogg_sync_init(&oy);
  ogg_stream_init(&os,0x1a2b3c4d);
  memcpy(ogg_sync_buffer(&oy,b.size),b.data,b.size);
  ogg_sync_wrote(&oy,b.size);

  while(ogg_sync_pageout(&oy,&og)==1){
    ogg_stream_pagein(&os,&og);
    printf("page %d hlen=%ld blen=%ld ver=%d cont=%d bos=%d eos=%d "
           "gran=%" PRId64 " serial=0x%08x pageno=%ld segs=%d packets=%d "
           "crc=%02x%02x%02x%02x\n",
           idx,og.header_len,og.body_len,
           ogg_page_version(&og),ogg_page_continued(&og),
           ogg_page_bos(&og),ogg_page_eos(&og),
           (int64_t)ogg_page_granulepos(&og),
           (unsigned)ogg_page_serialno(&og),ogg_page_pageno(&og),
           og.header[26],ogg_page_packets(&og),
           og.header[22],og.header[23],og.header[24],og.header[25]);
    printf("  segtab:");
    for(int s=0;s<og.header[26];s++) printf(" %u",og.header[27+s]);
    printf("\n");
    while(ogg_stream_packetout(&os,&op)==1){
      printf("  pkt no=%lld bos=%ld eos=%ld gran=%" PRId64
             " bytes=%ld fnv=%016" PRIx64 "\n",
             (long long)op.packetno,op.b_o_s,op.e_o_s,
             (int64_t)op.granulepos,(long)op.bytes,
             fnv(op.packet,op.bytes));
    }
    idx++;
  }
  printf("pages_total=%d blob_bytes=%ld\n",idx,b.size);
  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
  blob_free(&b);
}

#define TRIALS 15

static double bench_crc_only(page_blob *b,int reps){
  double t[TRIALS];
  int k;
  volatile uint32_t sink=0;
  unsigned char *work=malloc((size_t)b->size);
  memcpy(work,b->data,b->size);
  for(k=0;k<TRIALS;k++){
    double t0=now_sec();
    long acc=0;
    int r;
    for(r=0;r<reps;r++){
      ogg_page lg;
      long off=0;
      int i;
      for(i=0;i<b->npages;i++){
        long page_len=b->plen[i];
        int hlen=((unsigned char *)b->data)[off+26]+27;
        lg.header=work+off;
        lg.header_len=hlen;
        lg.body=work+off+hlen;
        lg.body_len=page_len-hlen;
        ogg_page_checksum_set(&lg);
        acc+=lg.header[22];
        off+=page_len;
      }
    }
    t[k]=(now_sec()-t0)/(double)(b->npages*reps);
    sink+=(uint32_t)acc;
  }
  (void)sink;
  qsort(t,TRIALS,sizeof(double),cmp_double);
  free(work);
  return t[0]; /* best (minimum) time */
}

static double bench_sync(page_blob *b,int reps){
  double t[TRIALS];
  int k;
  for(k=0;k<TRIALS;k++){
    double t0=now_sec();
    int r;
    ogg_sync_state oy;
    ogg_page og;
    ogg_sync_init(&oy);
    unsigned char *src=malloc((size_t)b->size);
    memcpy(src,b->data,b->size);
    for(r=0;r<reps;r++){
      memcpy(ogg_sync_buffer(&oy,b->size+4096),src,b->size);
      ogg_sync_wrote(&oy,b->size);
      int got=0;
      while(ogg_sync_pageout(&oy,&og)==1) got++;
      if(got!=b->npages){
        fprintf(stderr,"sync miscount: got=%d want=%d\n",got,b->npages);
        exit(2);
      }
      ogg_sync_reset(&oy);
    }
    ogg_sync_clear(&oy);
    free(src);
    t[k]=(now_sec()-t0)/(double)(b->npages*reps);
  }
  qsort(t,TRIALS,sizeof(double),cmp_double);
  return t[0]; /* best (minimum) time */
}

static double bench_pagein(page_blob *b,int reps){
  double t[TRIALS];
  int k;
  for(k=0;k<TRIALS;k++){
    double t0=now_sec();
    int r;
    ogg_sync_state oy;
    ogg_stream_state os;
    ogg_page og;
    ogg_packet op;
    ogg_sync_init(&oy);
    unsigned char *src=malloc((size_t)b->size);
    memcpy(src,b->data,b->size);
    for(r=0;r<reps;r++){
      memcpy(ogg_sync_buffer(&oy,b->size+4096),src,b->size);
      ogg_sync_wrote(&oy,b->size);
      int got=0;
      ogg_stream_init(&os,0x1a2b3c4d);
      while(ogg_sync_pageout(&oy,&og)==1){
        ogg_stream_pagein(&os,&og);
        while(ogg_stream_packetout(&os,&op)==1) got++;
      }
      if(got!=b->npkts){
        fprintf(stderr,"pagein miscount: got=%d want=%d\n",got,b->npkts);
        exit(2);
      }
      ogg_stream_clear(&os);
      ogg_sync_reset(&oy);
    }
    ogg_sync_clear(&oy);
    free(src);
    t[k]=(now_sec()-t0)/(double)(b->npages*reps);
  }
  qsort(t,TRIALS,sizeof(double),cmp_double);
  return t[0]; /* best (minimum) time */
}

static void run_time(void){
  /* representative page body lengths (Ogg max body = 255*255 = 65025) */
  static const long sizes[]={0,64,255,1024,4096,16384,65025};
  int ns=(int)(sizeof(sizes)/sizeof(sizes[0]));
  int si;
  const int target=10000; /* pages processed per timed pass */

  printf("body_bytes page_bytes crc_ns/page sync_ns/page full_ns/page"
         " crc_GB/s sync_GB/s full_GB/s\n");
  for(si=0;si<ns;si++){
    page_blob b;
    long body=sizes[si];
    int npackets;
    int reps;
    double tc,ts,tf;
    rng_state=0x12345678u+ (uint32_t)body;
    blob_init(&b);
    /* cap one stream around 128 MB; repeat passes to reach ~10k pages.
       zero-byte packets coalesce 255/page, so encode more of them. */
    npackets = body>12000 ? 1500 : body>3000 ? 4000
              : body==0     ? 255000 : 10000;
    build_pages(&b,npackets,body);
    reps=(target+b.npages-1)/b.npages;
    if(reps<1) reps=1;

    tc=bench_crc_only(&b,reps);
    ts=bench_sync(&b,reps);
    tf=bench_pagein(&b,reps);
    printf("%ld %ld %d %.1f %.1f %.1f %.3f %.3f %.3f\n",
           body,b.size/b.npages,b.npages*reps,
           tc*1e9,ts*1e9,tf*1e9,
           (b.size/b.npages)/tc/1e9,
           (b.size/b.npages)/ts/1e9,
           (b.size/b.npages)/tf/1e9);
    fflush(stdout);
    blob_free(&b);
  }
}

int main(int argc,char **argv){
  if(argc==2 && !strcmp(argv[1],"check")){ run_check(); return 0; }
  if(argc==2 && !strcmp(argv[1],"time")){ run_time(); return 0; }
  fprintf(stderr,"usage: %s check|time\n",argv[0]);
  return 1;
}
