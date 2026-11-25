#include "m1_motion.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDO_MS 100u
#define FRESH_MS 400u
#define COMMAND_MS 500u
#define OPERATION_MS 3000u
static int due(uint32_t now, uint32_t deadline) { return now-deadline<0x80000000u; }
static const char *state_name(M1_MotionState s) {
    static const char *names[]={"off","arming","ready","stopping","fault"};
    return names[(unsigned)s];
}
static void output(M1_Motion *m, const char *line) { if(m->emit) m->emit(m->user,line); }
static void ack_to(M1_Motion *m, const char *session, uint32_t seq, const char *verb,
                   const char *status, const char *reason) {
    char line[180];
    snprintf(line,sizeof(line),"M1M1 ACK session=%s seq=%lu cmd=%s status=%s reason=%s",
             session,(unsigned long)seq,verb,status,reason); output(m,line);
}
static void ack(M1_Motion *m, const char *status, const char *reason) {
    ack_to(m,m->session,m->operation_sequence,m->verb,status,reason);
}
static int fresh(const M1_Motion *m, unsigned slot) {
    unsigned i;
    if(!m->link_ok || m->nodes[slot].valid!=15) return 0;
    for(i=0;i<4;++i) if(m->now-m->nodes[slot].at[i]>FRESH_MS) return 0;
    return 1;
}
static int healthy(const M1_Motion *m, unsigned s) {
    return fresh(m,s) && !m->nodes[s].error && !(m->nodes[s].sw&8u);
}
static int ready(const M1_Motion *m, unsigned s) {
    return healthy(m,s) && m->nodes[s].mode==3 &&
           (m->nodes[s].sw&0x026fu)==0x0227u;
}
static void report(M1_Motion *m) {
    char line[320];
    snprintf(line,sizeof(line),"M1M1 STATE session=%s seq=%lu state=%s configured=%u reason=%s left=%u right=%u lf=%d rf=%d lh=%d rh=%d lsw=%u rsw=%u lm=%u rm=%u le=%u re=%u lv=%ld rv=%ld",
        m->session[0]?m->session:"none",(unsigned long)++m->sequence,state_name(m->state),m->configured,
        m->reason[0]?m->reason:"ok",m->config.nodes[0],m->config.nodes[1],fresh(m,0),fresh(m,1),healthy(m,0),healthy(m,1),
        m->nodes[0].sw,m->nodes[1].sw,m->nodes[0].mode,m->nodes[1].mode,m->nodes[0].error,m->nodes[1].error,
        (long)m->nodes[0].velocity,(long)m->nodes[1].velocity); output(m,line);
}
static void cancel_pending(M1_Motion *m) {
    if(m->flush) m->flush(m->user);
    m->pending=0; m->count=m->cursor=0;
    /* SDO has no transaction ID. Quarantine aborted exchanges before reuse. */
    m->quiet_until=m->now+2*SDO_MS;
}
static void best_effort_stop(M1_Motion *m) {
    unsigned i; M1_Frame f;
    if(!m->configured || !m->send) return;
    for(i=0;i<2;++i) {
        (void)m1_sdo_write(m->config.nodes[i],0x6040,0,M1_CW_QUICK_STOP,2,&f); (void)m->send(m->user,&f);
    }
    for(i=0;i<2;++i) {
        (void)m1_sdo_write(m->config.nodes[i],0x60ff,0,0,4,&f); (void)m->send(m->user,&f);
    }
}
static void fail(M1_Motion *m, const char *reason) {
    if(m->operation) ack(m,"failed",reason);
    m->operation=0; m->moving=0; m->state=M1_FAULT;
    snprintf(m->reason,sizeof(m->reason),"%s",reason);
    cancel_pending(m); best_effort_stop(m); report(m);
}
static void add(M1_Motion *m, unsigned slot, uint16_t index, uint8_t sub,
                 uint8_t size, int write, uint32_t value, uint32_t mask, uint8_t check) {
    M1_MotionOp *o=&m->ops[m->count++];
    o->node=(uint8_t)slot; o->index=index; o->sub=sub; o->size=size;
    o->write=(uint8_t)write; o->value=value; o->mask=mask; o->check=check;
}
static void pair_write(M1_Motion *m, uint16_t index, uint32_t value, uint8_t size) {
    unsigned i; for(i=0;i<2;++i) add(m,i,index,0,size,1,value,0,0);
}
static void pair_state(M1_Motion *m, uint16_t expected) {
    unsigned i; for(i=0;i<2;++i) add(m,i,0x6041,0,2,0,expected,0x006f,1);
}
static void sample_ops(M1_Motion *m) {
    unsigned s;
    for(s=0;s<2;++s) {
        add(m,s,0x6041,0,2,0,0,0,0); add(m,s,0x6061,0,1,0,0,0,0);
        add(m,s,0x1001,0,1,0,0,0,0); add(m,s,0x606c,0,4,0,0,0,0);
    }
}
static void begin(M1_Motion *m, uint32_t sequence, const char *verb) {
    m->operation=1; m->operation_sequence=sequence;
    snprintf(m->verb,sizeof(m->verb),"%s",verb);
    m->operation_at=m->now; m->count=m->cursor=0;
    ack(m,"accepted","pending");
}
static void arm(M1_Motion *m, uint32_t seq) {
    unsigned s;
    begin(m,seq,"ARM"); m->state=M1_ARMING;
    for(s=0;s<2;++s) {
        add(m,s,0x303d,0x0b,2,0,5,0xffff,1);
        add(m,s,0x60a9,0,4,0,m->config.si_velocity,0xffffffffu,1);
        add(m,s,0x6096,1,4,0,m->config.factor_num,0xffffffffu,1);
        add(m,s,0x6096,2,4,0,m->config.factor_den,0xffffffffu,1);
        add(m,s,0x1001,0,1,0,0,0xff,1);
        /* Explicitly start from disabled and stationary, never reset faults. */
        add(m,s,0x6041,0,2,0,0x0040,0x006f,1);
        add(m,s,0x606c,0,4,0,0,0xffffffffu,1);
    }
    pair_write(m,0x60ff,0,4);
    pair_write(m,0x6083,m->config.accel,4);
    pair_write(m,0x6084,m->config.decel,4);
    pair_write(m,0x6085,m->config.quick_decel,4);
    pair_write(m,0x6060,3,1);
    for(s=0;s<2;++s) add(m,s,0x6061,0,1,0,3,0xff,1);
    pair_write(m,0x6040,M1_CW_SHUTDOWN,2); pair_state(m,0x0021);
    pair_write(m,0x6040,M1_CW_SWITCH_ON,2); pair_state(m,0x0023);
    pair_write(m,0x6040,M1_CW_ENABLE_OPERATION,2); pair_state(m,0x0027);
    sample_ops(m);
}
static void stop(M1_Motion *m, uint32_t seq, const char *verb, int disable) {
    unsigned s;
    if(m->operation) ack(m,"failed","superseded_by_stop");
    cancel_pending(m); begin(m,seq,verb); m->state=M1_STOPPING;
    m->stopping_disable=(uint8_t)disable;
    pair_write(m,0x60ff,0,4);
    if(disable) pair_write(m,0x6040,M1_CW_QUICK_STOP,2);
    /* Reported standstill: require both feedback values after the zero writes.
     * This is drive feedback, not an independent physical safety measurement. */
    for(s=0;s<2;++s) {
        add(m,s,0x6041,0,2,0,0x1000,0x1000,1);
        add(m,s,0x606c,0,4,0,0,0xffffffffu,1);
    }
    if(disable) { pair_write(m,0x6040,M1_CW_DISABLE_VOLTAGE,2); pair_state(m,0x0040); }
    sample_ops(m);
}
static void complete(M1_Motion *m) {
    if(!strcmp(m->verb,"ARM")) {
        if(!ready(m,0) || !ready(m,1)) { fail(m,"not_ready"); return; }
        m->state=M1_READY; m->command_at=m->now;
        ack(m,"complete","ready");
    } else if(!strcmp(m->verb,"MOVE")) {
        if(!ready(m,0) || !ready(m,1)) { fail(m,"not_ready"); return; }
        ack(m,"complete","targets_written");
    } else {
        if(!fresh(m,0) || !fresh(m,1) || m->nodes[0].velocity || m->nodes[1].velocity) {
            fail(m,"standstill_unconfirmed"); return;
        }
        m->state=m->stopping_disable?M1_OFF:M1_READY; m->moving=0;
        m->command_at=m->now;
        if(!strcmp(m->verb,"HELLO")) {
            m->configured=0; memset(m->nodes,0,sizeof(m->nodes));
            ack(m,"complete","session_open");
        } else ack(m,"complete","standstill_feedback");
    }
    m->reason[0]=0; m->operation=0; m->count=m->cursor=0; report(m);
}
void m1_motion_init(M1_Motion *m, M1_MotionSend send, M1_MotionEmit emit,
                     M1_MotionFlush flush, void *user) {
    memset(m,0,sizeof(*m)); m->send=send; m->emit=emit; m->flush=flush; m->user=user;
    output(m,"M1M1 BOOT firmware=m1-motion-1 passive=1");
}
void m1_motion_tick(M1_Motion *m, uint32_t now) {
    M1_Frame f; M1_Result r;
    m->now=now;
    if(m->state==M1_READY && ((m->moving && now-m->command_at>=COMMAND_MS) || !ready(m,0) || !ready(m,1))) {
        fail(m,m->moving && now-m->command_at>=COMMAND_MS?"command_timeout":"feedback_lost");
    }
    if(m->operation && now-m->operation_at>=OPERATION_MS) { fail(m,"operation_timeout"); }
    if(m->pending && now-m->sent_at>=SDO_MS) {
        if(m->operation || m->state==M1_READY) fail(m,"sdo_timeout");
        else { m->nodes[m->current.node].valid=0; cancel_pending(m); }
    }
    if(now-m->report_at>=100) { report(m); m->report_at=now; }
    if(!m->configured || !m->link_ok || m->pending || !due(now,m->quiet_until)) return;
    if(m->operation) {
        if(m->cursor==m->count) { complete(m); return; }
        m->current=m->ops[m->cursor];
    } else {
        static const uint16_t indices[]={0x6041,0x6061,0x1001,0x606c};
        static const uint8_t sizes[]={2,1,1,4};
        unsigned c=m->poll_cursor++%8, field=c/2;
        m->current=(M1_MotionOp){indices[field],(uint8_t)(c%2),0,sizes[field],0,0,0,0};
    }
    if(m->current.write)
        r=m1_sdo_write(m->config.nodes[m->current.node],m->current.index,m->current.sub,m->current.value,m->current.size,&f);
    else r=m1_sdo_read(m->config.nodes[m->current.node],m->current.index,m->current.sub,&f);
    if(r!=M1_OK || !m->send || !m->send(m->user,&f)) { fail(m,"can_send_failed"); return; }
    m->pending=1; m->pending_operation=m->operation; m->sent_at=now;
}
void m1_motion_receive(M1_Motion *m, const M1_Frame *f, uint32_t now) {
    M1_SdoReply reply; M1_Result result; M1_MotionOp *o=&m->current;
    M1_MotionNode *n; unsigned field=4, s;
    m->now=now;
    for(s=0;s<2;++s) {
        if(!m->configured) break;
        if(f->id==0x80u+m->config.nodes[s] && f->length==8 && (f->data[0] || f->data[1])) {
            m->nodes[s].valid=0; fail(m,"drive_emcy"); return;
        }
        if(f->id==0x700u+m->config.nodes[s] && f->length==1 && f->data[0]==0) {
            m->nodes[s].valid=0; fail(m,"drive_reboot"); return;
        }
    }
    if(!m->pending) return;
    /* A late reply cannot beat tick's timeout or renew a stale lease. */
    if(now-m->sent_at>=SDO_MS) { m1_motion_tick(m,now); return; }
    result=m1_sdo_reply(f,m->config.nodes[o->node],o->index,o->sub,o->write,&reply);
    if(result==M1_UNRELATED_FRAME) return;
    if(result!=M1_OK || (!o->write && reply.size!=o->size)) {
        if(m->operation || m->state==M1_READY) fail(m,result==M1_SDO_ABORT?"sdo_abort":"bad_sdo");
        else { m->nodes[o->node].valid=0; cancel_pending(m); }
        return;
    }
    m->pending=0; m->quiet_until=now+2;
    n=&m->nodes[o->node];
    if(!o->write && !o->sub) {
        switch(o->index) {
            case 0x6041: n->sw=(uint16_t)reply.value; field=0; break;
            case 0x6061: n->mode=(uint8_t)reply.value; field=1; break;
            case 0x1001: n->error=(uint8_t)reply.value; field=2; break;
            case 0x606c:
                n->velocity=reply.value<=0x7fffffffu?(int32_t)reply.value:-1-(int32_t)(0xffffffffu-reply.value);
                field=3; break;
            default: break;
        }
        if(field<4) { n->at[field]=now; n->valid|=(uint8_t)(1u<<field); }
    }
    if(m->operation && m->pending_operation) {
        if(o->check && (reply.value&o->mask)!=o->value) {
            /* State/zero-speed transitions may take time; immutable admission
             * fingerprints and drive errors must fail immediately. */
            if(o->index==0x6041 || o->index==0x606c || o->index==0x6061) return;
            fail(m,"configuration_mismatch"); return;
        }
        ++m->cursor;
    }
}
void m1_motion_transport(M1_Motion *m, int available) {
    if(!available && m->link_ok && m->configured) {
        m->nodes[0].valid=m->nodes[1].valid=0; fail(m,"can_unavailable");
    }
    m->link_ok=(uint8_t)(available!=0);
}
void m1_motion_input_lost(M1_Motion *m) {
    m->discard=1; m->line_size=0;
    if(m->configured) fail(m,"uart_input_lost");
}
static int session_ok(const char *s) {
    size_t i,n=strlen(s);
    if(n<8 || n>24) return 0;
    for(i=0;i<n;++i) if(!((s[i]>='a' && s[i]<='z') || (s[i]>='A' && s[i]<='Z') ||
        (s[i]>='0' && s[i]<='9') || s[i]=='-')) return 0;
    return 1;
}
static void command(M1_Motion *m, const char *line) {
    char magic[8], session[25], verb[16], extra;
    unsigned long seq;
    int used=0;
    if(sscanf(line,"%7s %24s %lu %15s %n",magic,session,&seq,verb,&used)!=4 ||
       strcmp(magic,"M1M1") || !session_ok(session) || !seq || seq>0xffffffffu) return;
    if(!strcmp(verb,"HELLO") && !line[used]) {
        if(!strcmp(session,m->session)) {
            ack_to(m,session,(uint32_t)seq,verb,"rejected","new_session_required"); return;
        }
        if(m->configured && (m->state!=M1_OFF || m->operation)) {
            if(m->operation) ack(m,"failed","session_change");
            m->operation=0;
            strcpy(m->session,session); m->request_sequence=(uint32_t)seq;
            stop(m,(uint32_t)seq,"HELLO",1); return;
        }
        cancel_pending(m); m->configured=0; memset(m->nodes,0,sizeof(m->nodes));
        strcpy(m->session,session); m->request_sequence=(uint32_t)seq; m->reason[0]=0;
        ack_to(m,session,(uint32_t)seq,verb,"complete","session_open"); return;
    }
    if(strcmp(session,m->session) || seq<=m->request_sequence) {
        ack_to(m,session,(uint32_t)seq,verb,"rejected","stale_request"); return;
    }
    m->request_sequence=(uint32_t)seq;
    if(!strcmp(verb,"STATUS") && !line[used]) {
        report(m); ack_to(m,session,(uint32_t)seq,verb,"complete","ok"); return;
    }
    if((!strcmp(verb,"STOP") || !strcmp(verb,"DISARM") || !strcmp(verb,"ESTOP")) && !line[used]) {
        if(!m->configured) { ack_to(m,session,(uint32_t)seq,verb,"rejected","not_configured"); return; }
        stop(m,(uint32_t)seq,verb,strcmp(verb,"STOP")!=0 || m->state!=M1_READY); return;
    }
    if(m->operation) { ack_to(m,session,(uint32_t)seq,verb,"rejected","busy"); return; }
    if(!strcmp(verb,"CONFIG")) {
        M1_MotionConfig c; unsigned l,r; unsigned long num,den,a,d,q,si,fn,fd; int verified;
        memset(&c,0,sizeof(c));
        if(m->state!=M1_OFF || m->configured) { ack_to(m,session,(uint32_t)seq,verb,"rejected","disarm_required"); return; }
        if(sscanf(line+used,"%u %u %d %d %lu %lu %lf %lu %lu %lu %lu %lu %lu %d %c",
            &l,&r,&c.signs[0],&c.signs[1],&num,&den,&c.limit_rpm,&a,&d,&q,&si,&fn,&fd,&verified,&extra)!=14 ||
            l<1 || l>127 || r<1 || r>127 || l==r || (c.signs[0]!=1 && c.signs[0]!=-1) || (c.signs[1]!=1 && c.signs[1]!=-1) ||
            !num || num>0xffffffffu || !den || den>0xffffffffu || !isfinite(c.limit_rpm) || c.limit_rpm<=0 || c.limit_rpm>4000 ||
            !a || a>0xffffffffu || !d || d>0xffffffffu || !q || q>0xffffffffu || !si || si>0xffffffffu ||
            !fn || fn>0xffffffffu || !fd || fd>0xffffffffu || verified!=1) {
            ack_to(m,session,(uint32_t)seq,verb,"rejected","commissioning_required"); return;
        }
        c.nodes[0]=(uint8_t)l; c.nodes[1]=(uint8_t)r; c.units_num=(uint32_t)num; c.units_den=(uint32_t)den;
        c.accel=(uint32_t)a; c.decel=(uint32_t)d; c.quick_decel=(uint32_t)q; c.si_velocity=(uint32_t)si;
        c.factor_num=(uint32_t)fn; c.factor_den=(uint32_t)fd; c.commissioned=1;
        m->config=c; m->configured=1; m->quiet_until=m->now;
        ack_to(m,session,(uint32_t)seq,verb,"complete","configured"); return;
    }
    if(!m->configured || !m->link_ok) { ack_to(m,session,(uint32_t)seq,verb,"rejected","not_configured"); return; }
    if(!strcmp(verb,"ARM") && !line[used]) {
        if(m->state!=M1_OFF) { ack_to(m,session,(uint32_t)seq,verb,"rejected","disarm_required"); return; }
        arm(m,(uint32_t)seq); return;
    }
    if(!strcmp(verb,"MOVE")) {
        double vx,wz,left,right; int32_t raw[2];
        M1_VelocityScale scale={m->config.units_num,m->config.units_den,1};
        M1_DifferentialConfig geometry={0.739,0.220,0.220,10,10,m->config.limit_rpm,m->config.signs[0],m->config.signs[1],1};
        if(m->state!=M1_READY || !ready(m,0) || !ready(m,1)) { ack_to(m,session,(uint32_t)seq,verb,"rejected","not_ready"); return; }
        if(sscanf(line+used,"%lf %lf %c",&vx,&wz,&extra)!=2 || !isfinite(vx) || !isfinite(wz) ||
            fabs(vx)>0.5 || fabs(wz)>1.0 || m1_differential_rpm(&geometry,vx,wz,&left,&right)!=M1_OK ||
            m1_rpm_to_raw(&scale,left,m->config.limit_rpm,&raw[0])!=M1_OK ||
            m1_rpm_to_raw(&scale,right,m->config.limit_rpm,&raw[1])!=M1_OK) {
            ack_to(m,session,(uint32_t)seq,verb,"rejected","invalid_velocity"); return;
        }
        begin(m,(uint32_t)seq,verb); m->command_at=m->now;
        m->moving=(uint8_t)(vx!=0 || wz!=0);
        add(m,0,0x60ff,0,4,1,(uint32_t)raw[0],0,0);
        add(m,1,0x60ff,0,4,1,(uint32_t)raw[1],0,0); return;
    }
    ack_to(m,session,(uint32_t)seq,verb,"rejected","unsupported");
}
void m1_motion_input(M1_Motion *m, uint8_t b) {
    if(b=='\r' || b=='\n') {
        if(!m->discard && m->line_size) { m->line[m->line_size]=0; command(m,m->line); }
        m->line_size=0; m->discard=0;
    } else if(b<32 || b>126 || m->line_size>=sizeof(m->line)-1) m1_motion_input_lost(m);
    else if(!m->discard) m->line[m->line_size++]=(char)b;
}
int m1_motion_frame_allowed(const M1_Frame *f) {
    uint16_t index; uint8_t cmd;
    if(!f || f->id<0x601 || f->id>0x67f || f->length!=8) return 0;
    cmd=f->data[0]; index=(uint16_t)(f->data[1]|((uint16_t)f->data[2]<<8));
    if(cmd==0x40) return 1;
    if(f->data[3]) return 0;
    if(index==0x6040 && cmd==0x2b) {
        uint16_t cw=(uint16_t)(f->data[4]|((uint16_t)f->data[5]<<8));
        return cw==0 || cw==6 || cw==7 || cw==15 || cw==11;
    }
    if(index==0x6060 && cmd==0x2f) return f->data[4]==3;
    return cmd==0x23 && (index==0x60ff || index==0x6083 || index==0x6084 || index==0x6085);
}
