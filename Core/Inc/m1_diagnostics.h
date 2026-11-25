#ifndef M1_DIAGNOSTICS_H
#define M1_DIAGNOSTICS_H

#include "m1_canopen.h"

#define M1_DIAG_NODE_COUNT 2
#define M1_DIAG_SDO_TIMEOUT_MS 200u
#define M1_DIAG_FRESH_MS 1000u
#define M1_DIAG_TEXT_CAPACITY 96u

typedef int (*M1_DiagSend)(void *user, const M1_Frame *frame);
typedef void (*M1_DiagEmit)(void *user, const char *line);

typedef struct {
    uint8_t node, seen, error_valid, status_valid, mode_valid, velocity_valid;
    uint8_t error_register, mode, heartbeat, heartbeat_valid;
    uint16_t statusword, emcy;
    uint32_t seen_at, error_at, status_at, mode_at, velocity_at, emcy_at, retry_at;
    uint32_t timeouts, aborts;
    uint32_t mapping_seen;
    uint8_t mapping_mismatch;
    uint8_t retry_delayed;
    int32_t velocity_raw;
} M1_DiagNode;

typedef struct {
    void *user;
    M1_DiagSend send;
    M1_DiagEmit emit;
    M1_DiagNode nodes[M1_DIAG_NODE_COUNT];
    uint32_t now, report_at, poll_at, quiet_until, sent_at, sequence;
    uint32_t ignored, malformed, tx_failed;
    uint16_t object_index;
    uint8_t object_sub, expected_size, pending, segment, toggle, slot;
    uint8_t text_size_known, text_length;
    uint32_t announced_size;
    uint8_t text[M1_DIAG_TEXT_CAPACITY];
    uint8_t watching, probing, probe_slot, probe_entry, poll_cursor, transport_ok;
    uint8_t pending_probe, probe_skipped;
    uint16_t probe_ok, probe_failed;
    char probe_id[33];
    char command[80];
    uint8_t command_length, discard_line;
} M1_Diagnostics;

void m1_diag_init(M1_Diagnostics *d, M1_DiagSend send, M1_DiagEmit emit, void *user);
void m1_diag_tick(M1_Diagnostics *d, uint32_t now_ms);
void m1_diag_receive(M1_Diagnostics *d, const M1_Frame *frame, uint32_t now_ms);
void m1_diag_input(M1_Diagnostics *d, uint8_t byte);
void m1_diag_discard_input(M1_Diagnostics *d);
void m1_diag_transport(M1_Diagnostics *d, int available);
/* Final HAL transmit gate: SDO uploads/segment requests/client aborts only. */
int m1_diag_frame_allowed(const M1_Frame *frame);

#endif
