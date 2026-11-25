#include "m1_canopen.h"

#include <limits.h>
#include <math.h>
#include <string.h>

const uint32_t m1_rpdo1_map[3] = {0x60400010u, 0x60600008u, 0x607a0020u};
const uint32_t m1_rpdo2_map[2] = {0x60ff0020u, 0x60810020u};
const uint32_t m1_tpdo1_map[3] = {0x60410010u, 0x60610008u, 0x60640020u};
const uint32_t m1_tpdo2_map[2] = {0x606c0020u, 0x60fd0020u};

static int valid_node(uint8_t node) { return node >= 1 && node <= 127; }
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t get_signed32(const uint8_t *p) {
    const uint32_t v = get32(p);
    if (v <= INT32_MAX) return (int32_t)v;
    return -1 - (int32_t)(UINT32_MAX - v);
}
static M1_Result frame_init(uint8_t node, uint16_t base, uint8_t length,
                            M1_Frame *out) {
    if (out == NULL) return M1_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!valid_node(node)) return M1_INVALID_ARGUMENT;
    out->id = (uint16_t)(base + node);
    out->length = length;
    return M1_OK;
}
static M1_Result match_frame(const M1_Frame *f, uint8_t node, uint16_t base,
                             uint8_t length) {
    if (f == NULL || !valid_node(node)) return M1_INVALID_ARGUMENT;
    if (f->id != (uint16_t)(base + node)) return M1_UNRELATED_FRAME;
    if (f->length != length) return M1_MALFORMED_FRAME;
    return M1_OK;
}

M1_Result m1_check_mapping(const uint32_t *actual, size_t count,
                          const uint32_t *expected, size_t expected_count) {
    if (actual == NULL || expected == NULL) return M1_INVALID_ARGUMENT;
    if (count != expected_count || count > 8) return M1_MAPPING_MISMATCH;
    return memcmp(actual, expected, count * sizeof(*actual)) == 0
               ? M1_OK : M1_MAPPING_MISMATCH;
}

M1_Result m1_sdo_read(uint8_t node, uint16_t index, uint8_t sub, M1_Frame *out) {
    M1_Result r = frame_init(node, 0x600, 8, out);
    if (r != M1_OK) return r;
    out->data[0] = 0x40;
    put16(&out->data[1], index);
    out->data[3] = sub;
    return M1_OK;
}

M1_Result m1_sdo_write(uint8_t node, uint16_t index, uint8_t sub,
                      uint32_t value, uint8_t size, M1_Frame *out) {
    M1_Result r = frame_init(node, 0x600, 8, out);
    if (r != M1_OK) return r;
    if ((size != 1 && size != 2 && size != 4) ||
        (size == 1 && value > UINT8_MAX) || (size == 2 && value > UINT16_MAX)) {
        memset(out, 0, sizeof(*out));
        return M1_INVALID_ARGUMENT;
    }
    out->data[0] = (uint8_t)(0x23u | ((4u - size) << 2));
    put16(&out->data[1], index);
    out->data[3] = sub;
    put32(&out->data[4], value);
    return M1_OK;
}

M1_Result m1_sdo_reply(const M1_Frame *f, uint8_t node, uint16_t index,
                      uint8_t sub, int download, M1_SdoReply *out) {
    M1_Result r;
    uint8_t cmd, size;
    if (out == NULL || (download != 0 && download != 1)) return M1_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    r = match_frame(f, node, 0x580, 8);
    if (r != M1_OK) return r;
    if (get16(&f->data[1]) != index || f->data[3] != sub) return M1_UNRELATED_FRAME;
    cmd = f->data[0];
    if (cmd == 0x80) {
        out->abort_code = get32(&f->data[4]);
        return M1_SDO_ABORT;
    }
    if (download) return cmd == 0x60 ? M1_OK : M1_MALFORMED_FRAME;
    if ((cmd & 0xe0u) != 0x40u || (cmd & 0x10u) != 0) return M1_MALFORMED_FRAME;
    if ((cmd & 0x02u) == 0) return M1_SEGMENTED_TRANSFER_REQUIRED;
    /* Require an explicit byte count; never guess a response's object type. */
    if ((cmd & 1u) == 0) return M1_MALFORMED_FRAME;
    size = (uint8_t)(4u - ((cmd >> 2) & 3u));
    if (size != 1 && size != 2 && size != 4) return M1_MALFORMED_FRAME;
    out->size = size;
    out->value = get32(&f->data[4]);
    if (size < 4) out->value &= ((1u << (size * 8u)) - 1u);
    return M1_OK;
}

M1_Result m1_nmt(uint8_t node, M1_NmtCommand command, M1_Frame *out) {
    if (out == NULL) return M1_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    /* Deliberately disallow broadcast and reset commands in this motion codec. */
    if (!valid_node(node) || (command != M1_NMT_START && command != M1_NMT_STOP &&
                             command != M1_NMT_PRE_OPERATIONAL)) return M1_INVALID_ARGUMENT;
    out->length = 2;
    out->data[0] = (uint8_t)command;
    out->data[1] = node;
    return M1_OK;
}

M1_Result m1_sync(M1_Frame *out) {
    if (out == NULL) return M1_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->id = 0x80;
    return M1_OK;
}

