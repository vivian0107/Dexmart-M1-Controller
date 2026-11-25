#ifndef M1_CANOPEN_H
#define M1_CANOPEN_H

/* DEXMART CANopen Rev 8, pp.9-18, 32-35, 101-113, 135-137.
 * Codec only: never transmits, enables a drive, or changes EEPROM.
 * CiA402 CAN mapping must be read back and verified before using PDO codecs.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M1_MOTOR_MAX_RPM 4000.0 /* User-supplied motor maximum, not a bench target. */
#define M1_PV_MODE 3
#define M1_CW_DISABLE_VOLTAGE 0x0000u
#define M1_CW_SHUTDOWN 0x0006u
#define M1_CW_SWITCH_ON 0x0007u
#define M1_CW_ENABLE_OPERATION 0x000fu
#define M1_CW_QUICK_STOP 0x000bu
#define M1_CW_HALT 0x010fu

typedef enum {
    M1_OK = 0,
    M1_INVALID_ARGUMENT,
    M1_UNRELATED_FRAME,
    M1_MALFORMED_FRAME,
    M1_SDO_ABORT,
    M1_SEGMENTED_TRANSFER_REQUIRED,
    M1_MAPPING_MISMATCH,
    M1_OUT_OF_RANGE
} M1_Result;

/* Standard 11-bit DATA frames only. HAL binding must reject extended/RTR/FD. */
typedef struct {
    uint16_t id;
    uint8_t length;
    uint8_t data[8];
} M1_Frame;

typedef enum {
    M1_NMT_START = 1,
    M1_NMT_STOP = 2,
    M1_NMT_PRE_OPERATIONAL = 128
} M1_NmtCommand;

typedef struct {
    uint32_t value;
    uint32_t abort_code;
    uint8_t size;
} M1_SdoReply;

typedef enum {
    M1_STATE_NOT_READY,
    M1_STATE_SWITCH_ON_DISABLED,
    M1_STATE_READY_TO_SWITCH_ON,
    M1_STATE_SWITCHED_ON,
    M1_STATE_OPERATION_ENABLED,
    M1_STATE_QUICK_STOP_ACTIVE,
    M1_STATE_FAULT_REACTION,
    M1_STATE_FAULT,
    M1_STATE_UNKNOWN
} M1_DriveState;

typedef struct {
    uint16_t statusword;
    int8_t mode;
    int32_t position_raw;
} M1_StatusPosition;

typedef struct {
    int32_t velocity_raw;
    uint32_t digital_inputs;
} M1_VelocityInputs;

/* These four maps are the CiA402 CAN layout, NOT the EtherCAT or MultiCMD maps. */
extern const uint32_t m1_rpdo1_map[3];
extern const uint32_t m1_rpdo2_map[2];
extern const uint32_t m1_tpdo1_map[3];
extern const uint32_t m1_tpdo2_map[2];
M1_Result m1_check_mapping(const uint32_t *actual, size_t actual_count,
                          const uint32_t *expected, size_t expected_count);

M1_Result m1_sdo_read(uint8_t node, uint16_t index, uint8_t sub, M1_Frame *out);
M1_Result m1_sdo_write(uint8_t node, uint16_t index, uint8_t sub,
                      uint32_t value, uint8_t size, M1_Frame *out);
/* download=1 expects a write ACK; download=0 expects an expedited upload.
 * Correlates node/index/subindex. Strings (1008/1009/100A) may need segmented SDO.
 * Caller serializes SDO exchanges and handles timeouts/retries/stale replies.
 */
M1_Result m1_sdo_reply(const M1_Frame *frame, uint8_t node, uint16_t index,
                      uint8_t sub, int download, M1_SdoReply *out);
M1_Result m1_nmt(uint8_t node, M1_NmtCommand command, M1_Frame *out);
M1_Result m1_sync(M1_Frame *out);
M1_Result m1_rpdo1(uint8_t node, uint16_t controlword, int8_t mode,
                  int32_t position_raw, M1_Frame *out);
M1_Result m1_rpdo2(uint8_t node, int32_t target_velocity_raw,
                  uint32_t profile_velocity_raw, M1_Frame *out);
M1_Result m1_rpdo3(uint8_t node, uint32_t acceleration_raw,
                  uint32_t deceleration_raw, M1_Frame *out);
M1_Result m1_tpdo1(const M1_Frame *frame, uint8_t node, M1_StatusPosition *out);
M1_Result m1_tpdo2(const M1_Frame *frame, uint8_t node, M1_VelocityInputs *out);
M1_DriveState m1_drive_state(uint16_t statusword);
int m1_is_remote_pv_enabled(const M1_StatusPosition *feedback);

/* Explicit, bench-verified raw CAN units per MOTOR rpm. No implicit rpm or mm/s.
 * Does not configure Factor Group; caller must verify the actual drive settings.
 * Command quantization is toward zero to avoid exceeding the requested rpm.
 */
typedef struct {
    uint32_t units_numerator;
    uint32_t rpm_denominator;
    int verified;
} M1_VelocityScale;
M1_Result m1_rpm_to_raw(const M1_VelocityScale *scale, double motor_rpm,
                        double configured_limit_rpm, int32_t *out);
M1_Result m1_raw_to_rpm(const M1_VelocityScale *scale, int32_t raw, double *out);

typedef struct {
    double track_m;
    double left_radius_m;
    double right_radius_m;
    double left_motor_per_wheel_rev;
    double right_motor_per_wheel_rev;
    double motor_limit_rpm; /* >0 and <=4000; set a lower initial bench limit. */
    int left_sign;
    int right_sign;
    int geometry_verified;
} M1_DifferentialConfig;
M1_Result m1_differential_rpm(const M1_DifferentialConfig *config,
                             double vx_mps, double wz_radps,
                             double *left_motor_rpm, double *right_motor_rpm);

#ifdef __cplusplus
}
#endif
#endif
