#ifndef M1_MOTION_H
#define M1_MOTION_H
#include "m1_canopen.h"

/* M1M1: separate, opt-in motion application. No defaults authorize movement.
 * CiA402 SDO control avoids reliance on unverified PDO mappings.
 * All calls run on the main loop. Call tick at least every 10 ms.
 */
typedef int (*M1_MotionSend)(void *, const M1_Frame *);
typedef void (*M1_MotionEmit)(void *, const char *);
typedef void (*M1_MotionFlush)(void *);
typedef struct {
    uint8_t nodes[2];
    int signs[2];
    uint32_t units_num, units_den, accel, decel, quick_decel;
    uint32_t si_velocity, factor_num, factor_den;
    double limit_rpm;
    int commissioned;
} M1_MotionConfig;
typedef enum { M1_OFF, M1_ARMING, M1_READY, M1_STOPPING, M1_FAULT } M1_MotionState;
typedef struct {
    uint16_t sw;
    uint8_t mode, error, valid;
    int32_t velocity;
    uint32_t at[4]; /* status, mode, error, velocity; independent leases */
} M1_MotionNode;
typedef struct {
    uint16_t index;
    uint8_t node, sub, size, write, check;
    uint32_t value, mask;
} M1_MotionOp;
typedef struct {
    void *user;
    M1_MotionSend send;
    M1_MotionEmit emit;
    M1_MotionFlush flush;
    M1_MotionConfig config;
    M1_MotionNode nodes[2];
    M1_MotionState state;
    M1_MotionOp ops[80], current;
    unsigned count, cursor, poll_cursor;
    uint32_t now, sent_at, quiet_until, operation_at, command_at, report_at, sequence;
    uint32_t request_sequence, operation_sequence;
    uint8_t configured, pending, pending_operation, operation, moving, stopping_disable, link_ok;
    char session[25], verb[16], reason[40], line[256];
    uint16_t line_size;
    uint8_t discard;
} M1_Motion;
void m1_motion_init(M1_Motion *, M1_MotionSend, M1_MotionEmit, M1_MotionFlush, void *);
void m1_motion_tick(M1_Motion *, uint32_t);
void m1_motion_receive(M1_Motion *, const M1_Frame *, uint32_t);
void m1_motion_input(M1_Motion *, uint8_t);
void m1_motion_transport(M1_Motion *, int);
void m1_motion_input_lost(M1_Motion *);
int m1_motion_frame_allowed(const M1_Frame *);
#endif
