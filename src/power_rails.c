/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Thermal Grizzly WireView Pro II serial telemetry provider.
 *
 * Only read-only protocol operations are implemented here: identification,
 * UID and live sensor reads. Configuration and firmware commands are omitted.
 */
#include "nvtop/power_rails.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static struct power_rail_snapshot current_snapshot;

static uint16_t read_le16(const uint8_t *value) {
  return (uint16_t)value[0] | (uint16_t)value[1] << 8;
}

static uint32_t read_le32(const uint8_t *value) {
  return (uint32_t)value[0] | (uint32_t)value[1] << 8 | (uint32_t)value[2] << 16 | (uint32_t)value[3] << 24;
}

#define WIREVIEW_MAX_VOLTAGE_MV 20000
#define WIREVIEW_MAX_RAIL_CURRENT_MA 200000
#define WIREVIEW_MAX_RAIL_POWER_MW 4000000
#define WIREVIEW_MAX_TOTAL_CURRENT_MA 1200000
#define WIREVIEW_MAX_TOTAL_POWER_MW 12000000
#define WIREVIEW_MIN_TEMPERATURE_DECI_C -1000
#define WIREVIEW_MAX_TEMPERATURE_DECI_C 2000
#define WIREVIEW_RAIL_COUNT 6

bool power_rails_decode_wireview_frame(const uint8_t *frame, size_t size,
                                       struct power_rail_snapshot *snapshot) {
  if (!snapshot)
    return false;

  snapshot->valid = false;
  if (!frame || size != 100 || frame[10] > 100 || frame[11] != 0 || frame[95] != 0)
    return false;

  struct power_rail_snapshot decoded = *snapshot;
  memset(decoded.rails, 0, sizeof(decoded.rails));

  for (unsigned index = 0; index < 4; ++index) {
    int16_t temperature = (int16_t)read_le16(frame + index * 2);
    if (temperature < WIREVIEW_MIN_TEMPERATURE_DECI_C || temperature > WIREVIEW_MAX_TEMPERATURE_DECI_C)
      return false;
    decoded.temperatures_deci_c[index] = temperature;
  }
  decoded.fan_percent = frame[10];
  decoded.rail_count = WIREVIEW_RAIL_COUNT;
  for (unsigned index = 0; index < decoded.rail_count; ++index) {
    const uint8_t *rail = frame + 12 + index * 12;
    int32_t voltage_mv = (int16_t)read_le16(rail);
    uint32_t current_ma = read_le32(rail + 4);
    uint32_t power_mw = read_le32(rail + 8);
    if (voltage_mv < 0 || voltage_mv > WIREVIEW_MAX_VOLTAGE_MV || current_ma > WIREVIEW_MAX_RAIL_CURRENT_MA ||
        power_mw > WIREVIEW_MAX_RAIL_POWER_MW)
      return false;
    decoded.rails[index].voltage_mv = voltage_mv;
    decoded.rails[index].current_ma = current_ma;
    decoded.rails[index].recorded_max_current_ma =
        current_ma > snapshot->rails[index].recorded_max_current_ma
            ? current_ma
            : snapshot->rails[index].recorded_max_current_ma;
    decoded.rails[index].power_uw = (uint64_t)power_mw * 1000;
    decoded.rails[index].valid = true;
  }

  uint32_t total_power_mw = read_le32(frame + 84);
  uint32_t total_current_ma = read_le32(frame + 88);
  int32_t average_voltage_mv = (int16_t)read_le16(frame + 92);
  if (total_power_mw > WIREVIEW_MAX_TOTAL_POWER_MW || total_current_ma > WIREVIEW_MAX_TOTAL_CURRENT_MA ||
      average_voltage_mv < 0 || average_voltage_mv > WIREVIEW_MAX_VOLTAGE_MV)
    return false;

  decoded.total_power_uw = (uint64_t)total_power_mw * 1000;
  decoded.total_current_ma = total_current_ma;
  decoded.average_voltage_mv = average_voltage_mv;
  decoded.fault_status = read_le16(frame + 96);
  decoded.valid = true;
  *snapshot = decoded;
  return true;
}

