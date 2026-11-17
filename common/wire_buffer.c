/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wire_buffer.h"
#include <stdlib.h>
#include <string.h>
__attribute__((weak)) void *ph_wire_malloc(size_t size) { return malloc(size); }
__attribute__((weak)) void ph_wire_free(void *p) { free(p); }
ph_buf *ph_buf_new(size_t capacity)
{ return ph_buf_new_headroom(capacity,0); }
ph_buf *ph_buf_new_headroom(size_t capacity,size_t headroom)
{
    if(!capacity || headroom>(size_t)-1-capacity)return NULL;
    ph_buf *b=ph_wire_malloc(sizeof(*b));
    if(!b)return NULL;
    b->storage=ph_wire_malloc(capacity+headroom);
    if(!b->storage){ph_wire_free(b);return NULL;}
    b->buf=b->storage+headroom;b->owner=NULL;b->references=1;
    b->len=0;b->cap=capacity;b->buf[0]=0;return b;
}
ph_buf *ph_buf_retain(ph_buf *b)
{ if(b)__sync_add_and_fetch(&b->references,1);return b; }
bool ph_buf_unique(const ph_buf *b)
{ return b && !b->owner && __sync_add_and_fetch((unsigned *)&b->references,0)==1; }
ph_buf *ph_buf_prefix(ph_buf *b,const void *prefix,size_t length)
{
    if(!ph_buf_unique(b) || length>(size_t)(b->buf-b->storage))return NULL;
    ph_buf *view=ph_wire_malloc(sizeof(*view));if(!view)return NULL;
    *view=(ph_buf){.buf=b->buf-length,.len=b->len+length,.cap=b->cap+length,
                   .owner=ph_buf_retain(b),.references=1};
    memcpy(view->buf,prefix,length);return view;
}
size_t ph_buf_owned_bytes(const ph_buf *b)
{
    size_t bytes=0;
    while(b){
        bytes+=(sizeof(*b)+31)&~(size_t)15;
        if(!b->owner)bytes+=(b->cap+(size_t)(b->buf-b->storage)+31)&~(size_t)15;
        b=b->owner;
    }
    return bytes;
}
bool ph_buf_reserve(ph_buf *b,size_t capacity)
{
    if(!b || !b->buf || b->len>=b->cap || !ph_buf_unique(b))return false;
    if(capacity<=b->cap)return true;
    size_t headroom=(size_t)(b->buf-b->storage);
    if(headroom>(size_t)-1-capacity)return false;
    char *next=ph_wire_malloc(capacity+headroom);
    if(!next)return false;
    memcpy(next+headroom,b->buf,b->len+1);ph_wire_free(b->storage);
    b->storage=next;b->buf=next+headroom;b->cap=capacity;return true;
}
void ph_buf_free(ph_buf *b)
{
    while(b && !__sync_sub_and_fetch(&b->references,1)){
        ph_buf *owner=b->owner;
        ph_wire_free(b->storage);ph_wire_free(b);b=owner;
    }
}
