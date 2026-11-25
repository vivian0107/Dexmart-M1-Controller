#include "m1_canopen.h"

#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void near(double a, double b) { assert(fabs(a - b) < 1e-7); }

static void test_sdo(void) {
    M1_Frame f;
    M1_SdoReply reply;
    const uint8_t read_sw[8] = {0x40, 0x41, 0x60, 0, 0, 0, 0, 0};
    const uint8_t write_mode[8] = {0x2f, 0x60, 0x60, 0, 3, 0, 0, 0};
    const uint8_t write_cw[8] = {0x2b, 0x40, 0x60, 0, 6, 0, 0, 0};
    assert(m1_sdo_read(1, 0x6041, 0, &f) == M1_OK);
    assert(f.id == 0x601 && f.length == 8 && memcmp(f.data, read_sw, 8) == 0);
    assert(m1_sdo_write(2, 0x6060, 0, 3, 1, &f) == M1_OK);
    assert(f.id == 0x602 && memcmp(f.data, write_mode, 8) == 0);
    assert(m1_sdo_write(1, 0x6040, 0, 6, 2, &f) == M1_OK);
    assert(memcmp(f.data, write_cw, 8) == 0);
    assert(m1_sdo_write(1, 0x60ff, 0, (uint32_t)-4000, 4, &f) == M1_OK);
    assert(f.data[0] == 0x23 && f.data[4] == 0x60 && f.data[5] == 0xf0 &&
           f.data[6] == 0xff && f.data[7] == 0xff);
    assert(m1_sdo_write(1, 0x6060, 0, 256, 1, &f) == M1_INVALID_ARGUMENT);
    assert(f.length == 0);
    assert(m1_sdo_write(1, 0x6060, 0, 0, 3, &f) == M1_INVALID_ARGUMENT);
    assert(m1_sdo_read(0, 0x6041, 0, &f) == M1_INVALID_ARGUMENT);
    assert(m1_sdo_read(128, 0x6041, 0, &f) == M1_INVALID_ARGUMENT);
    assert(m1_sdo_read(127, 0x6041, 0, &f) == M1_OK && f.id == 0x67f);

    f = (M1_Frame){0x581, 8, {0x4b, 0x41, 0x60, 0, 0x27, 2, 0xaa, 0xbb}};
    assert(m1_sdo_reply(&f, 1, 0x6041, 0, 0, &reply) == M1_OK);
    assert(reply.size == 2 && reply.value == 0x0227 && reply.abort_code == 0);
    assert(m1_sdo_reply(&f, 2, 0x6041, 0, 0, &reply) == M1_UNRELATED_FRAME);
    assert(m1_sdo_reply(&f, 1, 0x6040, 0, 0, &reply) == M1_UNRELATED_FRAME);
    assert(m1_sdo_reply(&f, 1, 0x6041, 1, 0, &reply) == M1_UNRELATED_FRAME);
    f.length = 7;
    assert(m1_sdo_reply(&f, 1, 0x6041, 0, 0, &reply) == M1_MALFORMED_FRAME);
    f.length = 8;
    f.data[0] = 0x60;
    assert(m1_sdo_reply(&f, 1, 0x6041, 0, 1, &reply) == M1_OK);
    assert(m1_sdo_reply(&f, 1, 0x6041, 0, 0, &reply) == M1_MALFORMED_FRAME);
    f.data[0] = 0x41;
    assert(m1_sdo_reply(&f, 1, 0x6041, 0, 0, &reply) == M1_SEGMENTED_TRANSFER_REQUIRED);
    f.data[0] = 0x80;
    f.data[4] = 0;
    f.data[5] = 0;
    f.data[6] = 2;
    f.data[7] = 6;
    assert(m1_sdo_reply(&f, 1, 0x6041, 0, 0, &reply) == M1_SDO_ABORT);
    assert(reply.abort_code == 0x06020000 && reply.size == 0 && reply.value == 0);
    f.data[0] = 0x4f;
    f.data[4] = 3;
    assert(m1_sdo_reply(&f, 1, 0x6041, 0, 0, &reply) == M1_OK);
    assert(reply.value == 3 && reply.size == 1);
    f.data[0] = 0x43;
    assert(m1_sdo_reply(&f, 1, 0x6041, 0, 0, &reply) == M1_OK && reply.size == 4);
    f.data[0] = 0x47; /* Unexpected 24-bit object. */
    assert(m1_sdo_reply(&f, 1, 0x6041, 0, 0, &reply) == M1_MALFORMED_FRAME);
    f.data[0] = 0x42; /* Expedited but unspecified width must not be guessed. */
    assert(m1_sdo_reply(&f, 1, 0x6041, 0, 0, &reply) == M1_MALFORMED_FRAME);
}