#ifdef __linux__

#include <fcntl.h>
#include <glob.h>
#include <libgen.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define WIREVIEW_VENDOR_ID "0483"
#define WIREVIEW_PRODUCT_ID "5740"
#define WIREVIEW_VENDOR_COMMAND 0x01
#define WIREVIEW_UID_COMMAND 0x02
#define WIREVIEW_SENSOR_COMMAND 0x04
#define WIREVIEW_RESCAN_MS 2000
#define WIREVIEW_MAX_SCAN_CANDIDATES 8
#define WIREVIEW_WRITE_TIMEOUT_MS 100

static int wireview_fd = -1;
static int64_t next_scan_ms;
static struct termios original_settings;
static bool original_settings_valid;
static bool advisory_lock_held;
static bool tty_exclusive_held;
static bool rts_changed;
static bool original_rts_valid;
static bool original_rts_asserted;
static dev_t expected_device;

static int64_t monotonic_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool read_text_file(const char *path, char *value, size_t value_size) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  ssize_t count = read(fd, value, value_size - 1);
  close(fd);
  if (count <= 0)
    return false;
  value[count] = '\0';
  value[strcspn(value, "\r\n")] = '\0';
  return true;
}

static bool tty_matches_wireview(const char *tty_path) {
  char device_link[PATH_MAX];
  char parent[PATH_MAX];
  if (snprintf(device_link, sizeof(device_link), "%s/device", tty_path) >= (int)sizeof(device_link) ||
      !realpath(device_link, parent))
    return false;

  for (unsigned depth = 0; depth < 8; ++depth) {
    char vendor_path[PATH_MAX];
    char product_path[PATH_MAX];
    char vendor[16];
    char product[16];
    if (snprintf(vendor_path, sizeof(vendor_path), "%s/idVendor", parent) < (int)sizeof(vendor_path) &&
        snprintf(product_path, sizeof(product_path), "%s/idProduct", parent) < (int)sizeof(product_path) &&
        read_text_file(vendor_path, vendor, sizeof(vendor)) &&
        read_text_file(product_path, product, sizeof(product)) &&
        strcasecmp(vendor, WIREVIEW_VENDOR_ID) == 0 && strcasecmp(product, WIREVIEW_PRODUCT_ID) == 0)
      return true;

    char previous[PATH_MAX];
    strncpy(previous, parent, sizeof(previous));
    previous[sizeof(previous) - 1] = '\0';
    char *up = dirname(parent);
    if (!up || strcmp(up, previous) == 0)
      break;
  }
  return false;
}

static void disconnect_wireview(void) {
  if (wireview_fd >= 0) {
    if (original_settings_valid)
      tcsetattr(wireview_fd, TCSANOW, &original_settings);
    if (rts_changed) {
      int rts = TIOCM_RTS;
      ioctl(wireview_fd, original_rts_valid && original_rts_asserted ? TIOCMBIS : TIOCMBIC, &rts);
    }
    if (tty_exclusive_held)
      ioctl(wireview_fd, TIOCNXCL);
    if (advisory_lock_held)
      flock(wireview_fd, LOCK_UN);
    close(wireview_fd);
  }
  wireview_fd = -1;
  original_settings_valid = false;
  advisory_lock_held = false;
  tty_exclusive_held = false;
  rts_changed = false;
  original_rts_valid = false;
  original_rts_asserted = false;
  current_snapshot.valid = false;
}

static bool wait_fd(short events, int timeout_ms) {
  struct pollfd descriptor = {.fd = wireview_fd, .events = events};
  int result;
  do {
    result = poll(&descriptor, 1, timeout_ms);
  } while (result < 0 && errno == EINTR);
  return result > 0 && (descriptor.revents & events) != 0 &&
         !(descriptor.revents & (POLLERR | POLLHUP | POLLNVAL));
}

static bool would_block(int error) {
  if (error == EAGAIN)
    return true;
#if EWOULDBLOCK != EAGAIN
  if (error == EWOULDBLOCK)
    return true;
#endif
  return false;
}

