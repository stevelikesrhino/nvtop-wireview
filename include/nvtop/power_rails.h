/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Generic auxiliary power-rail telemetry.
 *
 * This interface deliberately lives outside the GPU vendor backends: a power
 * monitor is attached to a GPU, but it is not itself a GPU.
 */
#ifndef NVTOP_POWER_RAILS_H__
#define NVTOP_POWER_RAILS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NVTOP_MAX_POWER_RAILS 8
#define NVTOP_POWER_RAIL_SOURCE_NAME_LEN 48
#define NVTOP_POWER_RAIL_DEVICE_PATH_LEN 128

struct power_rail_reading {
  int32_t voltage_mv;
  uint32_t current_ma;
  uint32_t recorded_max_current_ma;
  uint64_t power_uw;
  bool valid;
};

struct power_rail_snapshot {
  bool available;
  bool valid;
  char source_name[NVTOP_POWER_RAIL_SOURCE_NAME_LEN];
  char device_path[NVTOP_POWER_RAIL_DEVICE_PATH_LEN];
  unsigned rail_count;
  struct power_rail_reading rails[NVTOP_MAX_POWER_RAILS];
  int32_t average_voltage_mv;
  uint32_t total_current_ma;
  uint64_t total_power_uw;
  int16_t temperatures_deci_c[4];
  uint8_t fan_percent;
  uint16_t fault_status;
  uint8_t firmware_version;
  uint8_t uid[12];
};

bool power_rails_init(void);
void power_rails_refresh(void);
void power_rails_shutdown(void);
const struct power_rail_snapshot *power_rails_get_snapshot(void);

/*
 * Public for protocol regression tests and future transports.
 * snapshot must be initialized before its first decode.
 */
bool power_rails_decode_wireview_frame(const uint8_t *frame, size_t size,
                                       struct power_rail_snapshot *snapshot);

#endif /* NVTOP_POWER_RAILS_H__ */