M1_Result m1_rpdo1(uint8_t node, uint16_t cw, int8_t mode,
                  int32_t pos, M1_Frame *out) {
    M1_Result r = frame_init(node, 0x200, 7, out);
    if (r != M1_OK) return r;
    put16(out->data, cw);
    out->data[2] = (uint8_t)mode;
    put32(&out->data[3], (uint32_t)pos);
    return M1_OK;
}
M1_Result m1_rpdo2(uint8_t node, int32_t target, uint32_t profile, M1_Frame *out) {
    M1_Result r = frame_init(node, 0x300, 8, out);
    if (r != M1_OK) return r;
    put32(out->data, (uint32_t)target);
    put32(&out->data[4], profile);
    return M1_OK;
}
M1_Result m1_rpdo3(uint8_t node, uint32_t accel, uint32_t decel, M1_Frame *out) {
    M1_Result r = frame_init(node, 0x400, 8, out);
    if (r != M1_OK) return r;
    put32(out->data, accel);
    put32(&out->data[4], decel);
    return M1_OK;
}
M1_Result m1_tpdo1(const M1_Frame *f, uint8_t node, M1_StatusPosition *out) {
    M1_Result r;
    if (out == NULL) return M1_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    r = match_frame(f, node, 0x180, 7);
    if (r != M1_OK) return r;
    out->statusword = get16(f->data);
    out->mode = f->data[2] <= INT8_MAX ? (int8_t)f->data[2]
               : (int8_t)(-1 - (int)(UINT8_MAX - f->data[2]));
    out->position_raw = get_signed32(&f->data[3]);
    return M1_OK;
}
M1_Result m1_tpdo2(const M1_Frame *f, uint8_t node, M1_VelocityInputs *out) {
    M1_Result r;
    if (out == NULL) return M1_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    r = match_frame(f, node, 0x280, 8);
    if (r != M1_OK) return r;
    out->velocity_raw = get_signed32(f->data);
    out->digital_inputs = get32(&f->data[4]);
    return M1_OK;
}

M1_DriveState m1_drive_state(uint16_t sw) {
    switch (sw & 0x004fu) {
        case 0x0000: return M1_STATE_NOT_READY;
        case 0x0040: return M1_STATE_SWITCH_ON_DISABLED;
        case 0x000f: return M1_STATE_FAULT_REACTION;
        case 0x0008: return M1_STATE_FAULT;
        default: break;
    }
    switch (sw & 0x006fu) {
        case 0x0021: return M1_STATE_READY_TO_SWITCH_ON;
        case 0x0023: return M1_STATE_SWITCHED_ON;
        case 0x0027: return M1_STATE_OPERATION_ENABLED;
        case 0x0007: return M1_STATE_QUICK_STOP_ACTIVE;
        default: return M1_STATE_UNKNOWN;
    }
}
int m1_is_remote_pv_enabled(const M1_StatusPosition *f) {
    return f != NULL && f->mode == M1_PV_MODE && (f->statusword & 0x0200u) != 0 &&
           m1_drive_state(f->statusword) == M1_STATE_OPERATION_ENABLED;
}

static int valid_scale(const M1_VelocityScale *s) {
    return s != NULL && s->verified == 1 && s->units_numerator > 0 && s->rpm_denominator > 0;
}
M1_Result m1_rpm_to_raw(const M1_VelocityScale *s, double rpm,
                        double limit, int32_t *out) {
    double raw;
    if (out == NULL) return M1_INVALID_ARGUMENT;
    *out = 0;
    if (!valid_scale(s) || !isfinite(rpm) || !isfinite(limit) ||
        limit <= 0 || limit > M1_MOTOR_MAX_RPM) return M1_INVALID_ARGUMENT;
    if (fabs(rpm) > limit) return M1_OUT_OF_RANGE;
    /* Quantize toward zero: nearest rounding could turn a 4000 rpm request
     * into 4002 rpm when, for example, one raw unit represents 6 rpm. */
    raw = trunc(rpm * (double)s->units_numerator / (double)s->rpm_denominator);
    if (!isfinite(raw) || raw < INT32_MIN || raw > INT32_MAX) return M1_OUT_OF_RANGE;
    *out = (int32_t)raw;
    return M1_OK;
}
M1_Result m1_raw_to_rpm(const M1_VelocityScale *s, int32_t raw, double *out) {
    if (out == NULL) return M1_INVALID_ARGUMENT;
    *out = 0;
    if (!valid_scale(s)) return M1_INVALID_ARGUMENT;
    *out = (double)raw * (double)s->rpm_denominator / (double)s->units_numerator;
    return M1_OK;
}

static int positive(double x) { return isfinite(x) && x > 0; }
M1_Result m1_differential_rpm(const M1_DifferentialConfig *c,
                             double vx, double wz, double *left, double *right) {
    double l, r, peak, factor;
    const double two_pi = 6.28318530717958647692;
    if (left == NULL || right == NULL || left == right) return M1_INVALID_ARGUMENT;
    *left = *right = 0;
    if (c == NULL || c->geometry_verified != 1 || !isfinite(vx) || !isfinite(wz) ||
        !positive(c->track_m) || !positive(c->left_radius_m) || !positive(c->right_radius_m) ||
        !positive(c->left_motor_per_wheel_rev) || !positive(c->right_motor_per_wheel_rev) ||
        !positive(c->motor_limit_rpm) || c->motor_limit_rpm > M1_MOTOR_MAX_RPM ||
        (c->left_sign != -1 && c->left_sign != 1) ||
        (c->right_sign != -1 && c->right_sign != 1)) return M1_INVALID_ARGUMENT;
    l = (vx - wz * c->track_m / 2.0) * 60.0 * c->left_motor_per_wheel_rev /
        (two_pi * c->left_radius_m);
    r = (vx + wz * c->track_m / 2.0) * 60.0 * c->right_motor_per_wheel_rev /
        (two_pi * c->right_radius_m);
    if (!isfinite(l) || !isfinite(r)) return M1_OUT_OF_RANGE;
    peak = fmax(fabs(l), fabs(r));
    factor = peak > c->motor_limit_rpm ? c->motor_limit_rpm / peak : 1.0;
    *left = l * factor * c->left_sign;
    *right = r * factor * c->right_sign;
    return M1_OK;
}