static bool write_command(uint8_t command) {
  if (command != WIREVIEW_VENDOR_COMMAND && command != WIREVIEW_UID_COMMAND &&
      command != WIREVIEW_SENSOR_COMMAND)
    return false;
  int64_t deadline = monotonic_ms() + WIREVIEW_WRITE_TIMEOUT_MS;
  for (;;) {
    ssize_t written = write(wireview_fd, &command, 1);
    if (written == 1)
      return true;
    if (written < 0 && errno == EINTR)
      continue;
    if (written < 0 && would_block(errno)) {
      int remaining = (int)(deadline - monotonic_ms());
      if (remaining > 0 && wait_fd(POLLOUT, remaining))
        continue;
    }
    return false;
  }
}

static bool read_exact(uint8_t *buffer, size_t size, int timeout_ms) {
  size_t offset = 0;
  int64_t deadline = monotonic_ms() + timeout_ms;
  while (offset < size) {
    int remaining = (int)(deadline - monotonic_ms());
    if (remaining <= 0 || !wait_fd(POLLIN, remaining))
      return false;
    ssize_t count = read(wireview_fd, buffer + offset, size - offset);
    if (count > 0) {
      offset += (size_t)count;
    } else if (count == 0 || (errno != EINTR && !would_block(errno))) {
      return false;
    }
  }
  return true;
}

static bool request(uint8_t command, uint8_t *response, size_t response_size, int timeout_ms) {
  return write_command(command) && read_exact(response, response_size, timeout_ms);
}

