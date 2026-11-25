#include "m1_diagnostics.h"

#include <stdio.h>
#include <string.h>

typedef struct { uint16_t index; uint8_t sub, size; } ReadObject;
/* size=0 is a bounded identity string. Read RAM 01-11, never write it. */
static const ReadObject objects[] = {
    {0x1008,0,0}, {0x1009,0,0}, {0x100a,0,0},
    {0x1000,0,4}, {0x1001,0,1}, {0x1002,0,4}, {0x1017,0,2},
    {0x3001,11,2}, {0x303d,11,2}, /* 01-11: persisted and RAM control mode. */
    {0x3009,0x13,2}, {0x3009,0x16,2}, {0x3009,0x20,2}, /* ID, baud, CAN selection. */
    {0x6040,0,2}, {0x6041,0,2}, {0x6060,0,1}, {0x6061,0,1},
    {0x6064,0,4}, {0x606c,0,4}, {0x60fd,0,4},
    {0x6080,0,4}, {0x607f,0,4}, {0x6083,0,4}, {0x6084,0,4}, {0x6085,0,4},
    {0x608f,1,4}, {0x608f,2,4}, {0x6091,1,4}, {0x6091,2,4},
    {0x6092,1,4}, {0x6092,2,4}, {0x6096,1,4}, {0x6096,2,4},
    {0x6097,1,4}, {0x6097,2,4}, {0x60a8,0,4}, {0x60a9,0,4}, {0x60aa,0,4},
    {0x1400,1,4}, {0x1400,2,1}, {0x1401,1,4}, {0x1401,2,1},
    {0x1402,1,4}, {0x1402,2,1},
    {0x1800,1,4}, {0x1800,2,1}, {0x1800,5,2},
    {0x1801,1,4}, {0x1801,2,1}, {0x1801,5,2},
    {0x1600,0,1}, {0x1600,1,4}, {0x1600,2,4}, {0x1600,3,4},
    {0x1601,0,1}, {0x1601,1,4}, {0x1601,2,4},
    {0x1602,0,1}, {0x1602,1,4}, {0x1602,2,4},
    {0x1a00,0,1}, {0x1a00,1,4}, {0x1a00,2,4}, {0x1a00,3,4},
    {0x1a01,0,1}, {0x1a01,1,4}, {0x1a01,2,4}
};
static const ReadObject poll_objects[] = {
    {0x1001,0,1}, {0x6041,0,2}, {0x6061,0,1}, {0x606c,0,4}
};
typedef struct { uint16_t index; uint8_t sub; uint32_t value; } MappingEntry;
static const MappingEntry mapping[] = {
    {0x1600,0,3}, {0x1600,1,0x60400010}, {0x1600,2,0x60600008}, {0x1600,3,0x607a0020},
    {0x1601,0,2}, {0x1601,1,0x60ff0020}, {0x1601,2,0x60810020},
    {0x1602,0,2}, {0x1602,1,0x60830020}, {0x1602,2,0x60840020},
    {0x1a00,0,3}, {0x1a00,1,0x60410010}, {0x1a00,2,0x60610008}, {0x1a00,3,0x60640020},
    {0x1a01,0,2}, {0x1a01,1,0x606c0020}, {0x1a01,2,0x60fd0020}
};
static const char *mapping_state(const M1_DiagNode *n) {
    if(n->mapping_mismatch) return "mismatch";
    return n->mapping_seen==((1u<<(sizeof(mapping)/sizeof(mapping[0])))-1u)?"match":"unknown";
}

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static int due(uint32_t now, uint32_t deadline) { return now-deadline < 0x80000000u; }
static int fresh(uint32_t now, uint32_t at) { return now-at <= M1_DIAG_FRESH_MS; }
static void emit(M1_Diagnostics *d, const char *s) { if (d->emit) d->emit(d->user,s); }
static int whitelisted(uint16_t index, uint8_t sub) {
    size_t i;
    for(i=0;i<sizeof(objects)/sizeof(objects[0]);++i)
        if(objects[i].index==index && objects[i].sub==sub) return 1;
    return 0;
}
int m1_diag_frame_allowed(const M1_Frame *f) {
    size_t i;
    if(!f || f->length!=8 || f->id<0x601 || f->id>0x67f) return 0;
    if(f->data[0]==0x60 || f->data[0]==0x70) {
        for(i=1;i<8;++i) if(f->data[i]) return 0;
        return 1;
    }
    if(!whitelisted((uint16_t)(f->data[1]|((uint16_t)f->data[2]<<8)),f->data[3])) return 0;
    if(f->data[0]==0x80) return u32(f->data+4)==0x05040000u;
    if(f->data[0]!=0x40) return 0;
    return u32(f->data+4)==0;
}
static int send(M1_Diagnostics *d, const M1_Frame *f) {
    if(!d->transport_ok || !m1_diag_frame_allowed(f) || !d->send || !d->send(d->user,f)) {
        ++d->tx_failed;
        return 0;
    }
    d->sent_at=d->now;
    return 1;
}
static void invalidate(M1_Diagnostics *d) {
    M1_DiagNode *n=&d->nodes[d->slot];
    if(d->object_index==0x1001) n->error_valid=0;
    if(d->object_index==0x6041) n->status_valid=0;
    if(d->object_index==0x6061) n->mode_valid=0;
    if(d->object_index==0x606c) n->velocity_valid=0;
}
static void abort_upload(M1_Diagnostics *d) {
    M1_Frame f;
    if(!d->pending) return;
    memset(&f,0,sizeof(f));
    f.id=(uint16_t)(0x600+d->nodes[d->slot].node); f.length=8;
    f.data[0]=0x80; f.data[1]=(uint8_t)d->object_index;
    f.data[2]=(uint8_t)(d->object_index>>8); f.data[3]=d->object_sub;
    f.data[6]=4; f.data[7]=5; /* Client abort: SDO protocol timeout. No OD write. */
    (void)send(d,&f);
}
static void failure(M1_Diagnostics *d, const char *reason, uint32_t code) {
    char line[180];
    snprintf(line,sizeof(line),"M1D1 READ node=%u index=0x%04X sub=%u result=%s code=0x%08lX",
             d->nodes[d->slot].node,d->object_index,d->object_sub,reason,(unsigned long)code);
    emit(d,line); invalidate(d); d->pending=0; d->segment=0; d->quiet_until=d->now+20;
}
static void start_read(M1_Diagnostics *d, uint8_t slot, ReadObject o) {
    M1_Frame f;
    d->slot=slot; d->object_index=o.index; d->object_sub=o.sub; d->expected_size=o.size;
    d->segment=0; d->toggle=0; d->text_length=0; d->text_size_known=0;
    d->announced_size=0;
    (void)m1_sdo_read(d->nodes[slot].node,o.index,o.sub,&f);
    if(send(d,&f)) d->pending=1;
    else { failure(d,"tx_failed",0); d->nodes[slot].retry_at=d->now+1000; d->nodes[slot].retry_delayed=1; }
}
static int segment_request(M1_Diagnostics *d) {
    M1_Frame f;
    memset(&f,0,sizeof(f)); f.id=(uint16_t)(0x600+d->nodes[d->slot].node); f.length=8;
    f.data[0]=(uint8_t)(0x60 | (d->toggle<<4));
    if(send(d,&f)) return 1;
    failure(d,"tx_failed",0); return 0;
}
static void numeric(M1_Diagnostics *d, uint32_t value, uint8_t size) {
    M1_DiagNode *n=&d->nodes[d->slot];
    char line[180];
    size_t i;
    if(d->expected_size!=size) { failure(d,"wrong_size",size); return; }
    n->seen=1; n->seen_at=d->now; n->retry_delayed=0;
    for(i=0;i<sizeof(mapping)/sizeof(mapping[0]);++i) {
        if(mapping[i].index==d->object_index && mapping[i].sub==d->object_sub) {
            n->mapping_seen|=1u<<i;
            if(value!=mapping[i].value) n->mapping_mismatch=1;
        }
    }
    switch(d->object_index) {
        case 0x1001: n->error_register=(uint8_t)value; n->error_valid=1; n->error_at=d->now; break;
        case 0x6041: n->statusword=(uint16_t)value; n->status_valid=1; n->status_at=d->now; break;
        case 0x6061: n->mode=(uint8_t)value; n->mode_valid=1; n->mode_at=d->now; break;
        case 0x606c:
            n->velocity_raw=value<=INT32_MAX?(int32_t)value:-1-(int32_t)(UINT32_MAX-value);
            n->velocity_valid=1; n->velocity_at=d->now; break;
        default: break;
    }
    snprintf(line,sizeof(line),"M1D1 READ node=%u index=0x%04X sub=%u result=ok size=%u value=0x%08lX",
             n->node,d->object_index,d->object_sub,size,(unsigned long)value);
    emit(d,line); d->pending=0; d->quiet_until=d->now+5;
}
static void identity(M1_Diagnostics *d) {
    static const char hex[]="0123456789ABCDEF";
    char encoded[M1_DIAG_TEXT_CAPACITY*2+1], line[340];
    uint8_t i;
    for(i=0;i<d->text_length;++i) { encoded[2*i]=hex[d->text[i]>>4]; encoded[2*i+1]=hex[d->text[i]&15]; }
    encoded[2*d->text_length]=0;
    d->nodes[d->slot].seen=1; d->nodes[d->slot].seen_at=d->now;
    d->nodes[d->slot].retry_delayed=0;
    snprintf(line,sizeof(line),"M1D1 READ node=%u index=0x%04X sub=%u result=ok size=%u text_hex=%s",
             d->nodes[d->slot].node,d->object_index,d->object_sub,d->text_length,encoded);
    emit(d,line); d->pending=0; d->segment=0; d->quiet_until=d->now+5;
}
void m1_diag_receive(M1_Diagnostics *d, const M1_Frame *f, uint32_t now) {
    uint8_t i, cmd, size;
    if(!d || !f) return;
    d->now=now;
    for(i=0;i<M1_DIAG_NODE_COUNT;++i) {
        M1_DiagNode *n=&d->nodes[i];
        if(f->id==0x700+n->node && f->length==1 &&
           (f->data[0]==0 || f->data[0]==4 || f->data[0]==5 || f->data[0]==127)) {
            n->seen=1; n->seen_at=now; n->heartbeat=f->data[0]; n->heartbeat_valid=1;
            if(f->data[0]==0) {
                n->error_valid=n->status_valid=n->mode_valid=0; n->emcy=0;
                n->velocity_valid=0;
                n->mapping_seen=0; n->mapping_mismatch=0;
                if(d->pending && d->slot==i) failure(d,"node_reboot",0);
            }
            return;
        }
        if(f->id==0x80+n->node && f->length==8) {
            n->seen=1; n->seen_at=now; n->emcy=(uint16_t)(f->data[0]|((uint16_t)f->data[1]<<8)); n->emcy_at=now;
            if(n->emcy) n->error_valid=n->status_valid=0;
            return;
        }
    }
    if(d->pending && f->id==0x580+d->nodes[d->slot].node && now-d->sent_at>=M1_DIAG_SDO_TIMEOUT_MS) {
        m1_diag_tick(d,now); /* Late replies must not renew status freshness. */
        ++d->ignored; return;
    }
    if(!d->pending || f->id!=0x580+d->nodes[d->slot].node) { ++d->ignored; return; }
    if(f->length!=8) { ++d->malformed; return; }
    cmd=f->data[0];
    if(cmd==0x80) {
        if(f->data[1]!=(uint8_t)d->object_index || f->data[2]!=(uint8_t)(d->object_index>>8) || f->data[3]!=d->object_sub) {
            ++d->ignored; return;
        }
        d->nodes[d->slot].seen=1; d->nodes[d->slot].seen_at=now;
        ++d->nodes[d->slot].aborts; failure(d,"abort",u32(f->data+4)); return;
    }
    if(d->segment) {
        if((cmd&0xe0)!=0 || ((cmd>>4)&1)!=d->toggle) {
            ++d->malformed; abort_upload(d); failure(d,"bad_segment",0); return;
        }
        size=(uint8_t)(7-((cmd>>1)&7));
        if((!(cmd&1) && size!=7) || d->text_length+size>M1_DIAG_TEXT_CAPACITY) {
            abort_upload(d); failure(d,"invalid_length",0); return;
        }
        memcpy(d->text+d->text_length,f->data+1,size); d->text_length=(uint8_t)(d->text_length+size);
        if(d->text_size_known && d->text_length>d->announced_size) {
            abort_upload(d); failure(d,"length_mismatch",0); return;
        }
        if(cmd&1) {
            if(d->text_size_known && d->text_length!=d->announced_size) failure(d,"length_mismatch",0);
            else identity(d);
        } else { d->toggle^=1; (void)segment_request(d); }
        return;
    }
    if(f->data[1]!=(uint8_t)d->object_index || f->data[2]!=(uint8_t)(d->object_index>>8) || f->data[3]!=d->object_sub) {
        ++d->ignored; return;
    }
    if((cmd&0xe0)!=0x40 || (cmd&0x10)) { ++d->malformed; failure(d,"bad_reply",cmd); return; }
    if(cmd&2) {
        if(!(cmd&1)) { failure(d,"missing_size",cmd); return; }
        size=(uint8_t)(4-((cmd>>2)&3));
        if(d->expected_size) {
            uint32_t value=u32(f->data+4);
            if(size<4) value&=(1u<<(8*size))-1u;
            numeric(d,value,size);
        } else { memcpy(d->text,f->data+4,size); d->text_length=size; identity(d); }
    } else {
        if(d->expected_size || (cmd&0x0c)) { failure(d,"unexpected_segmented",cmd); return; }
        d->text_size_known=(uint8_t)(cmd&1); d->announced_size=u32(f->data+4);
        if(d->text_size_known && d->announced_size>M1_DIAG_TEXT_CAPACITY) {
            abort_upload(d); failure(d,"too_long",d->announced_size); return;
        }
        d->segment=1; d->toggle=0; (void)segment_request(d);
    }
}

