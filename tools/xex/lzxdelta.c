/* Standalone port of rexglue's lzx_decompress + lzxdelta_apply_patch (src/system/lzx.cpp).
   Usage: lzxdelta <window_size> <dest_file> <patch_file>   (applies patch to dest in place) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "mspack.h"
#include "lzx.h"
typedef struct { struct mspack_system sys; uint8_t* buf; off_t size, off; } memfile;
static int mread(struct mspack_file* f, void* b, int n) { memfile* m=(memfile*)f; off_t t=m->size-m->off; if(t>n)t=n; memcpy(b,m->buf+m->off,t); m->off+=t; return (int)t; }
static int mwrite(struct mspack_file* f, void* b, int n) { memfile* m=(memfile*)f; off_t t=m->size-m->off; if(t>n)t=n; memcpy(m->buf+m->off,b,t); m->off+=t; return (int)t; }
static void* malloc_(struct mspack_system* s, size_t n) { (void)s; return calloc(n,1); }
static void free_(void* p) { free(p); }
static void copy_(void* s, void* d, size_t n) { memcpy(d,s,n); }
static uint32_t be32(const uint8_t* p){return (uint32_t)p[0]<<24|p[1]<<16|p[2]<<8|p[3];}
static uint16_t be16(const uint8_t* p){return (uint16_t)(p[0]<<8|p[1]);}
static int lzx_decompress(const void* src, size_t slen, void* dst, size_t dlen, uint32_t win, void* ref, size_t rlen) {
  int bits=0; while((1u<<bits)<win) bits++;
  struct mspack_system sys={0}; sys.read=mread; sys.write=mwrite; sys.alloc=malloc_; sys.free=free_; sys.copy=copy_;
  memfile in={0}, out={0}; in.buf=(uint8_t*)src; in.size=slen; out.buf=dst; out.size=dlen;
  struct lzxd_stream* z=lzxd_init(&sys,(struct mspack_file*)&in,(struct mspack_file*)&out,bits,0,0x8000,(off_t)dlen,0);
  if(!z) return 1;
  if(ref){ size_t pad=win-rlen; memset(&z->window[0],0,pad); memcpy(&z->window[pad],ref,rlen); z->ref_data_size=win; }
  int r=lzxd_decompress(z,(off_t)dlen); lzxd_free(z); return r;
}
int main(int argc,char** argv){
  if(argc!=4){fprintf(stderr,"usage\n");return 2;}
  uint32_t win=strtoul(argv[1],0,0);
  FILE* f=fopen(argv[2],"rb"); fseek(f,0,SEEK_END); long dn=ftell(f); rewind(f); uint8_t* dest=malloc(dn); fread(dest,1,dn,f); fclose(f);
  f=fopen(argv[3],"rb"); fseek(f,0,SEEK_END); long pn=ftell(f); rewind(f); uint8_t* p=malloc(pn); fread(p,1,pn,f); fclose(f);
  uint8_t* end=p+pn; int n=0;
  while(p+12<=end){
    uint32_t oa=be32(p),na=be32(p+4); uint16_t ul=be16(p+8),cl=be16(p+10);
    if(!oa&&!na&&!ul&&!cl) break;
    if((long)na+ul>dn || (long)oa+ul>dn){fprintf(stderr,"patch %d out of range na=%x ul=%x\n",n,na,ul);return 3;}
    int psz=-4;
    if(cl==0) memset(dest+na,0,ul);
    else if(cl==1) memmove(dest+na,dest+oa,ul);
    else { psz=cl-4; int r=lzx_decompress(p+12,cl,dest+na,ul,win,dest+oa,ul); if(r){fprintf(stderr,"lzx error %d at patch %d\n",r,n);return 4;} }
    p+=16+psz; n++;
  }
  f=fopen(argv[2],"wb"); fwrite(dest,1,dn,f); fclose(f); printf("applied %d patches\n",n); return 0;
}
