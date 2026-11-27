// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/audio.h"
#include "drivers/jacks.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#if defined(__linux__)
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

// What the runtime's threads share. An app reads it through App::session().
// Four threads run at once:
//   audio    AudioDevice::run(), calling App::render()
//   MIDI     Controller::run_midi(): surface and external MIDI, lights
//   service  Controller::run(): control commands and slow hardware requests
//   screen   run_app(): at App::frame_interval() during input or animation,
//            10 frames a second once it stands still; asleep in between (ScreenWake)
// The screen thread's counters (Display::Stats, plus the last draw).
struct ScreenStats {
    std::atomic<uint64_t> presented {0}, pushed {0}, pixels {0}, clips {0};
    std::atomic<uint32_t> last_push_us {0}, max_push_us {0}, full_push_us {0}, draw_us {0};
};

// What wakes the screen thread before its next frame is due: a control used, a
// note from outside, a screenshot asked for. input() is for any thread and
// never blocks: it makes a futex wake only while the screen thread sleeps.
// The screen thread sleeps in sleep_until() rather than polling.
struct ScreenWake {
    using Clock = std::chrono::steady_clock;
    std::atomic<uint32_t> events {0};   // moves with each thing to answer at once
    std::atomic<uint32_t> sleeping {0}; // the screen thread is in sleep_until()
    void input() {
        events.fetch_add(1, std::memory_order_seq_cst);
#if defined(__linux__)
        if (sleeping.load(std::memory_order_seq_cst))
            syscall(SYS_futex, reinterpret_cast<uint32_t*>(&events), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
#endif
    }
    // Screen thread: sleeps until `until`, or until events is no longer
    // `seen`. True when an event ended the sleep (or had already come).
    bool sleep_until(Clock::time_point until, uint32_t seen) {
        while (events.load(std::memory_order_seq_cst) == seen) {
            auto now = Clock::now();
            if (now >= until) return false;
#if defined(__linux__)
            auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(until - now).count();
            timespec limit {time_t(left / 1000000000), long(left % 1000000000)};
            sleeping.store(1, std::memory_order_seq_cst);
            // The kernel compares the word with `seen` before it sleeps, so an event between the test above and here is not slept through.
            syscall(SYS_futex, reinterpret_cast<uint32_t*>(&events), FUTEX_WAIT_PRIVATE, seen, &limit, nullptr, 0);
            sleeping.store(0, std::memory_order_seq_cst);
#else
            std::this_thread::sleep_for(std::min<Clock::duration>(until - now, std::chrono::milliseconds(4)));
#endif
        }
        return true;
    }
};

// Every field is atomic or guarded by a lock. Comments name the writer.
struct Session {
    std::atomic<bool> running {true}, failed {false}, screenshot {false};
    ScreenWake wake; // Any thread tells it; the screen thread sleeps on it.
    AudioStats audio; // Audio thread.
    // The screen thread sets it when App::audio_format() changes; the audio
    // thread closes the device, opens the new format and clears it.
    std::atomic<bool> audio_reopen {false};
    ScreenStats screen; // Screen thread.

    // Surface diagnostics, written by the MIDI thread.
    std::atomic<uint32_t> held {0};                // Pads held now; bit 0 is pad 1.
    std::atomic<int> last_pad {-1}, velocity {0};  // Last pad struck, 0-15, and its velocity.
    std::atomic<uint32_t> last_midi {0};           // Status, key and value of the last surface message.
    std::atomic<uint32_t> last_input {0};          // pack_last_input(); the service thread writes the power key.
    std::atomic<unsigned> hardware_events {0}, external_events {0}, synthetic_events {0}, clocks {0};
    std::atomic<unsigned> midi_send_errors {0};
    // Any thread, the audio thread too: the MIDI thread runs a pass now,
    // App::midi_output() among it, instead of at its next wake-up, so a
    // message the app has just decided goes out at once. One write to an
    // eventfd: it never blocks. The MIDI thread sets the fd while it runs.
    std::atomic<int> midi_wake {-1};
    void wake_midi() const {
        int fd = midi_wake.load(std::memory_order_relaxed);
        uint64_t one = 1;
        if (fd >= 0 && ::write(fd, &one, sizeof(one)) < 0) {} // full (never in practice): a wake is already due
    }

    std::atomic<bool> push_exit {true}; // Wheel Push returns to the stock app.

    // Headphones and line out: 1 plugged, 0 empty, -1 unknown. The service
    // thread writes each before App::jack() hears of the change.
    std::atomic<int> jacks[jack_count] {{-1}, {-1}};
    int plugged(Jack jack) const { return jacks[unsigned(jack)].load(); }

    // A remote tool such as control calibration owns the screen while
    // `prompting`: the runtime draws the prompt instead of the app, and apps
    // should leave button presses alone. The service thread writes `prompt`.
    std::atomic<bool> prompting {false};

    // SD card access (drivers/card_share.h), written by the service thread:
    // a CardShareState, whether a computer is connected and whether it has
    // ejected the drive, and a count that moves each time the card comes back.
    // While the state is not Home nothing may read or write the card.
    std::atomic<uint8_t> card_share {0};
    std::atomic<bool> card_connected {false}, card_ejected {false};
    std::atomic<uint64_t> card_returns {0};

    // File Sharing (drivers/file_share.h), written by the service thread: a
    // FileShareState and how many connections a computer has to the card.
    std::atomic<uint8_t> file_share {0};
    std::atomic<uint16_t> file_share_users {0};

    // Screen Mirror (drivers/screen_mirror.h), written by the screen thread:
    // a MirrorState, the picture size and rate offered, whether it is JPEG, whether a
    // JPEG size fell back to 640 x 480 YUYV (no encoder), whether a change the
    // app asked for waits (a computer shows the camera) and the pictures sent.
    struct Mirror {
        std::atomic<uint8_t> state {0};
        std::atomic<uint16_t> width {0}, height {0}, fps {0};
        std::atomic<bool> mjpeg {false}, fallback {false}, held {false};
        std::atomic<uint64_t> frames {0}, late {0}, bad {0}; // late: ticks with no picture sent; bad: pictures dropped
    } mirror;
    std::mutex prompt_lock;
    std::string prompt;
};
