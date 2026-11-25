#include "m1_diagnostics.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    M1_Diagnostics d;
    M1_Frame last;
    unsigned sends;
    int fail_tx;
    char log[100000];
} Fixture;
static Fixture fixture;
static int tx(void *user, const M1_Frame *f) {
    Fixture *x=user;
    assert(m1_diag_frame_allowed(f));
    x->last=*f; ++x->sends; return !x->fail_tx;
}
static void log_line(void *user, const char *s) {
    Fixture *x=user;
    assert(strlen(x->log)+strlen(s)+2<sizeof(x->log));
    strcat(x->log,s); strcat(x->log,"\n");
}
static Fixture *reset(void) {
    Fixture *x=&fixture;
    memset(x,0,sizeof(*x)); m1_diag_init(&x->d,tx,log_line,x); m1_diag_transport(&x->d,1); return x;
}
static void cmd(Fixture *x, const char *s) { while(*s) m1_diag_input(&x->d,(uint8_t)*s++); m1_diag_input(&x->d,'\n'); }
static M1_Frame reply(Fixture *x, uint8_t command, uint32_t value) {
    M1_Frame f={(uint16_t)(0x580+x->d.nodes[x->d.slot].node),8,{0}};
    f.data[0]=command; f.data[1]=(uint8_t)x->d.object_index;
    f.data[2]=(uint8_t)(x->d.object_index>>8); f.data[3]=x->d.object_sub;
    f.data[4]=(uint8_t)value; f.data[5]=(uint8_t)(value>>8);
    f.data[6]=(uint8_t)(value>>16); f.data[7]=(uint8_t)(value>>24); return f;
}
static void test_read_only_and_parser(void) {
    Fixture *x=reset(); M1_Frame f={0x601,8,{0x2b,0x40,0x60,0,6,0,0,0}};
    unsigned i;
    m1_diag_tick(&x->d,1000); assert(!x->sends);
    cmd(x,"ACTIVATE"); cmd(x,"MOVE 1 0 0 0 0 0"); cmd(x,"READ 300A 00"); assert(!x->sends);
    assert(!m1_diag_frame_allowed(&f)); f.id=0; assert(!m1_diag_frame_allowed(&f));
    f.id=0x601; f.data[0]=0x40; f.data[1]=0x0a; f.data[2]=0x30; f.data[4]=0;
    assert(!m1_diag_frame_allowed(&f)); /* No read of write-only maintenance commands. */
    f.data[1]=0x41; f.data[2]=0x60; assert(m1_diag_frame_allowed(&f));
    f.data[4]=1; assert(!m1_diag_frame_allowed(&f));
    f.data[0]=0x60; assert(!m1_diag_frame_allowed(&f));
    memset(f.data+1,0,7); assert(m1_diag_frame_allowed(&f));
    for(i=0;i<100;++i) m1_diag_input(&x->d,'X');
    cmd(x,"WATCH ON"); assert(!x->d.watching); /* Tail of oversized line is discarded. */
    cmd(x,"NODES 0 2"); cmd(x,"NODES 1 1"); cmd(x,"NODES 1 128"); cmd(x,"NODES 1 2 junk");
    assert(x->d.nodes[0].node==1 && x->d.nodes[1].node==2);
    cmd(x,"NODES 10 11"); assert(x->d.nodes[0].node==10 && x->d.nodes[1].node==11);
    cmd(x,"WATCH ON"); m1_diag_tick(&x->d,1300); assert(x->sends==1 && x->last.id==0x60a);
    cmd(x,"CANCEL"); assert(!x->d.pending && !x->d.watching && x->last.data[0]==0x80);
}
static void test_segmented_identity(void) {
    Fixture *x=reset(); M1_Frame f;
    cmd(x,"PROBE"); m1_diag_tick(&x->d,0);
    assert(x->last.id==0x601 && x->last.data[0]==0x40 && x->last.data[1]==8 && x->last.data[2]==0x10);
    f=reply(x,0x41,9); m1_diag_receive(&x->d,&f,1); assert(x->last.data[0]==0x60);
    f=(M1_Frame){0x581,8,{0,'D','E','X','M','A','R','T'}};
    m1_diag_receive(&x->d,&f,2); assert(x->last.data[0]==0x70);
    f=(M1_Frame){0x581,8,{0x1b,' ','M',0,0,0,0,0}};
    m1_diag_receive(&x->d,&f,3); assert(!x->d.pending);
    assert(strstr(x->log,"text_hex=4445584D415254204D"));
    x=reset(); cmd(x,"PROBE"); m1_diag_tick(&x->d,0);
    f=reply(x,0x41,97); m1_diag_receive(&x->d,&f,1);
    assert(!x->d.pending && x->last.data[0]==0x80 && strstr(x->log,"result=too_long"));
    x=reset(); cmd(x,"PROBE"); m1_diag_tick(&x->d,0);
    f=reply(x,0x41,8); m1_diag_receive(&x->d,&f,1);
    f=(M1_Frame){0x581,8,{0x10,'b','a','d',0,0,0,0}}; /* Wrong toggle. */
    m1_diag_receive(&x->d,&f,2); assert(!x->d.pending && strstr(x->log,"bad_segment"));
}
static void test_timeout_abort_and_correlation(void) {
    Fixture *x=reset(); M1_Frame f;
    cmd(x,"WATCH ON"); m1_diag_tick(&x->d,40);
    f=reply(x,0x4f,0); ++f.data[3]; m1_diag_receive(&x->d,&f,50);
    assert(x->d.pending && !x->d.nodes[0].seen && x->d.ignored==1);
    m1_diag_tick(&x->d,240); assert(!x->d.pending && x->d.nodes[0].timeouts==1);
    assert(x->last.data[0]==0x80);
    m1_diag_tick(&x->d,500); assert(strstr(x->log,"link=offline health=unhealthy reason=no_response"));
    x=reset(); cmd(x,"WATCH ON"); m1_diag_tick(&x->d,40);
    f=reply(x,0x80,0x06020000); m1_diag_receive(&x->d,&f,50);
    assert(!x->d.pending && x->d.nodes[0].seen && !x->d.nodes[0].error_valid);
    assert(strstr(x->log,"result=abort code=0x06020000"));
    x=reset(); cmd(x,"WATCH ON"); m1_diag_tick(&x->d,40);
    f=reply(x,0x4f,0); m1_diag_receive(&x->d,&f,241);
    assert(!x->d.pending && !x->d.nodes[0].error_valid && !x->d.nodes[0].seen);
    assert(x->d.nodes[0].timeouts==1 && x->d.ignored==1);
    x=reset(); x->fail_tx=1; cmd(x,"WATCH ON"); m1_diag_tick(&x->d,40);
    assert(!x->d.pending && x->d.tx_failed==1 && strstr(x->log,"result=tx_failed"));
}
static void run_poll(Fixture *x, uint32_t start, uint32_t end, uint16_t sw, uint8_t error) {
    uint32_t now;
    for(now=start;now<end;now+=10) {
        m1_diag_tick(&x->d,now);
        if(x->d.pending) {
            uint32_t value=0; uint8_t command=0x43;
            M1_Frame f;
            if(x->d.object_index==0x1001) { value=error; command=0x4f; }
            if(x->d.object_index==0x6041) { value=sw; command=0x4b; }
            if(x->d.object_index==0x6061) { value=3; command=0x4f; }
            f=reply(x,command,value); m1_diag_receive(&x->d,&f,now);
        }
    }
}
static void test_live_health(void) {
    Fixture *x=reset(); M1_Frame f;
    cmd(x,"WATCH ON"); run_poll(x,0,1000,0x0221,0);
    assert(strstr(x->log,"health=healthy reason=not_enabled"));
    assert(!strstr(x->log,"drive_ready=1"));
    x->log[0]=0; run_poll(x,1000,2000,0x0227,0); cmd(x,"STATUS");
    assert(strstr(x->log,"health=healthy reason=pv_enabled"));
    assert(strstr(x->log,"drive_ready=1 motion_allowed=0"));
    f=(M1_Frame){0x81,8,{0x10,0x23,1,0,0,0,0,0}};
    m1_diag_receive(&x->d,&f,1990); x->log[0]=0; cmd(x,"STATUS");
    assert(strstr(x->log,"node=1 uptime_ms=1990 link=online health=unhealthy reason=emcy"));
    x->log[0]=0; run_poll(x,2000,3000,0x0227,1); cmd(x,"STATUS");
    assert(strstr(x->log,"health=unhealthy reason=drive_fault"));
    x->log[0]=0; run_poll(x,3000,4000,0x02a7,0); cmd(x,"STATUS");
    assert(strstr(x->log,"health=impaired reason=drive_warning"));
    x->log[0]=0; m1_diag_transport(&x->d,0); cmd(x,"STATUS");
    assert(strstr(x->log,"health=unhealthy reason=can_unavailable"));
    m1_diag_transport(&x->d,1); x->log[0]=0; cmd(x,"STATUS");
    assert(strstr(x->log,"health=unknown reason=status_unavailable"));
    run_poll(x,4000,5000,0x0227,0);
    f=(M1_Frame){0x701,1,{0}}; m1_diag_receive(&x->d,&f,5000);
    x->log[0]=0; cmd(x,"STATUS");
    assert(strstr(x->log,"node=1 uptime_ms=5000 link=online health=unknown"));
    cmd(x,"WATCH OFF"); m1_diag_tick(&x->d,7000); x->log[0]=0;
    f=(M1_Frame){0x701,1,{5}}; m1_diag_receive(&x->d,&f,7000); cmd(x,"STATUS");
    assert(strstr(x->log,"node=1 uptime_ms=7000 link=online health=unknown"));
    assert(strstr(x->log,"node=2 uptime_ms=7000 link=offline health=unhealthy"));
}
static uint32_t map_value(uint16_t index, uint8_t sub) {
    switch(index) {
        case 0x1600: return sub?m1_rpdo1_map[sub-1]:3;
        case 0x1601: return sub?m1_rpdo2_map[sub-1]:2;
        case 0x1602: return !sub?2:sub==1?0x60830020u:0x60840020u;
        case 0x1a00: return sub?m1_tpdo1_map[sub-1]:3;
        case 0x1a01: return sub?m1_tpdo2_map[sub-1]:2;
        default: return 0;
    }
}
static void test_profile_scan(void) {
    unsigned bad;
    for(bad=0;bad<3;++bad) {
        Fixture *x=reset(); uint32_t now;
        cmd(x,"REQ scan-1 PROBE");
        assert(strstr(x->log,"ACK id=scan-1 cmd=PROBE status=accepted reason=pending"));
        cmd(x,"REQ other INFO");
        for(now=0;now<3000 && x->d.probing;now+=5) {
            m1_diag_tick(&x->d,now);
            if(x->d.pending) {
                uint8_t size=x->d.expected_size?x->d.expected_size:2;
                uint32_t value=map_value(x->d.object_index,x->d.object_sub);
                M1_Frame f;
                if(bad==1 && x->d.object_index==0x1601 && x->d.object_sub==1) value=0x30510108;
                f=reply(x,(uint8_t)(0x43|((4-size)<<2)),value);
                if(bad==2 && x->d.object_index==0x608f && x->d.object_sub==1) f=reply(x,0x80,0x06020000);
                m1_diag_receive(&x->d,&f,now);
            }
        }
        assert(!x->d.probing);
        assert(strstr(x->log,bad==1?"PROFILE node=1 mapping=mismatch":"PROFILE node=1 mapping=match"));
        assert(strstr(x->log,bad==1?"ACK id=scan-1 cmd=PROBE status=complete reason=profile_mismatch":
                             bad==2?"ACK id=scan-1 cmd=PROBE status=complete reason=partial":
                                    "ACK id=scan-1 cmd=PROBE status=complete reason=ok"));
    }
}
static void test_command_acknowledgements(void) {
    Fixture *x=reset();
    cmd(x,"REQ p1 PING"); assert(strstr(x->log,"ACK id=p1 cmd=PING status=complete reason=ok"));
    cmd(x,"REQ invalid$ PING"); assert(strstr(x->log,"ACK id=none cmd=REQ status=rejected reason=invalid_request"));
    cmd(x,"REQ move-1 ACTIVATE");
    assert(strstr(x->log,"ACK id=move-1 cmd=ACTIVATE status=rejected reason=read_only"));
    cmd(x,"HOLD aabb"); assert(strstr(x->log,"ACK id=aabb cmd=HOLD status=rejected reason=read_only"));
    cmd(x,"STOP_ID ccdd"); assert(strstr(x->log,"ACK id=ccdd cmd=STOP_ID status=rejected reason=read_only"));
    cmd(x,"ESTOP_ID eeff"); assert(strstr(x->log,"ACK id=eeff cmd=ESTOP_ID status=rejected reason=read_only"));
    assert(x->sends==0);
    assert(!strstr(x->log,"APPLIED") && !strstr(x->log,"Drive is ready"));
    cmd(x,"REQ scan PROBE"); cmd(x,"REQ again PROBE");
    assert(strstr(x->log,"ACK id=again cmd=PROBE status=rejected reason=busy"));
    cmd(x,"REQ c1 CANCEL");
    assert(strstr(x->log,"ACK id=scan cmd=PROBE status=cancelled reason=requested"));
    assert(strstr(x->log,"ACK id=c1 cmd=CANCEL status=complete reason=ok"));
    x=reset(); cmd(x,"REQ offline PROBE");
    for(unsigned now=0;now<1000;now+=10) m1_diag_tick(&x->d,now);
    assert(strstr(x->log,"ACK id=offline cmd=PROBE status=failed reason=no_valid_reads"));
    x=reset(); m1_diag_transport(&x->d,0); cmd(x,"REQ bad WATCH ON");
    assert(strstr(x->log,"ACK id=bad cmd=WATCH status=failed reason=can_unavailable"));
}
int main(int argc, char **argv) {
    if(argc==2 && !strcmp(argv[1],"--emit-ack")) {
        Fixture *x=reset(); unsigned now;
        cmd(x,"REQ link-1 INFO"); cmd(x,"REQ link-2 NODES 1 2");
        cmd(x,"REQ link-3 WATCH ON"); cmd(x,"REQ link-4 PROBE");
        for(now=0;now<6000 && x->d.probing;now+=10) {
            m1_diag_tick(&x->d,now);
            if(x->d.pending) {
                uint8_t size=x->d.expected_size?x->d.expected_size:2;
                uint32_t value=map_value(x->d.object_index,x->d.object_sub);
                M1_Frame f;
                if(x->d.object_index==0x6041) value=0x0221;
                if(x->d.object_index==0x6061) value=3;
                f=reply(x,(uint8_t)(0x43|((4-size)<<2)),value);
                m1_diag_receive(&x->d,&f,now);
            }
        }
        assert(!x->d.probing); puts(x->log); return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--emit-health")) {
        Fixture *x=reset();
        cmd(x,"WATCH ON"); run_poll(x,0,1000,0x0221,0);
        run_poll(x,1000,2000,0x0228,1);
        cmd(x,"WATCH OFF"); m1_diag_tick(&x->d,4000);
        puts(x->log);
        return 0;
    }
    test_read_only_and_parser(); test_segmented_identity();
    test_timeout_abort_and_correlation(); test_live_health();
    test_profile_scan();
    test_command_acknowledgements();
    puts("Read-only diagnostic SDO, console, segmentation and live health tests passed");
    return 0;
}