static void test_pdo_and_state(void) {
    M1_Frame f;
    M1_StatusPosition status;
    M1_VelocityInputs velocity;
    const uint32_t wrong_map[3] = {0x30510108, 0x30510210, 0x30510310};
    const uint8_t target[8] = {0x60, 0xf0, 0xff, 0xff, 0xa0, 0x0f, 0, 0};
    assert(m1_check_mapping(m1_rpdo1_map, 3, m1_rpdo1_map, 3) == M1_OK);
    assert(m1_check_mapping(wrong_map, 3, m1_rpdo1_map, 3) == M1_MAPPING_MISMATCH);
    assert(m1_check_mapping(m1_tpdo1_map, 2, m1_tpdo1_map, 3) == M1_MAPPING_MISMATCH);
    assert(m1_rpdo1(1, M1_CW_ENABLE_OPERATION, M1_PV_MODE, 0, &f) == M1_OK);
    assert(f.id == 0x201 && f.length == 7 && f.data[0] == 0x0f && f.data[2] == 3);
    assert(m1_rpdo2(2, -4000, 4000, &f) == M1_OK);
    assert(f.id == 0x302 && f.length == 8 && memcmp(f.data, target, 8) == 0);
    assert(m1_rpdo3(2, 100, 200, &f) == M1_OK);
    assert(f.id == 0x402 && f.data[0] == 100 && f.data[4] == 200);
    assert(m1_nmt(1, M1_NMT_PRE_OPERATIONAL, &f) == M1_OK);
    assert(f.id == 0 && f.length == 2 && f.data[0] == 0x80 && f.data[1] == 1);
    assert(m1_nmt(0, M1_NMT_START, &f) == M1_INVALID_ARGUMENT);
    assert(m1_nmt(1, (M1_NmtCommand)129, &f) == M1_INVALID_ARGUMENT);
    assert(m1_sync(&f) == M1_OK && f.id == 0x80 && f.length == 0);
    f = (M1_Frame){0x181, 7, {0x27, 2, 3, 0, 0, 0, 0x80, 0}};
    assert(m1_tpdo1(&f, 1, &status) == M1_OK);
    assert(status.position_raw == INT32_MIN && status.mode == 3);
    assert(m1_is_remote_pv_enabled(&status));
    status.statusword &= (uint16_t)~0x200u;
    assert(!m1_is_remote_pv_enabled(&status));
    status.statusword = 0x227;
    status.mode = 9;
    assert(!m1_is_remote_pv_enabled(&status));
    f.length = 8; /* A different layout must not silently be decoded. */
    assert(m1_tpdo1(&f, 1, &status) == M1_MALFORMED_FRAME);
    f = (M1_Frame){0x282, 8, {0xff, 0xff, 0xff, 0xff, 0x78, 0x56, 0x34, 0x12}};
    assert(m1_tpdo2(&f, 2, &velocity) == M1_OK);
    assert(velocity.velocity_raw == -1 && velocity.digital_inputs == 0x12345678);
    assert(m1_tpdo2(&f, 1, &velocity) == M1_UNRELATED_FRAME);
    assert(m1_drive_state(0) == M1_STATE_NOT_READY);
    assert(m1_drive_state(0x40) == M1_STATE_SWITCH_ON_DISABLED);
    assert(m1_drive_state(0x21) == M1_STATE_READY_TO_SWITCH_ON);
    assert(m1_drive_state(0x23) == M1_STATE_SWITCHED_ON);
    assert(m1_drive_state(0x27) == M1_STATE_OPERATION_ENABLED);
    assert(m1_drive_state(0x07) == M1_STATE_QUICK_STOP_ACTIVE);
    assert(m1_drive_state(0x0f) == M1_STATE_FAULT_REACTION);
    assert(m1_drive_state(0x08) == M1_STATE_FAULT);
    assert(m1_drive_state(0x01) == M1_STATE_UNKNOWN);
    assert(m1_drive_state(0xa6b7) == M1_STATE_OPERATION_ENABLED); /* Optional flags. */
}

