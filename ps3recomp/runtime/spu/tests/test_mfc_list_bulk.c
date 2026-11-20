/* ADS60 bulk list DMA (spu_dma.h mfc_list_bulk, R3_NATIVE_DOF_JOB=4): random
 * DMA lists run once element by element (lean path) and once with the bulk
 * merge armed must leave identical LS / guest memory, identical stall state,
 * and the same per-element observer calls in the same order. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../spu_dma.h"

#define MEM_SIZE 0x80000u
static uint8_t memory[MEM_SIZE];
uint8_t* vm_base = memory;
uint8_t g_vm_page_bitmap[65536 / 8];
int g_cri_video_dma;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t g_barrier_sync_watch;
void* volatile g_sn_defer_ctx;
volatile unsigned g_sn_defer;
unsigned long long ps3_ms_now(void) { return 0; }
unsigned spu_sn_defer_ticks(void) { return 0; }
unsigned* spu_ls_watch_list(int* n) { *n=0;return NULL; }

/* observer log: every per-element note, in order */
#define LOG_MAX 8192
static uint32_t s_log[LOG_MAX][3]; static unsigned s_logn;
static void note(uint32_t kind,uint32_t ea,uint32_t n)
{ if(s_logn<LOG_MAX){s_log[s_logn][0]=kind;s_log[s_logn][1]=ea;s_log[s_logn][2]=n;} s_logn++; }
static void put_hook(uint32_t ea,uint32_t n) { note(1,ea,n); }
void (*g_spu_put_hook)(uint32_t, uint32_t) = put_hook;
void (*volatile g_spu_write_watch)(spu_context*, uint32_t, uint32_t, const uint8_t*);
void spu_overlay_note_get(spu_context* c,uint32_t ea,const uint8_t* p,uint32_t n)
{ (void)c;(void)p; note(2,ea,n); }
static uint32_t s_reserved_line = 0xFFFFFFFFu; static unsigned s_coh_notes;
int spu_coh_is_reserved(uint32_t ea) { return ea==s_reserved_line; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void spu_coh_notify_write_except(uint32_t ea,const void* c) { (void)c; if(ea==s_reserved_line)s_coh_notes++; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_lockguard_check(spu_context* c,uint32_t ea,const uint8_t* p,uint32_t n,const char* what)
{ (void)c;(void)ea;(void)p;(void)n;(void)what; }
void spu_ch_wake(spu_context* c) { (void)c; }
SPU_THREAD_LOCAL spu_dma_trace_ent g_spu_dma_ring[512];
SPU_THREAD_LOCAL uint32_t g_spu_dma_ring_n;
SPU_THREAD_LOCAL void* g_spu_putrec;
SPU_THREAD_LOCAL int g_spu_dry_put;
int g_spu_jobtrace_id;
void spu_jobtrace(spu_context* s,uint32_t lsa,uint64_t ea,uint32_t size,uint32_t cmd,int unused)
{ (void)s;(void)lsa;(void)ea;(void)size;(void)cmd;(void)unused; }
volatile uint32_t g_dmaw_lo, g_dmaw_hi, g_dmaw_img;
volatile long g_dmaw_left;
void spu_dmawatch_log(spu_context* s,uint32_t lsa,uint64_t ea,uint32_t size,uint32_t cmd)
{ (void)s;(void)lsa;(void)ea;(void)size;(void)cmd; }
static int s_win_fail_bulk; static unsigned s_win_bulk_calls;
int spu_ls_window_transfer(spu_context* s,uint32_t lsa,uint64_t ea,uint32_t size,uint32_t cmd)
{ (void)s;(void)lsa;(void)ea;(void)cmd;
  if(s_win_fail_bulk && size>0x100){ s_win_bulk_calls++; return 1; } /* only the bulk call moves > one element */
  return 0; }
void spu_phys_trace(const spu_context* s,uint32_t cmd,uint32_t lsa,uint32_t ea,uint32_t size)
{ (void)s;(void)cmd;(void)lsa;(void)ea;(void)size; }
void spu_putrec_note(uint32_t ea,const uint8_t* src,uint32_t size) { (void)src; note(3,ea,size); }
void spu_pmtrace(const void* c,uint32_t spu,uint32_t type,uint32_t a,uint32_t b,uint32_t d)
{ (void)c;(void)spu;(void)type;(void)a;(void)b;(void)d; }
void ps3_guest_mem_notify_put(uint32_t ea) { note(4,ea,0); }
volatile int g_semlog_on = 1;
volatile uint32_t g_semlog_rd_ea = 1;
void semlog_note_mem(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e)
{ (void)c;(void)d;(void)e; note(5,a,b); }
void semlog_note_read(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e)
{ (void)c;(void)d;(void)e; note(6,a,b); }
volatile uint32_t g_semfix_base;
void semfix_note_spu(uint32_t a,uint32_t b) { note(7,a,b); }
void semfix_pre_spu(uint32_t a,uint32_t b) { (void)a;(void)b; }
volatile long g_cpurep_spu_live;
void cpurep_spu_dma(spu_context* s,uint32_t lsa,uint64_t ea,uint32_t size,uint32_t cmd)
{ (void)s;(void)lsa;(void)ea;(void)size;(void)cmd; }

static void check(int ok,const char* what,unsigned it)
{ if(!ok){fprintf(stdout,"FAIL: %s (iteration %u)\n",what,it);exit(2);} }
static uint32_t s_rng = 12345u;
static uint32_t rnd(void) { s_rng = s_rng*1664525u + 1013904223u; return s_rng >> 8; }

typedef struct { uint8_t ls[SPU_LS_SIZE]; uint8_t mem[MEM_SIZE]; uint32_t stall_mask, rem, dest;
                 uint32_t log[LOG_MAX][3]; unsigned logn, coh; uint32_t ring_n; } snap;
static snap s_a, s_b;
static uint8_t s_ls0[SPU_LS_SIZE], s_mem0[MEM_SIZE];

static void run(spu_context* ctx,int bulk,uint32_t nel,uint32_t cmd,snap* out)
{
    memcpy(ctx->ls,s_ls0,SPU_LS_SIZE); memcpy(memory,s_mem0,MEM_SIZE);
    ctx->list_stall_mask=0; ctx->event_status=0; s_logn=0; s_coh_notes=0; g_spu_dma_ring_n=0;
    g_spu_dma_lean_img=419; g_spu_dma_bulk_img=bulk?419:0;
    const uint32_t tag=3;
    check(mfc_run_list(ctx,0x100,nel,0x2000,0,cmd,tag)==0,"list return",0);
    memcpy(out->ls,ctx->ls,SPU_LS_SIZE); memcpy(out->mem,memory,MEM_SIZE);
    out->stall_mask=ctx->list_stall_mask; out->rem=ctx->list_stall_remaining[tag];
    out->dest=ctx->list_stall_dest_lsa[tag];
    out->logn=s_logn; memcpy(out->log,s_log,sizeof s_log); out->coh=s_coh_notes; out->ring_n=g_spu_dma_ring_n;
}

int main(void)
{
    memset(g_vm_page_bitmap,0xFF,sizeof g_vm_page_bitmap);
    spu_context* ctx=(spu_context*)calloc(1,sizeof(*ctx));
    check(ctx!=NULL,"allocate context",0); spu_context_init(ctx,0); ctx->image_id=419;
    unsigned merged_runs=0;
    for(unsigned it=0;it<4000;it++){
        for(unsigned i=0;i<SPU_LS_SIZE;i++)s_ls0[i]=(uint8_t)rnd();
        for(unsigned i=0;i<MEM_SIZE;i++)s_mem0[i]=(uint8_t)rnd();
        const int put=rnd()&1; const uint32_t cmd=put?0x20:0x40;
        const uint32_t nel=1+rnd()%48;
        uint32_t ea=0x20000+(rnd()%0x100)*16, total=0;
        g_semfix_base = (rnd()&3)==0 ? (ea & ~0xFFFu) : 0;
        s_reserved_line = (rnd()&3)==0 ? ((ea + (rnd()%0x2000)) & ~127u) : 0xFFFFFFFFu;
        for(uint32_t k=0;k<nel;k++){
            uint32_t sz=16*(1+rnd()%64), f=0, r=rnd()%20;
            if(r==0) sz=8;                                /* small, non-16 element */
            if(r==1) ea+=32;                              /* gap: not contiguous */
            if(r==2) f=0x80000000u;                       /* stall-and-notify */
            if(r==3) sz=0;                                /* empty element */
            if(total+sz+0x2000+64>=0x30000) sz=16;
            if(ea+sz>=MEM_SIZE-0x100) { ea=0x20000; }
            uint32_t el=0x100+k*8;
            s_ls0[el]=(uint8_t)((sz|f)>>24); s_ls0[el+1]=(uint8_t)((sz|f)>>16);
            s_ls0[el+2]=(uint8_t)((sz|f)>>8); s_ls0[el+3]=(uint8_t)(sz|f);
            s_ls0[el+4]=(uint8_t)(ea>>24); s_ls0[el+5]=(uint8_t)(ea>>16);
            s_ls0[el+6]=(uint8_t)(ea>>8); s_ls0[el+7]=(uint8_t)ea;
            ea+= sz; total+=sz;
            if(f && (rnd()&1)) break;
        }
        run(ctx,0,nel,cmd,&s_a);
        run(ctx,1,nel,cmd,&s_b);
        if(s_b.ring_n==s_a.ring_n && s_a.logn) merged_runs++;
        check(memcmp(s_a.ls,s_b.ls,SPU_LS_SIZE)==0,"LS bytes",it);
        check(memcmp(s_a.mem,s_b.mem,MEM_SIZE)==0,"guest bytes",it);
        check(s_a.stall_mask==s_b.stall_mask && s_a.rem==s_b.rem && s_a.dest==s_b.dest,"stall state",it);
        check(s_a.logn==s_b.logn,"observer call count",it);
        if(s_a.logn<=LOG_MAX) check(memcmp(s_a.log,s_b.log,sizeof(uint32_t)*3*s_a.logn)==0,"observer order",it);
        check(s_a.ring_n==s_b.ring_n,"trace ring entries",it);
        check((s_a.coh!=0)==(s_b.coh!=0),"reservation notify",it);
    }
    fprintf(stdout,"OK: bulk list DMA matches per-element (%u runs)\n",merged_runs);

    /* Late bail-out (after the element scan): a failed run must not be
     * rescanned from each of its elements -- one merge attempt per run. */
    {
        const uint32_t nel=32, ea0=0x20000;
        for(unsigned i=0;i<SPU_LS_SIZE;i++)s_ls0[i]=(uint8_t)rnd();
        for(unsigned i=0;i<MEM_SIZE;i++)s_mem0[i]=(uint8_t)rnd();
        for(uint32_t k=0;k<nel;k++){
            const uint32_t sz=0x100, ea=ea0+k*sz, el=0x100+k*8;
            s_ls0[el]=0; s_ls0[el+1]=0; s_ls0[el+2]=(uint8_t)(sz>>8); s_ls0[el+3]=(uint8_t)sz;
            s_ls0[el+4]=(uint8_t)(ea>>24); s_ls0[el+5]=(uint8_t)(ea>>16);
            s_ls0[el+6]=(uint8_t)(ea>>8); s_ls0[el+7]=(uint8_t)ea;
        }
        run(ctx,0,nel,0x40,&s_a);
        s_win_fail_bulk=1; s_win_bulk_calls=0;
        run(ctx,1,nel,0x40,&s_b);
        s_win_fail_bulk=0;
        check(s_win_bulk_calls==1,"late bail-out: one merge attempt per list",s_win_bulk_calls);
        check(memcmp(s_a.ls,s_b.ls,SPU_LS_SIZE)==0,"late bail-out: LS bytes",0);
        check(s_a.logn==s_b.logn && s_a.ring_n==s_b.ring_n,"late bail-out: per-element fallback",0);

        /* overlap bail-out: data range covers the list itself; reports the scanned run */
        memcpy(ctx->ls,s_ls0,SPU_LS_SIZE);
        g_spu_dma_lean_img=419; g_spu_dma_bulk_img=419;
        uint32_t after=0, scanned=0;
        uint32_t moved=mfc_list_bulk(ctx,0x100,nel,0x0,0,0x40,&after,&scanned);
        check(moved==0 && scanned==nel,"overlap bail-out reports scanned run",scanned);
        moved=mfc_list_bulk(ctx,0x100,nel,0x2000,0,0x40,&after,&scanned);
        check(moved==nel && after==0x2000+nel*0x100,"merge after overlap case",moved);
        fprintf(stdout,"OK: late bail-out is linear (1 merge attempt for %u elements)\n",nel);
    }
    free(ctx); return 0;
}