static bool connect_wireview(void) {
  wireview_fd = open(current_snapshot.device_path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (wireview_fd < 0)
    return false;

  struct stat device_stat;
  if (fstat(wireview_fd, &device_stat) != 0 || !S_ISCHR(device_stat.st_mode) || device_stat.st_rdev != expected_device ||
      !isatty(wireview_fd)) {
    disconnect_wireview();
    return false;
  }
  if (flock(wireview_fd, LOCK_EX | LOCK_NB) != 0) {
    disconnect_wireview();
    return false;
  }
  advisory_lock_held = true;
  if (ioctl(wireview_fd, TIOCEXCL) != 0) {
    disconnect_wireview();
    return false;
  }
  tty_exclusive_held = true;

  struct termios settings;
  if (tcgetattr(wireview_fd, &settings) != 0) {
    disconnect_wireview();
    return false;
  }
  original_settings = settings;
  original_settings_valid = true;
  cfmakeraw(&settings);
  cfsetispeed(&settings, B115200);
  cfsetospeed(&settings, B115200);
  settings.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
  settings.c_cflag |= CS8 | CLOCAL | CREAD;
  if (tcsetattr(wireview_fd, TCSANOW, &settings) != 0) {
    disconnect_wireview();
    return false;
  }

  int rts = TIOCM_RTS;
  int modem_bits;
  if (ioctl(wireview_fd, TIOCMGET, &modem_bits) == 0) {
    original_rts_valid = true;
    original_rts_asserted = (modem_bits & TIOCM_RTS) != 0;
  }
  if (ioctl(wireview_fd, TIOCMBIC, &rts) != 0) {
    disconnect_wireview();
    return false;
  }
  rts_changed = true;
  struct timespec reset_delay = {.tv_nsec = 50 * 1000 * 1000};
  while (nanosleep(&reset_delay, &reset_delay) != 0) {
    if (errno != EINTR) {
      disconnect_wireview();
      return false;
    }
  }
  if (tcflush(wireview_fd, TCIOFLUSH) != 0) {
    disconnect_wireview();
    return false;
  }
  if (ioctl(wireview_fd, TIOCMBIS, &rts) != 0) {
    disconnect_wireview();
    return false;
  }

  static const char expected_welcome[] = "Thermal Grizzly WireView Pro II";
  uint8_t welcome[sizeof(expected_welcome)];
  uint8_t vendor[3];
  if (!read_exact(welcome, sizeof(welcome), 1000) || memcmp(welcome, expected_welcome, sizeof(welcome)) != 0 ||
      !request(WIREVIEW_VENDOR_COMMAND, vendor, sizeof(vendor), 250) || vendor[0] != 0xef || vendor[1] != 0x05 ||
      !request(WIREVIEW_UID_COMMAND, current_snapshot.uid, sizeof(current_snapshot.uid), 250)) {
    disconnect_wireview();
    return false;
  }
  current_snapshot.firmware_version = vendor[2];
  return true;
}

static bool select_wireview_candidate(const char *tty_path) {
  if (!tty_matches_wireview(tty_path))
    return false;

  const char *tty_name = strrchr(tty_path, '/');
  if (!tty_name || tty_name[1] == '\0')
    return false;

  char device_number_path[PATH_MAX];
  char device_number[32];
  unsigned major_number, minor_number;
  if (snprintf(device_number_path, sizeof(device_number_path), "%s/dev", tty_path) >=
          (int)sizeof(device_number_path) ||
      !read_text_file(device_number_path, device_number, sizeof(device_number)) ||
      sscanf(device_number, "%u:%u", &major_number, &minor_number) != 2 ||
      snprintf(current_snapshot.device_path, sizeof(current_snapshot.device_path), "/dev/%s", tty_name + 1) >=
          (int)sizeof(current_snapshot.device_path))
    return false;

  expected_device = makedev(major_number, minor_number);
  current_snapshot.available = true;
  return true;
}

static bool scan_and_connect_wireview(void) {
  current_snapshot.available = false;
  current_snapshot.device_path[0] = '\0';
  expected_device = 0;

  glob_t matches;
  memset(&matches, 0, sizeof(matches));
  int result = glob("/sys/class/tty/ttyACM*", 0, NULL, &matches);
  if (result != 0) {
    globfree(&matches);
    return false;
  }

  unsigned candidates = 0;
  bool connected = false;
  for (size_t index = 0; index < matches.gl_pathc && candidates < WIREVIEW_MAX_SCAN_CANDIDATES; ++index) {
    if (!select_wireview_candidate(matches.gl_pathv[index]))
      continue;
    candidates++;
    if (connect_wireview()) {
      connected = true;
      break;
    }
  }
  globfree(&matches);
  return connected;
}

bool power_rails_init(void) {
  disconnect_wireview();
  memset(&current_snapshot, 0, sizeof(current_snapshot));
  current_snapshot.rail_count = WIREVIEW_RAIL_COUNT;
  snprintf(current_snapshot.source_name, sizeof(current_snapshot.source_name), "WireView Pro II");
  if (!scan_and_connect_wireview()) {
    next_scan_ms = monotonic_ms() + WIREVIEW_RESCAN_MS;
    return current_snapshot.available;
  }
  power_rails_refresh();
  return current_snapshot.available;
}

void power_rails_refresh(void) {
  if (wireview_fd < 0) {
    int64_t now = monotonic_ms();
    if (now < next_scan_ms)
      return;
    if (!scan_and_connect_wireview()) {
      next_scan_ms = now + WIREVIEW_RESCAN_MS;
      return;
    }
  }

  uint8_t frame[100];
  current_snapshot.valid = false;
  if (!request(WIREVIEW_SENSOR_COMMAND, frame, sizeof(frame), 300) ||
      !power_rails_decode_wireview_frame(frame, sizeof(frame), &current_snapshot)) {
    disconnect_wireview();
    next_scan_ms = monotonic_ms() + WIREVIEW_RESCAN_MS;
  }
}

void power_rails_shutdown(void) { disconnect_wireview(); }

#else

bool power_rails_init(void) {
  memset(&current_snapshot, 0, sizeof(current_snapshot));
  current_snapshot.rail_count = WIREVIEW_RAIL_COUNT;
  return false;
}

void power_rails_refresh(void) {}

void power_rails_shutdown(void) {}

#endif

const struct power_rail_snapshot *power_rails_get_snapshot(void) { return &current_snapshot; }