static void test_units_and_differential(void) {
    M1_VelocityScale scale = {10, 1, 1}; /* Synthetic verified scale for this test. */
    M1_DifferentialConfig c = {0.6, 0.1, 0.1, 10, 10, 4000, 1, 1, 1};
    int32_t raw;
    double rpm, left, right;
    assert(m1_rpm_to_raw(&scale, -4000, 4000, &raw) == M1_OK && raw == -40000);
    assert(m1_raw_to_rpm(&scale, raw, &rpm) == M1_OK);
    near(rpm, -4000);
    assert(m1_rpm_to_raw(&scale, 4001, 4000, &raw) == M1_OUT_OF_RANGE && raw == 0);
    assert(m1_rpm_to_raw(&scale, 1, 4001, &raw) == M1_INVALID_ARGUMENT);
    assert(m1_rpm_to_raw(&scale, NAN, 4000, &raw) == M1_INVALID_ARGUMENT);
    scale.verified = 0;
    assert(m1_rpm_to_raw(&scale, 1, 4000, &raw) == M1_INVALID_ARGUMENT);
    scale.verified = 1;
    scale.units_numerator = 1;
    scale.rpm_denominator = 6;
    assert(m1_rpm_to_raw(&scale, 4000, 4000, &raw) == M1_OK && raw == 666);
    assert(m1_rpm_to_raw(&scale, -4000, 4000, &raw) == M1_OK && raw == -666);
    assert(m1_raw_to_rpm(&scale, raw, &rpm) == M1_OK);
    near(rpm, -3996);
    scale.rpm_denominator = 1;
    scale.units_numerator = UINT32_MAX;
    assert(m1_rpm_to_raw(&scale, 4000, 4000, &raw) == M1_OUT_OF_RANGE);
    assert(m1_differential_rpm(&c, 1, 0, &left, &right) == M1_OK);
    near(left, 600 / 0.62831853071795864769);
    near(left, right);
    assert(m1_differential_rpm(&c, -1, 0, &left, &right) == M1_OK && left < 0 && right < 0);
    assert(m1_differential_rpm(&c, 0, 1, &left, &right) == M1_OK && left < 0 && right > 0);
    near(left, -right);
    assert(m1_differential_rpm(&c, 0, -1, &left, &right) == M1_OK && left > 0 && right < 0);
    assert(m1_differential_rpm(&c, 100, 20, &left, &right) == M1_OK);
    near(right, 4000);
    near(left / right, 94.0 / 106.0); /* Joint scaling preserves curvature. */
    c.left_sign = -1;
    assert(m1_differential_rpm(&c, 1, 0, &left, &right) == M1_OK);
    near(left, -right);
    c.left_sign = 1;
    c.right_radius_m = 0.2;
    assert(m1_differential_rpm(&c, 1, 0, &left, &right) == M1_OK);
    near(left, 2 * right);
    c.motor_limit_rpm = 100;
    assert(m1_differential_rpm(&c, 100, 0, &left, &right) == M1_OK);
    near(left, 100);
    near(right, 50);
    c.geometry_verified = 0;
    assert(m1_differential_rpm(&c, 1, 0, &left, &right) == M1_INVALID_ARGUMENT);
    near(left, 0); near(right, 0);
    c.geometry_verified = 1;
    c.track_m = 0;
    assert(m1_differential_rpm(&c, 1, 0, &left, &right) == M1_INVALID_ARGUMENT);
    c.track_m = 0.6;
    c.motor_limit_rpm = 4001;
    assert(m1_differential_rpm(&c, 1, 0, &left, &right) == M1_INVALID_ARGUMENT);
    c.motor_limit_rpm = 4000;
    assert(m1_differential_rpm(&c, INFINITY, 0, &left, &right) == M1_INVALID_ARGUMENT);
    assert(m1_differential_rpm(&c, DBL_MAX, 0, &left, &right) == M1_OUT_OF_RANGE);
}

static void test_supplied_vehicle_geometry(void) {
    /* User-supplied nominal dimensions; verification here is test-only. */
    const M1_DifferentialConfig c = {0.739, 0.220, 0.220, 10, 10, 4000, 1, 1, 1};
    double left, right;
    assert(m1_differential_rpm(&c, 1, 0, &left, &right) == M1_OK);
    near(left, 434.0589357051691);
    near(right, left);
    assert(m1_differential_rpm(&c, 0, 1, &left, &right) == M1_OK);
    near(left, -160.38477674306);
    near(right, 160.38477674306);
    assert(m1_differential_rpm(&c, 0, -1, &left, &right) == M1_OK);
    near(left, 160.38477674306);
    near(right, -160.38477674306);
    /* Reconstruct body motion from unequal wheel targets, including 10:1 gear. */
    assert(m1_differential_rpm(&c, 0.5, 0.4, &left, &right) == M1_OK);
    left *= 6.28318530717958647692 * 0.220 / 600.0;
    right *= 6.28318530717958647692 * 0.220 / 600.0;
    near((left + right) / 2.0, 0.5);
    near((right - left) / 0.739, 0.4);
}

int main(void) {
    test_sdo();
    test_pdo_and_state();
    test_units_and_differential();
    test_supplied_vehicle_geometry();
    puts("M1 codec, state, verified units and differential tests passed");
    return 0;
}
