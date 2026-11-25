#define _POSIX_C_SOURCE 200809L
#include "m1_motion.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __unix__
#include <poll.h>
#include <time.h>
#include <unistd.h>
#endif

typedef struct {
    M1_Motion m;
    M1_Frame queue[128];
    unsigned head,tail,sends,enables,flushes;
    uint32_t now;
    uint16_t sw[2];
    int32_t target[2],velocity[2];
    uint8_t mode[2],error[2],drop[2];
    int mismatch,abort_target,stuck,stdio_mode;
    char log[65536]; size_t log_size;
} Rig;
static uint32_t value(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void put(uint8_t *p,uint32_t v) { unsigned i; for(i=0;i<4;++i) p[i]=(uint8_t)(v>>(8*i)); }
static int send_frame(void *user,const M1_Frame *f) {
    Rig *r=user;
    assert(m1_motion_frame_allowed(f)); assert(r->head-r->tail<128);
    r->queue[r->head++%128]=*f; ++r->sends;
    if(f->data[0]!=0x40 && f->data[1]==0x40 && f->data[2]==0x60 && f->data[4]==15) ++r->enables;
    return 1;
}
static void emit(void *user,const char *s) {
    Rig *r=user; size_t n=strlen(s);
    if(r->stdio_mode) { puts(s); fflush(stdout); }
    if(r->log_size+n+2>=sizeof(r->log)) r->log_size=0;
    memcpy(r->log+r->log_size,s,n); r->log_size+=n;
    r->log[r->log_size++]='\n'; r->log[r->log_size]=0;
}
static void flush(void *user) { Rig *r=user; r->tail=r->head; ++r->flushes; }
static void initialize(Rig *r) {
    memset(r,0,sizeof(*r)); r->sw[0]=r->sw[1]=0x1240;
    m1_motion_init(&r->m,send_frame,emit,flush,r); m1_motion_transport(&r->m,1);
}
static void reply(Rig *r,const M1_Frame *f) {
    unsigned node=(unsigned)(f->id-0x601),size=4;
    uint16_t index=(uint16_t)(f->data[1]|((uint16_t)f->data[2]<<8));
    uint32_t v=value(f->data+4); M1_Frame answer=*f;
    if(node>1 || r->drop[node]) return;
    answer.id=(uint16_t)(0x581+node);
    if(f->data[0]!=0x40) {
        answer.data[0]=0x60;
        if(index==0x60ff) {
            if(r->abort_target && v) { answer.data[0]=0x80; put(answer.data+4,0x06090030); }
            else { r->target[node]=(int32_t)v; if(!r->stuck) r->velocity[node]=(int32_t)v; }
        }
        if(index==0x6060) r->mode[node]=(uint8_t)v;
        if(index==0x6040) {
            if(v==0) r->sw[node]=0x0240;
            if(v==6) r->sw[node]=0x0221;
            if(v==7) r->sw[node]=0x0223;
            if(v==15) r->sw[node]=0x0227;
            if(v==11) { r->sw[node]=0x0207; if(!r->stuck) r->velocity[node]=0; }
        }
    } else {
        switch(index) {
            case 0x303d: v=r->mismatch?4:5; size=2; break;
            case 0x60a9: v=0xfd010300u; break;
            case 0x6096: v=1; break;
            case 0x6041: v=r->sw[node]|(r->velocity[node]==0?0x1000u:0); size=2; break;
            case 0x6061: v=r->mode[node]; size=1; break;
            case 0x1001: v=r->error[node]; size=1; break;
            case 0x606c: v=(uint32_t)r->velocity[node]; break;
            default: assert(0);
        }
        answer.data[0]=(uint8_t)(0x43|((4-size)<<2)); put(answer.data+4,v);
    }
    m1_motion_receive(&r->m,&answer,r->now);
}
static void advance(Rig *r,unsigned ms) {
    unsigned i;
    for(i=0;i<ms;++i) {
        ++r->now;
        m1_motion_tick(&r->m,r->now);
        if(r->tail!=r->head) { M1_Frame f=r->queue[r->tail++%128]; reply(r,&f); }
    }
}
static void input(Rig *r,const char *s) { while(*s) m1_motion_input(&r->m,(uint8_t)*s++); }
static void configure(Rig *r) {
    input(r,"M1M1 testlink 1 HELLO\n");
    input(r,"M1M1 testlink 2 CONFIG 1 2 1 -1 1 1 100 100 100 200 4244701952 1 1 1\n");
    assert(r->m.configured);
}
static void arm_rig(Rig *r) {
    configure(r); input(r,"M1M1 testlink 3 ARM\n"); advance(r,400);
    assert(r->m.state==M1_READY); assert(strstr(r->log,"cmd=ARM status=complete reason=ready"));
}
static void tests(void) {
    Rig r; unsigned n;
    initialize(&r); advance(&r,2000); assert(r.sends==0);
    input(&r,"ACTIVATE\nMOVE 1 0\n"); assert(r.sends==0);
    input(&r,"M1M1 testlink 1 HELLO\nM1M1 testlink 2 CONFIG 1 2 1 -1 1 1 100 100 100 200 4244701952 1 1 0\n");
    assert(!r.m.configured); assert(strstr(r.log,"commissioning_required"));

    initialize(&r); arm_rig(&r); advance(&r,1000); assert(r.m.state==M1_READY); /* stationary idle */
    input(&r,"M1M1 testlink 4 MOVE 0.1 0\n"); advance(&r,50);
    assert(r.target[0]>0 && r.target[1]<0);
    assert(strstr(r.log,"cmd=MOVE status=complete reason=targets_written"));
    input(&r,"M1M1 testlink 5 MOVE -0.1 0\n"); advance(&r,50);
    assert(r.target[0]<0 && r.target[1]>0);
    input(&r,"M1M1 testlink 6 MOVE 0 0.2\n"); advance(&r,50);
    assert(r.target[0]<0 && r.target[1]<0); /* left turn, mirrored right motor */
    input(&r,"M1M1 testlink 7 MOVE 0 -0.2\n"); advance(&r,50);
    assert(r.target[0]>0 && r.target[1]>0);
    n=r.sends; input(&r,"M1M1 testlink 7 MOVE 0.1 0\nM1M1 oldlink1 100 MOVE 0.1 0\n");
    assert(r.sends==n); assert(strstr(r.log,"stale_request"));
    input(&r,"M1M1 testlink 8 STOP\n"); advance(&r,500);
    assert(r.m.state==M1_READY && !r.target[0] && !r.target[1]);
    assert(strstr(r.log,"cmd=STOP status=complete reason=standstill_feedback"));
    input(&r,"M1M1 testlink 9 MOVE nan 0\n"); assert(strstr(r.log,"invalid_velocity"));
    input(&r,"M1M1 testlink 10 MOVE 0.1 0\n"); advance(&r,600);
    assert(r.m.state==M1_FAULT); assert(strstr(r.log,"command_timeout"));
    advance(&r,50); assert(!r.velocity[0] && !r.velocity[1]);
    input(&r,"M1M1 testlink 11 MOVE 0.1 0\n"); assert(strstr(r.log,"not_ready"));
    input(&r,"M1M1 testlink 12 DISARM\n"); advance(&r,600); assert(r.m.state==M1_OFF);

    initialize(&r); configure(&r); r.mismatch=1;
    input(&r,"M1M1 testlink 3 ARM\n"); advance(&r,500);
    assert(r.m.state==M1_FAULT && !r.enables); assert(strstr(r.log,"configuration_mismatch"));

    initialize(&r); arm_rig(&r); r.abort_target=1;
    input(&r,"M1M1 testlink 4 MOVE 0.1 0\n"); advance(&r,100);
    assert(r.m.state==M1_FAULT && strstr(r.log,"cmd=MOVE status=failed reason=sdo_abort"));

    initialize(&r); arm_rig(&r); r.drop[1]=1;
    input(&r,"M1M1 testlink 4 MOVE 0.1 0\n"); advance(&r,300);
    assert(r.m.state==M1_FAULT && strstr(r.log,"sdo_timeout"));
    assert(!strstr(r.log,"cmd=MOVE status=complete"));

    initialize(&r); arm_rig(&r);
    input(&r,"M1M1 testlink 4 MOVE 0.1 0\n"); advance(&r,50); r.stuck=1;
    input(&r,"M1M1 testlink 5 STOP\n"); advance(&r,3100);
    assert(r.m.state==M1_FAULT && strstr(r.log,"cmd=STOP status=failed"));
    assert(!strstr(r.log,"cmd=STOP status=complete"));

    initialize(&r); arm_rig(&r);
    input(&r,"M1M1 testlink 4 MOVE 0.1 0\nM1M1 testlink 5 ESTOP\n"); advance(&r,600);
    assert(r.m.state==M1_OFF && !r.target[0] && !r.target[1]);
    assert(strstr(r.log,"superseded_by_stop"));
    m1_motion_transport(&r.m,0); assert(r.m.state==M1_FAULT);

    initialize(&r); arm_rig(&r); m1_motion_input_lost(&r.m);
    assert(r.m.state==M1_FAULT && strstr(r.log,"uart_input_lost"));

    initialize(&r); arm_rig(&r);
    input(&r,"M1M1 testlink 4 MOVE 0.1 0\n"); advance(&r,40);
    input(&r,"M1M1 newlink1 1 HELLO\n"); advance(&r,600);
    assert(r.m.state==M1_OFF && !r.m.configured && !r.velocity[0] && !r.velocity[1]);
    assert(strstr(r.log,"session=newlink1 seq=1 cmd=HELLO status=complete reason=session_open"));
    input(&r,"M1M1 testlink 99 MOVE 0.1 0\n"); assert(r.m.state==M1_OFF);
    puts("M1 motion: differential commands, admission, ACKs, stop feedback, timeout and failures passed");
}
int main(int argc,char **argv) {
#ifdef __unix__
    if(argc==2 && !strcmp(argv[1],"--stdio")) {
        Rig r; struct timespec start,now;
        initialize(&r); r.stdio_mode=1; clock_gettime(CLOCK_MONOTONIC,&start);
        for(;;) {
            struct pollfd fd={STDIN_FILENO,POLLIN,0}; unsigned char bytes[512]; ssize_t n,i;
            (void)poll(&fd,1,1);
            clock_gettime(CLOCK_MONOTONIC,&now);
            { uint32_t t=(uint32_t)((now.tv_sec-start.tv_sec)*1000+(now.tv_nsec-start.tv_nsec)/1000000);
              if(t>r.now) advance(&r,t-r.now); }
            if(fd.revents&(POLLIN|POLLHUP)) {
                n=read(STDIN_FILENO,bytes,sizeof(bytes)); if(n<=0) return 0;
                for(i=0;i<n;++i) m1_motion_input(&r.m,bytes[i]);
            }
        }
    }
#else
    (void)argc; (void)argv;
#endif
    tests(); return 0;
}