static void report(M1_Diagnostics *d) {
    uint8_t i;
    for(i=0;i<M1_DIAG_NODE_COUNT;++i) {
        M1_DiagNode *n=&d->nodes[i];
        const char *link="unknown", *health="unknown", *reason="not_sampled";
        unsigned ready=0;
        char sw[12]="NA", er[12]="NA", mode[12]="NA", velocity[16]="NA", line[480];
        if(n->seen) link=fresh(d->now,n->seen_at)?"online":"offline";
        else if(n->timeouts) link="offline";
        if(!d->transport_ok) { link="offline"; health="unhealthy"; reason="can_unavailable"; }
        else if(!strcmp(link,"offline")) { health="unhealthy"; reason="no_response"; }
        else if(n->emcy && !(n->error_valid && n->status_valid &&
                due(n->error_at,n->emcy_at) && due(n->status_at,n->emcy_at))) {
            health="unhealthy"; reason="emcy";
        } else if(!n->error_valid || !n->status_valid || !fresh(d->now,n->error_at) || !fresh(d->now,n->status_at)) {
            reason="status_unavailable";
        } else if(n->error_register || (n->statusword&8)) { health="unhealthy"; reason="drive_fault"; }
        else if(n->statusword&0x80) { health="impaired"; reason="drive_warning"; }
        else if(n->mapping_mismatch) { health="impaired"; reason="mapping_mismatch"; }
        else {
            health="healthy"; reason="not_enabled";
            if(m1_drive_state(n->statusword)==M1_STATE_UNKNOWN) { health="unknown"; reason="unknown_state"; }
            else if(!(n->statusword&0x200)) reason="local_control";
            else if(!n->mode_valid || !fresh(d->now,n->mode_at)) reason="mode_unknown";
            else if(n->mode!=M1_PV_MODE) reason="not_pv_mode";
            else if(m1_drive_state(n->statusword)==M1_STATE_OPERATION_ENABLED) { ready=1; reason="pv_enabled"; }
            else if(m1_drive_state(n->statusword)==M1_STATE_QUICK_STOP_ACTIVE) reason="quick_stop_active";
        }
        if(n->status_valid) snprintf(sw,sizeof(sw),"0x%04X",n->statusword);
        if(n->error_valid) snprintf(er,sizeof(er),"0x%02X",n->error_register);
        if(n->mode_valid) snprintf(mode,sizeof(mode),"%d",n->mode<=127?(int)n->mode:(int)n->mode-256);
        if(n->velocity_valid && fresh(d->now,n->velocity_at)) snprintf(velocity,sizeof(velocity),"%ld",(long)n->velocity_raw);
        snprintf(line,sizeof(line),"M1D1 HEALTH seq=%lu node=%u uptime_ms=%lu link=%s health=%s reason=%s status=%s error=%s mode=%s mapping=%s drive_ready=%u motion_allowed=0 velocity_raw=%s age_ms=%lu status_age_ms=%lu error_age_ms=%lu timeouts=%lu aborts=%lu",
                 (unsigned long)++d->sequence,n->node,(unsigned long)d->now,link,health,reason,sw,er,mode,mapping_state(n),ready,
                 velocity,
                 (unsigned long)(n->seen?d->now-n->seen_at:UINT32_MAX),
                 (unsigned long)(n->status_valid?d->now-n->status_at:UINT32_MAX),
                 (unsigned long)(n->error_valid?d->now-n->error_at:UINT32_MAX),
                 (unsigned long)n->timeouts,(unsigned long)n->aborts);
        emit(d,line);
    }
}
void m1_diag_transport(M1_Diagnostics *d, int ok) {
    uint8_t i;
    if(!d) return;
    d->transport_ok=(uint8_t)(ok!=0);
    if(!ok) {
        if(d->pending) failure(d,"can_unavailable",0);
        for(i=0;i<2;++i) d->nodes[i].error_valid=d->nodes[i].status_valid=d->nodes[i].mode_valid=d->nodes[i].velocity_valid=0;
    }
}
void m1_diag_init(M1_Diagnostics *d, M1_DiagSend tx, M1_DiagEmit log, void *user) {
    memset(d,0,sizeof(*d)); d->send=tx; d->emit=log; d->user=user;
    d->nodes[0].node=1; d->nodes[1].node=2;
    emit(d,"M1D1 BOOT firmware=m1-diag-1 read_only=1 motion_allowed=0");
    emit(d,"M1D1 INFO can_bps=1000000 uart_bps=115200 default_nodes=1,2 watch=off");
}
void m1_diag_tick(M1_Diagnostics *d, uint32_t now) {
    unsigned tries;
    d->now=now;
    if(now-d->report_at>=500) { report(d); d->report_at=now; }
    if(d->pending) {
        if(now-d->sent_at>=M1_DIAG_SDO_TIMEOUT_MS) {
            uint8_t slot=d->slot;
            abort_upload(d); ++d->nodes[slot].timeouts;
            d->nodes[slot].retry_at=now+1000; d->nodes[slot].retry_delayed=1;
            failure(d,"timeout",0);
            if(d->probing && d->probe_slot==slot) { ++d->probe_slot; d->probe_entry=0; }
        }
        return;
    }
    if(!d->transport_ok || !due(now,d->quiet_until)) return;
    if(d->watching && now-d->poll_at>=40) {
        for(tries=0;tries<8;++tries) {
            uint8_t cursor=d->poll_cursor++;
            uint8_t slot=(uint8_t)(cursor%2), field=(uint8_t)((cursor/2)%4);
            M1_DiagNode *n=&d->nodes[slot];
            if(n->retry_delayed && !due(now,n->retry_at)) continue;
            d->poll_at=now; start_read(d,slot,poll_objects[field]); return;
        }
    }
    if(d->probing) {
        while(d->probe_slot<2) {
            M1_DiagNode *n=&d->nodes[d->probe_slot];
            if(n->retry_delayed && !due(now,n->retry_at)) { ++d->probe_slot; d->probe_entry=0; continue; }
            if(d->probe_entry<sizeof(objects)/sizeof(objects[0])) {
                start_read(d,d->probe_slot,objects[d->probe_entry++]); return;
            }
            ++d->probe_slot; d->probe_entry=0;
        }
        d->probing=0;
        for(tries=0;tries<2;++tries) {
            char line[140];
            snprintf(line,sizeof(line),"M1D1 PROFILE node=%u mapping=%s units_verified=0 motion_allowed=0",
                     d->nodes[tries].node,mapping_state(&d->nodes[tries]));
            emit(d,line);
        }
        emit(d,"M1D1 PROBE result=complete means=scan_finished_not_motion_ready");
    }
}
static void cancel(M1_Diagnostics *d) {
    abort_upload(d); d->pending=0; d->segment=0; d->watching=0; d->probing=0; d->quiet_until=d->now+250;
}
static const char *skip_space(const char *p) { while(*p==' ' || *p=='\t') ++p; return p; }
static int node_number(const char **p, uint8_t *out) {
    unsigned value=0;
    *p=skip_space(*p);
    if(**p<'0' || **p>'9') return 0;
    while(**p>='0' && **p<='9') { value=value*10+(unsigned)(**p-'0'); if(value>127) return 0; ++*p; }
    if(!value) return 0;
    *out=(uint8_t)value; return 1;
}
static void command(M1_Diagnostics *d, const char *s) {
    if(!strcmp(s,"HELP")) {
        emit(d,"M1D1 HELP commands=INFO,STATUS,PROBE,WATCH_ON,WATCH_OFF,NODES_a_b,CANCEL read_only=1");
    } else if(!strcmp(s,"INFO")) {
        emit(d,"M1D1 INFO firmware=m1-diag-1 can_bps=1000000 uart_bps=115200 read_only=1 motion_allowed=0");
    } else if(!strcmp(s,"STATUS")) report(d);
    else if(!strcmp(s,"PROBE")) {
        unsigned i;
        if(d->probing) { emit(d,"M1D1 ERROR reason=probe_busy"); return; }
        for(i=0;i<2;++i) { d->nodes[i].mapping_seen=0; d->nodes[i].mapping_mismatch=0; }
        d->probe_slot=0; d->probe_entry=0; d->probing=1;
        emit(d,"M1D1 PROBE result=started");
    } else if(!strcmp(s,"WATCH ON")) { d->watching=1; emit(d,"M1D1 WATCH enabled=1"); }
    else if(!strcmp(s,"WATCH OFF") || !strcmp(s,"CANCEL")) { cancel(d); emit(d,"M1D1 WATCH enabled=0"); }
    else if(!strncmp(s,"NODES ",6)) {
        const char *p=s+6;
        uint8_t a,b;
        if(!node_number(&p,&a) || (*p!=' ' && *p!='\t') || !node_number(&p,&b) || *skip_space(p) || a==b) {
            emit(d,"M1D1 ERROR reason=nodes_require_two_distinct_ids_1_to_127"); return;
        }
        cancel(d); memset(d->nodes,0,sizeof(d->nodes)); d->nodes[0].node=a; d->nodes[1].node=b;
        emit(d,"M1D1 NODES result=changed_watch_disabled"); report(d);
    } else emit(d,"M1D1 ERROR reason=unsupported_command_read_only");
}
void m1_diag_discard_input(M1_Diagnostics *d) { d->command_length=0; d->discard_line=1; }
void m1_diag_input(M1_Diagnostics *d, uint8_t byte) {
    if(byte=='\r' || byte=='\n') {
        if(d->discard_line) { d->discard_line=0; emit(d,"M1D1 ERROR reason=invalid_or_overflowed_line"); }
        else if(d->command_length) {
            d->command[d->command_length]=0; command(d,d->command);
        }
        d->command_length=0; return;
    }
    if(d->discard_line) return;
    if((byte<32 && byte!='\t') || byte>126 || d->command_length>=sizeof(d->command)-1) {
        m1_diag_discard_input(d); return;
    }
    d->command[d->command_length++]=(char)byte;
}
