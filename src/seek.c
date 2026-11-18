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

 function: build and query a page index for timestamp-based seeking

 ********************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <ogg/ogg.h>

int ogg_seek_index_init(ogg_seek_index *idx){
  if(!idx) return -1;
  memset(idx,0,sizeof(*idx));
  return 0;
}

void ogg_seek_index_clear(ogg_seek_index *idx){
  if(idx){
    if(idx->points)_ogg_free(idx->points);
    memset(idx,0,sizeof(*idx));
  }
}

int ogg_seek_index_add(ogg_seek_index *idx,
                       ogg_int64_t offset,
                       ogg_int64_t granulepos,
                       long serialno,
                       long pageno){
  ogg_seekpoint *points;
  size_t capacity;

  if(!idx || offset<0) return -1;

  /* Entries are in physical stream order.  A non-increasing offset would
     make the stored page boundary ambiguous. */
  if(idx->count && offset<=idx->points[idx->count-1].offset) return -1;

  if(idx->count>=idx->capacity){
    if(idx->capacity>(size_t)-1/2) return -1;
    capacity=idx->capacity?idx->capacity*2:64;
    if(capacity>(size_t)-1/sizeof(*idx->points)) return -1;
    points=_ogg_realloc(idx->points,capacity*sizeof(*idx->points));
    if(!points) return -1;
    idx->points=points;
    idx->capacity=capacity;
  }

  idx->points[idx->count].offset=offset;
  idx->points[idx->count].granulepos=granulepos;
  idx->points[idx->count].serialno=serialno;
  idx->points[idx->count].pageno=pageno;
  idx->count++;
  return 0;
}

int ogg_seek_index_add_page(ogg_seek_index *idx,
                            ogg_int64_t offset,
                            const ogg_page *og){
  if(!idx || !og || !og->header || og->header_len<27) return -1;

  return ogg_seek_index_add(idx,offset,ogg_page_granulepos(og),
                            ogg_page_serialno(og),ogg_page_pageno(og));
}

int ogg_seek_index_build(ogg_seek_index *idx,
                         const void *data,
                         size_t bytes){
  ogg_seek_index built;
  ogg_sync_state oy;
  size_t fed=0;
  int need_data=1;

  if(!idx || (!data && bytes)) return -1;
  if(ogg_seek_index_init(&built)) return -1;
  if(ogg_sync_init(&oy)){
    ogg_seek_index_clear(&built);
    return -1;
  }

  for(;;){
    ogg_page og;
    ogg_int64_t offset;
    long ret;

    if(need_data && fed<bytes){
      size_t chunk=bytes-fed;
      char *buffer;

      if(chunk>65536) chunk=65536;
      buffer=ogg_sync_buffer(&oy,(long)chunk);
      if(!buffer) goto fail;
      memcpy(buffer,(const unsigned char *)data+fed,chunk);
      if(ogg_sync_wrote(&oy,(long)chunk)) goto fail;
      fed+=chunk;
    }
    need_data=0;

    /* fed is the total number of bytes handed to the sync state;
       fill-returned is the number still pending at the read frontier. */
    offset=(ogg_int64_t)fed-(ogg_int64_t)(oy.fill-oy.returned);
    ret=ogg_sync_pageseek(&oy,&og);
    if(ret>0){
      if(ogg_seek_index_add_page(&built,offset,&og)) goto fail;
      continue;
    }

    if(ret<0) continue; /* skipped junk; the next call rescans */

    /* ret == 0: need more data.  If the input is exhausted this is a
       truncated tail and cannot describe another complete page. */
    if(fed>=bytes) break;
    need_data=1;
  }

  ogg_sync_clear(&oy);
  ogg_seek_index_clear(idx);
  *idx=built;
  return 0;

 fail:
  ogg_sync_clear(&oy);
  ogg_seek_index_clear(&built);
  return -1;
}

int ogg_seek_index_find(const ogg_seek_index *idx,
                        long serialno,
                        ogg_int64_t target_granulepos,
                        ogg_seekpoint *point){
  const ogg_seekpoint *first=NULL;
  const ogg_seekpoint *first_ge=NULL;
  const ogg_seekpoint *last_lt=NULL;
  int have_timed=0;
  size_t i;

  if(!idx || !point) return -1;
  if(!idx->points || idx->count==0) return 0;

  for(i=0;i<idx->count;i++){
    const ogg_seekpoint *p=&idx->points[i];

    if(p->serialno!=serialno) continue;
    if(!first) first=p;
    if(p->granulepos>=0){
      have_timed=1;
      if(p->granulepos<target_granulepos){
        last_lt=p;
      }else if(!first_ge){
        first_ge=p;
      }
    }
  }

  if(!first) return 0; /* serial number not present */

  if(!have_timed){
    /* A stream with no timed pages can still be read from its beginning. */
    *point=*first;
    return 1;
  }

  /* An exact timestamp hit selects the first page carrying that
     timestamp.  This avoids skipping earlier pages that share it. */
  if(first_ge && first_ge->granulepos==target_granulepos){
    *point=*first_ge;
    return 1;
  }

  if(last_lt){
    /* target lies between last_lt and first_ge.  Starting at last_lt is
       exactly one timed page early and leaves no data gap. */
    *point=*last_lt;
    return 1;
  }

  if(first_ge){
    /* No page ends at or before the target.  Start at the first physical
       page for this stream so that headers and continued-packet state are
       available, rather than skipping ahead to the first timed page. */
    *point=*first;
    return 1;
  }

  /* Unreachable for valid granulepos values, but keep a defined result. */
  *point=*first;
  return 1;
}
