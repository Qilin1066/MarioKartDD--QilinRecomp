// What the replacements share: the interrupt model and calls into guest code.
//
// The guest's interrupt handlers (a DVD read finishing, an alarm, the
// retrace) run its callbacks on whichever thread was interrupted, with
// interrupts off. Here, completions are queued and delivered at safe points:
// whenever the guest enables interrupts, and inside every replacement that
// waits or polls. Only replaced calls can see a delivery, and each one is
// entered with the guest's registers written back, which makes a delivery the
// same as an interrupt arriving at that instruction.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

#include "qilin/rt/system.hpp"

namespace mkdd {

// Run just before the program ends without destructors (the game leaving,
// a reset, the window closing): main.cpp saves the pipeline cache there.
inline void (*g_before_exit)() = nullptr;
inline void before_exit() {
    if (g_before_exit) g_before_exit();
}

// The refresh of the display the window is on, in Hz (0 when unknown or
// without vsync), and whether the retraces are locked to it: for the F3 overlay.
inline std::atomic<double> g_display_hz{0};

// Queue an interrupt's work; it runs with interrupts off, at the next safe point.
void post_interrupt(std::function<void(qilin::rt::Cpu&)> work);
// A source of timed interrupts (the retrace, alarms): `poll` is called at
// every safe point to post whatever has come due by the host clock;
// `deadline` says when the next one is due (UINT64_MAX: none), so a wait with
// every thread blocked knows how long to sleep.
void add_timer_source(std::function<void()> poll, std::function<uint64_t()> deadline);
uint64_t next_timer_deadline();
// (It also publishes that deadline to the poll clock: start_poll_clock.)
// Sleep up to `ns` while every guest thread waits, or until another host
// thread calls wake_idle because it has something for a timer source to
// post (the GPU thread, reaching a draw-done command). `on_time`: the end is a
// timer's deadline (a retrace), so it is met to within tens of microseconds:
// the OS sleeps until shortly before it, and the rest is spent yielding.
// Otherwise the end is only an upper bound and the OS's timer slack is fine.
void idle_sleep(uint64_t ns, bool on_time = true);
void wake_idle();
// The time spent in idle_sleep so far (the guest with nothing to run), for the report: the rest is work.
uint64_t idle_time_ns();

// Called when the guest writes WPAR (PPCMtwpar), which resets the write-gather
// pipe; the GPU (gpu.cpp) installs it.
void on_wpar_write(void (*reset)());
// The guest's time base: 40.5 MHz (a quarter of the bus clock), counted from
// 1 January 2000, 00:00 local time as on the console, less the time the game
// spent paused.
uint64_t time_base();
// Whether a component cable is plugged in (the port's progressive setting,
// set_console in boot.hpp): VIGetDTVStatus.
bool component_cable();
// SRAM settings the game itself changed (OSSetSoundMode, OSSetProgressiveMode):
// 0 or 1 once it has, -1 otherwise. The main loop takes them (exchange with
// -1) and writes them to the settings file, as the console keeps them in SRAM.
inline std::atomic<int> g_game_set_stereo{-1}, g_game_set_progressive{-1};
// The console's reset switch, held or not: the host sets it; the game reads
// it through OSGetResetSwitchState.
inline std::atomic<bool> g_reset_button{false};

// Pausing the game for the settings menu. The main thread asks; the guest's
// running thread parks at the next retrace (vi.cpp), and since only one
// guest thread runs at a time the whole game stands still. The time base
// stops while it's parked, so its clock, alarms and audio go on from where
// they were.
void request_pause(bool on);
bool game_parked();
void park_if_requested();  // the guest's thread, at each retrace
// Days since 1970-01-01 of a Gregorian date, and back.
int64_t days_from_civil(int64_t y, unsigned m, unsigned d);
void civil_from_days(int64_t days, int64_t& y, unsigned& m, unsigned& d);

// Start the program again with its original command line (the game's reset).
// main sets it up; it doesn't return.
void set_command_line(int argc, char** argv);
[[noreturn]] void restart_program();
inline constexpr uint64_t kTimerHz = 40500000;
// A safe point: run what's queued if interrupts are on, then switch threads
// if that woke one of higher priority (threads.cpp).
void deliver_interrupts(qilin::rt::Cpu& c);
// The same with interrupts taken as on, without switching: for the wait loop
// that runs while every thread is blocked. True if anything ran.
bool deliver_interrupts_idle(qilin::rt::Cpu& c);
bool has_pending_interrupts();
// The back edge of a lifted poll loop (rt::set_idle): deliver what's due, or
// sleep until the next timed event (a millisecond at most).
void poll_loop(qilin::rt::Cpu& c);
// The back edge of any other lifted loop, when rt::g_poll_due is set
// (rt::set_poll): a safe point like the others, so a loop that calls nothing
// (a file being decompressed, a movie frame decoded) can't hold the sound's
// interrupt back. start_poll_clock's thread sets the flag when the next timer
// deadline the guest's thread published (next_timer_deadline publishes it)
// has come, every millisecond while it stays due undelivered, and 4 ms after
// the last publication in case a timer was set since; MKDD_POLL=ms sets it
// every millisecond instead, MKDD_POLL=0 never.
void poll_point(qilin::rt::Cpu& c);
// A timer due at `t` was just set (an alarm): the poll clock hears of it now
// if it's sooner than what it waits for.
void note_deadline(uint64_t t);
uint64_t poll_point_count();  // how many times it has run (the report's)
uint64_t poll_clock_count();  // how many times the clock has set the flag
void start_poll_clock();
bool interrupts_enabled();
uint32_t interrupt_mask();  // the OS's: bit 31-n set masks interrupt n
void set_interrupts_enabled(bool on);  // a thread switch restores the new thread's
bool in_interrupt();                   // inside an interrupt's work

// Raise OS interrupt `n` (__OS_INTERRUPT_*, listed in os.cpp: 5 audio DMA,
// 6 ARAM, 7 DSP, 9-16 EXI, 17 CP, 18 PE token, 19 PE finish, 20 SI ...): its
// registered handler runs as an interrupt, if unmasked; a masked one is held
// until the OS unmasks it.
void raise_interrupt(uint32_t n);
// Inside an interrupt's work: run OS interrupt `n`'s handler now. False if
// the OS has it masked (nothing ran). For level-triggered sources (the DSP).
bool call_interrupt_handler(qilin::rt::Cpu& c, uint32_t n);

// threads.cpp: after an interrupt's work, switch if it readied a thread of
// higher priority than the one running.
void preempt(qilin::rt::Cpu& c);

// Call guest function `fn` with r3, r4 as if from an interrupt: on a fresh
// frame below the current stack, with every register restored afterwards.
void guest_callback(qilin::rt::Cpu& c, uint32_t fn, uint32_t a0, uint32_t a1);

}  // namespace mkdd
