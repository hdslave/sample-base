// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

std::string json_string(const std::string& value);

struct StorageMount {
    std::string device, path, type;
    bool read_only = true;
};
std::vector<StorageMount> parse_mountinfo(const std::string& text);

// CPU time from /proc/stat in clock ticks: busy (user, nice, system, irq,
// softirq, steal) and the total including idle and iowait. The first entry
// sums every CPU; cpu0, cpu1 ... follow. Guest time is already in user.
struct CpuTimes { uint64_t busy = 0, total = 0; };
std::vector<CpuTimes> parse_proc_stat(const std::string& text);
// The busy share of the time between two readings, 0..1; 0 when no time
// passed or the counters went backwards.
double cpu_load(const CpuTimes& before, const CpuTimes& after);

// /proc/meminfo in kB, 0 where a line is missing. Kernels without
// MemAvailable report MemFree instead.
struct MemoryReading { uint64_t total_kb = 0, available_kb = 0; };
MemoryReading parse_meminfo(const std::string& text);

// Typed readings for an app's own screen; json() reports everything as text.
// Values the device lacks stay -1 or empty: never a made-up battery level.
struct SupplyReading { std::string name, type; int online = -1; long microvolts = -1; };
struct ThermalReading { std::string type; long millidegrees = 0; };
struct BacklightReading { long brightness = -1, maximum = -1; };
// The CPUs' speed as cpufreq has it, in kHz: the fastest the policy allows now (`limit`), the fastest the hardware
// goes (`top`), the speed it runs at, and each speed it can take, slowest first. -1 or empty where the kernel has
// no cpufreq.
struct CpuSpeedReading { long limit_khz = -1, top_khz = -1, now_khz = -1; std::string governor; std::vector<long> steps_khz; };
struct StorageSpace { std::string device, path, type; bool read_only = true; uint64_t free_bytes = 0, total_bytes = 0; };

// All file access belongs on the control/service thread. Roots are injectable
// for offline tests. No raw GPIO, I2C, regulator, gadget or block-device writes.
class Platform {
public:
    explicit Platform(std::filesystem::path sys = "/sys", std::filesystem::path proc = "/proc", std::filesystem::path run = "/run")
        : sys(std::move(sys)), proc(std::move(proc)), run(std::move(run)) {}
    ~Platform();
    std::string json() const;
    bool brightness(unsigned value);
    // The CPUs' speed limit: every cpufreq policy's scaling_max_freq set to the fastest speed it has at or under
    // `khz` (0: its top speed, no limit). The governor is left alone: under the kernel's `performance` the CPUs
    // then run at the limit steadily, with no switching for an audio thread to meet. On the MPC Sample the four
    // cores share one policy; 1416000 is the fastest speed at the lowest CPU voltage (docs/HARDWARE.md, "CPU
    // speed"). The speed before the first change comes back on normal exit, and is kept in
    // <run>/sample-base-cpu-limit meanwhile, so install/restore-stock.sh puts it back after a crash. Returns
    // whether every policy has the limit asked for; false without cpufreq.
    bool cpu_limit(long khz);
    CpuSpeedReading cpu_speed() const;                // The first cpufreq policy
    // What an earlier process changed and was killed before it could put back: the CPUs' limit and the backlight
    // as their notes in <run> have them, as install/restore-stock.sh would. That script runs when an app's service
    // stops, but not when one app is swapped for another or updated in place, and a process killed then leaves
    // both changed with nobody to undo them: the next process would take the changed values for the stock ones.
    // For the runtime's one long-lived Platform, once as it starts (Controller::run()). It does nothing once this
    // Platform has changed either itself: the notes are then its own.
    void recover();

    std::vector<SupplyReading> supplies() const;      // Every power supply
    std::vector<ThermalReading> temperatures() const; // Every thermal zone, in zone order
    BacklightReading backlight() const;               // The first backlight
    std::string usb_state() const;                    // The first USB device controller's state
    // Mounted block devices, each device once (bind mounts of it are left
    // out), with free and total space. statvfs() can wait on a slow card.
    std::vector<StorageSpace> storage() const;
    std::vector<CpuTimes> cpu_times() const;          // parse_proc_stat() of /proc/stat
    MemoryReading memory() const;                     // parse_meminfo() of /proc/meminfo
private:
    std::filesystem::path sys, proc, run;
    std::filesystem::path changed_backlight;
    std::string saved_brightness;
    std::vector<std::pair<std::filesystem::path, std::string>> saved_cpu_limits; // each policy changed, and its limit before
    void restore_cpu_limits();
};
