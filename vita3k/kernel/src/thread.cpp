// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <cpu/functions.h>
#include <kernel/thread/thread_state.h>

#include <kernel/state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <util/align.h>

#include <util/log.h>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <windows.h>

#include <timeapi.h>
// clang-format on
#pragma comment(lib, "winmm.lib")
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

void ThreadSignal::wait() {
    guest_sched_release_for_block();
    std::unique_lock<std::mutex> lock(mutex);
    recv_cond.wait(lock, [&]() { return signaled; });
    signaled = false;
}

bool ThreadSignal::send() {
    std::unique_lock<std::mutex> lock(mutex);
    if (signaled) {
        return false;
    }
    signaled = true;
    recv_cond.notify_one();
    return true;
}

void request_precise_host_timer() {
#ifdef _WIN32
    static bool done = false;
    if (done)
        return;
    done = true;
    if (timeBeginPeriod(1) == TIMERR_NOERROR)
        LOG_INFO("[TIMEDWAIT] requested 1ms host timer resolution (timeBeginPeriod): guest timed waits now have ~1ms granularity instead of the 15.6ms default");
    else
        LOG_WARN("[TIMEDWAIT] timeBeginPeriod(1) failed; short guest waits keep the default host timer granularity");
#endif
}

// Keep this a plain timed wait, because spinning here would churn the waited-on primitive's own lock
// and can starve whichever thread is trying to signal it.
bool ThreadState::wait_for_run_precise(std::unique_lock<std::mutex> &lock, int64_t timeout_us) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
    const auto woken = [&] { return status == ThreadStatus::run; };
    return status_cond.wait_until(lock, deadline, woken);
}

// LOG_DEBUG is compiled out, so set this true to get every thread creation back rather than one per shape
static constexpr bool LOG_EVERY_THREAD_CREATION = false;

int ThreadState::init(const char *name, Ptr<const void> entry_point, int init_priority, SceInt32 affinity_mask, int stack_size, const SceKernelThreadOptParam *option = nullptr) {
    constexpr size_t KERNEL_TLS_SIZE = 0x800;

    // the stack size should be page-aligned
    stack_size = align(stack_size, KiB(4));

    this->name = name;
    this->entry_point = entry_point.address();

    int core_num = kernel.corenum_allocator.new_corenum();
    if (core_num < 0) {
        LOG_ERROR("Out of core number to allocate, use 0");
        core_num = 0;
    }

    if (init_priority > SCE_KERNEL_LOWEST_PRIORITY_USER) {
        assert(SCE_KERNEL_HIGHEST_DEFAULT_PRIORITY <= init_priority && init_priority <= SCE_KERNEL_LOWEST_DEFAULT_PRIORITY);
        priority = init_priority - SCE_KERNEL_DEFAULT_PRIORITY + SCE_KERNEL_GAME_DEFAULT_PRIORITY_ACTUAL;
    } else {
        priority = init_priority;
    }
    this->affinity_mask = affinity_mask;
    this->stack_size = stack_size;
    start_tick = rtc_get_ticks(kernel.base_tick.tick);
    last_vblank_waited = 0;

    cpu = init_cpu(kernel.cpu_opt, id, static_cast<std::size_t>(core_num), mem);
    if (!cpu) {
        return SCE_KERNEL_ERROR_ERROR;
    }
    cpu->on_guest_breakpoint = [this](uint32_t pc) { report_guest_breakpoint(pc); };
    if (kernel.debugger.watch_code) {
        set_log_code(*cpu, true);
    }
    if (kernel.debugger.watch_memory) {
        set_log_mem(*cpu, true);
    }

    std::string alloc_name = fmt::format("Stack for thread {} (#{})", name, id);
    stack = alloc_block(mem, stack_size, alloc_name.c_str());
    memset(stack.get_ptr<void>().get(mem), 0xcc, stack_size);

    const std::string thread_line = fmt::format("[THREAD] created \"{}\" (#{}) entry=0x{:X} prio={} affinity=0x{:X} stack=0x{:X}..0x{:X}", name, id, entry_point.address(), priority, affinity_mask, stack.get(), stack.get() + stack_size);
    bool first_of_shape;
    {
        const std::lock_guard<std::mutex> guard(kernel.logged_threads_mutex);
        first_of_shape = kernel.logged_threads.emplace(fmt::format("{}|{:X}|{}|{:X}", name, entry_point.address(), priority, affinity_mask)).second;
    }

    // a repeat of an identical shape carries nothing new so only the first one is INFO
    if (LOG_EVERY_THREAD_CREATION || first_of_shape)
        LOG_INFO("{}", thread_line);
    else
        LOG_DEBUG("{}", thread_line);

    alloc_name = fmt::format("TLS for thread {} (#{})", name, id);
    const size_t tls_size = KERNEL_TLS_SIZE + kernel.tls_msize;
    tls = alloc_block(mem, tls_size, alloc_name.c_str());
    const Ptr<uint8_t> base_tls_ptr = tls.get_ptr<uint8_t>();
    memset(base_tls_ptr.get(mem), 0, tls_size);

    int *tls_array = tls.get_ptr<int>().get(mem);

    tls_array[TLS_PROCESS_ID] = 1; // stubbed. unused
    tls_array[TLS_THREAD_ID] = id;
    tls_array[TLS_SP_TOP] = stack.get();
    tls_array[TLS_SP_BOTTOM] = stack.get() + stack_size;
    tls_array[TLS_CURRENT_PRIORITY] = priority;
    tls_array[TLS_CPU_AFFINITY_MASK] = affinity_mask;

    const Ptr<uint8_t> user_tls_ptr = base_tls_ptr + KERNEL_TLS_SIZE;
    write_tpidruro(*cpu, user_tls_ptr.address());
    if (kernel.tls_address) {
        assert(kernel.tls_psize <= kernel.tls_msize);
        memcpy(user_tls_ptr.get(mem), kernel.tls_address.get(mem), kernel.tls_psize);
    }

    CPUContext ctx;
    ctx.set_sp(stack_top());
    if (option) {
        ctx.cpu_registers[0] = option->attr;
        ctx.cpu_registers[1] = option->size;
    }
    this->init_cpu_ctx = ctx;

    return 0;
}

void ThreadState::raise_waiting_threads() {
    for (const auto &t : waiting_threads) {
        const std::unique_lock<std::mutex> lock(t->mutex);
        assert(t->status == ThreadStatus::wait);
        t->status = ThreadStatus::run;
        t->status_cond.notify_all();
    }
    waiting_threads.clear();
}

int ThreadState::start(SceSize arglen, const Ptr<void> argp, bool run_entry_callback) {
    std::unique_lock<std::mutex> thread_lock(mutex);
    if (status != ThreadStatus::dormant)
        return SCE_KERNEL_ERROR_RUNNING;

    run_start_callback = run_entry_callback;
    load_context(*cpu, init_cpu_ctx);
    write_pc(*cpu, entry_point);
    write_lr(*cpu, kernel.halt_instruction_pc);
    write_reg(*cpu, 0, arglen);

    // Copy data to stack
    if (argp && arglen > 0) {
        const Address data_addr = stack_alloc(*cpu, align(arglen, 8));
        memcpy(Ptr<uint8_t>(data_addr).get(mem), argp.get(mem), arglen);
        write_reg(*cpu, 1, data_addr);
    } else {
        write_reg(*cpu, 1, 0);
    }

    if (kernel.debugger.wait_for_debugger) {
        kernel.debugger.wait_for_debugger = false;
        status = ThreadStatus::suspend;
    } else {
        status = ThreadStatus::run;
    }
    status_cond.notify_one();

    return SCE_KERNEL_OK;
}

void ThreadState::exit(SceInt32 status) {
    std::lock_guard<std::mutex> guard(mutex);
    run_end_callback = true;
    exit_requested = true;
    returned_value = static_cast<uint32_t>(status);
}

void ThreadState::exit_delete(bool exit) {
    std::lock_guard<std::mutex> lock(mutex);

    run_end_callback = exit;
    delete_requested = true;

    if (status == ThreadStatus::run) {
        stop(*cpu);
    } else if (status == ThreadStatus::wait) {
        // wake threads blocked in a sync primitive so they can observe delete_requested
        update_status(ThreadStatus::run);
    } else {
        // dormant or suspend: wake run_loop() so it can observe delete_requested
        status_cond.notify_all();
    }

    // Wake if thread waiting on sceKernelWaitSignal
    signal.send();
}

// Guest execution gate (config: accurate-thread-scheduling).
namespace {
std::mutex g_sched_mutex;
std::condition_variable g_sched_cv;
constexpr size_t GUEST_CORES_MAX = 8;
constexpr auto SCHED_DEADLINE_WINDOW = std::chrono::milliseconds(2);
constexpr auto SCHED_MIN_SLICE = std::chrono::microseconds(50);
constexpr auto SCHED_TIMESLICE = std::chrono::microseconds(250);

thread_local bool tls_holds_token = false;
thread_local int tls_token_priority = 0;
thread_local CPUState *tls_token_cpu = nullptr;
thread_local std::chrono::steady_clock::time_point tls_slice_start;

int g_sched_running = 0;
struct Holder {
    CPUState *cpu;
    int priority;
    std::chrono::steady_clock::time_point since;
    const char *name;
    SceUID id;
};

std::array<Holder, GUEST_CORES_MAX> g_sched_holders{};
// Do not "correct" this to 3 because the Vita has 3 cores - use the guest-cores config if you want to test it.
int g_sched_cores = 1;

std::array<uint16_t, 256> g_sched_waiter_counts{};
int g_sched_waiters_total = 0;
std::atomic<int> g_sched_best_waiter{ 256 };

void waiter_add(int priority) {
    g_sched_waiter_counts[priority]++;
    g_sched_waiters_total++;
    if (priority < g_sched_best_waiter.load(std::memory_order_relaxed))
        g_sched_best_waiter.store(priority, std::memory_order_relaxed);
}

void waiter_remove(int priority) {
    g_sched_waiter_counts[priority]--;
    g_sched_waiters_total--;
    int best = g_sched_best_waiter.load(std::memory_order_relaxed);
    if (g_sched_waiters_total == 0) {
        g_sched_best_waiter.store(256, std::memory_order_relaxed);
    } else if (priority == best && g_sched_waiter_counts[priority] == 0) {
        while (best < 256 && g_sched_waiter_counts[best] == 0)
            best++;
        g_sched_best_waiter.store(best, std::memory_order_relaxed);
    }
}

std::atomic<uint64_t> g_sched_acquires{ 0 };
std::atomic<uint64_t> g_sched_forced{ 0 };
std::atomic<uint64_t> g_sched_preempts{ 0 };
std::atomic<uint64_t> g_sched_wait_us{ 0 };
std::atomic<uint64_t> g_sched_max_wait_us{ 0 };
std::atomic<uint64_t> g_sched_fastpath{ 0 };
std::atomic<uint64_t> g_sched_yield_slice{ 0 };
std::atomic<uint64_t> g_sched_yield_preempted{ 0 };
std::atomic<uint64_t> g_sched_yield_block{ 0 };
std::atomic<uint64_t> g_sched_hold_us{ 0 };
std::atomic<uint64_t> g_sched_hold_max_us{ 0 };
std::atomic<int64_t> g_sched_last_report{ 0 };

void sched_report(int64_t now_us) {
    int64_t last = g_sched_last_report.load(std::memory_order_relaxed);
    if (last != 0 && now_us - last < 10'000'000)
        return;
    if (!g_sched_last_report.compare_exchange_strong(last, now_us))
        return;
    if (last == 0)
        return;
    const uint64_t acquires = g_sched_acquires.exchange(0);
    if (acquires == 0)
        return;
    const uint64_t fast = g_sched_fastpath.exchange(0);
    const uint64_t held_us = g_sched_hold_us.exchange(0);
    LOG_INFO("[GUEST-SCHED] 10s: acquires={} ({:.1f}% uncontended) preempts={} deadline_overrides={} wait_total_ms={:.1f} wait_max_ms={:.2f} | yields slice={} preempt={} block={} | held_total_ms={:.0f} held_avg_us={:.1f} held_max_ms={:.2f}",
        acquires, acquires ? (100.0 * fast / acquires) : 0.0,
        g_sched_preempts.exchange(0), g_sched_forced.exchange(0),
        g_sched_wait_us.exchange(0) / 1000.0, g_sched_max_wait_us.exchange(0) / 1000.0,
        g_sched_yield_slice.exchange(0), g_sched_yield_preempted.exchange(0), g_sched_yield_block.exchange(0),
        held_us / 1000.0, acquires ? static_cast<double>(held_us) / acquires : 0.0,
        g_sched_hold_max_us.exchange(0) / 1000.0);
}

void sched_acquire(int priority, SceInt32 affinity_mask, bool enabled, CPUState *cpu,
    const std::string &name, SceUID id) {
    if (affinity_mask != 0 || !enabled)
        return;
    if (tls_holds_token)
        return; // already ours for the rest of this timeslice

    const auto start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(g_sched_mutex);

    if (g_sched_running < g_sched_cores && g_sched_waiters_total == 0) {
        g_sched_running++;
        for (auto &h : g_sched_holders) {
            if (!h.cpu) {
                h = { cpu, priority, start, name.c_str(), id };
                break;
            }
        }
        g_sched_acquires.fetch_add(1, std::memory_order_relaxed);
        g_sched_fastpath.fetch_add(1, std::memory_order_relaxed);
        tls_holds_token = true;
        tls_token_priority = priority;
        tls_token_cpu = cpu;
        tls_slice_start = start;
        return;
    }

    waiter_add(priority);

    auto preempt_lower = [&](const std::chrono::steady_clock::time_point now) {
        Holder *victim = nullptr;
        for (auto &h : g_sched_holders) {
            if (h.cpu && h.priority > priority && now - h.since >= SCHED_MIN_SLICE && (!victim || h.priority > victim->priority))
                victim = &h;
        }
        if (victim) {
            stop(*victim->cpu);
            g_sched_preempts.fetch_add(1, std::memory_order_relaxed);
        }
    };
    preempt_lower(start);

    const auto deadline = start + SCHED_DEADLINE_WINDOW;
    while (g_sched_running >= g_sched_cores || g_sched_best_waiter.load(std::memory_order_relaxed) < priority) {
        if (g_sched_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
            g_sched_forced.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int64_t> last_logged_us{ 0 };
            const int64_t now_forced_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                                              .count();
            int64_t prev_logged = last_logged_us.load(std::memory_order_relaxed);
            if (now_forced_us - prev_logged >= 10'000'000
                && last_logged_us.compare_exchange_strong(prev_logged, now_forced_us)) {
                int holder_prio = -1;
                for (const auto &h : g_sched_holders) {
                    if (h.cpu)
                        holder_prio = h.priority;
                }
                LOG_WARN("[GUEST-SCHED] \"{}\" (#{} prio {}) forced through after {}ms; holder prio {}", name, id, priority, std::chrono::duration_cast<std::chrono::milliseconds>(SCHED_DEADLINE_WINDOW).count(), holder_prio);
            }
            break;
        }
        preempt_lower(std::chrono::steady_clock::now());
    }
    waiter_remove(priority);
    g_sched_running++;
    for (auto &h : g_sched_holders) {
        if (!h.cpu) {
            h = { cpu, priority, std::chrono::steady_clock::now(), name.c_str(), id };
            break;
        }
    }

    tls_holds_token = true;
    tls_token_priority = priority;
    tls_token_cpu = cpu;
    tls_slice_start = std::chrono::steady_clock::now();

    const auto waited_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    g_sched_acquires.fetch_add(1, std::memory_order_relaxed);
    g_sched_wait_us.fetch_add(waited_us, std::memory_order_relaxed);
    uint64_t prev_max = g_sched_max_wait_us.load(std::memory_order_relaxed);
    while (waited_us > prev_max && !g_sched_max_wait_us.compare_exchange_weak(prev_max, waited_us)) {
    }
}

void sched_release_internal(CPUState *cpu) {
    const auto held_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - tls_slice_start).count());
    g_sched_hold_us.fetch_add(held_us, std::memory_order_relaxed);
    uint64_t prev_hold = g_sched_hold_max_us.load(std::memory_order_relaxed);
    while (held_us > prev_hold && !g_sched_hold_max_us.compare_exchange_weak(prev_hold, held_us)) {
    }

    bool has_waiters;
    {
        const std::lock_guard<std::mutex> lock(g_sched_mutex);
        for (auto &h : g_sched_holders) {
            if (h.cpu == cpu) {
                h.cpu = nullptr;
                break;
            }
        }
        if (g_sched_running > 0)
            g_sched_running--;
        has_waiters = g_sched_waiters_total > 0;
    }
    tls_holds_token = false;
    tls_token_cpu = nullptr;

    if (has_waiters)
        g_sched_cv.notify_all();

    sched_report(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void sched_maybe_yield(SceInt32 affinity_mask, bool enabled, CPUState *cpu) {
    if (affinity_mask != 0 || !enabled || !tls_holds_token)
        return;

    const bool slice_over = (std::chrono::steady_clock::now() - tls_slice_start) >= SCHED_TIMESLICE;
    const bool outranked = !slice_over && g_sched_best_waiter.load(std::memory_order_relaxed) < tls_token_priority;
    if (slice_over || outranked) {
        (slice_over ? g_sched_yield_slice : g_sched_yield_preempted).fetch_add(1, std::memory_order_relaxed);
        sched_release_internal(cpu);
    }
}
} // namespace

void guest_sched_set_cores(int cores) {
    const std::lock_guard<std::mutex> lock(g_sched_mutex);
    g_sched_cores = std::clamp(cores, 1, 4); // the Vita has four cores, three for the application
    if (g_sched_cores != cores)
        LOG_WARN("[GUEST-SCHED] guest-cores {} out of range, using {}", cores, g_sched_cores);
    LOG_INFO("[GUEST-SCHED] gate will run at most {} guest thread(s) at a time", g_sched_cores);
}

void guest_sched_release_for_block() {
    if (tls_holds_token) {
        g_sched_yield_block.fetch_add(1, std::memory_order_relaxed);
        sched_release_internal(tls_token_cpu);
    }
}

CPUState *guest_sched_token_cpu() {
    return tls_holds_token ? tls_token_cpu : nullptr;
}

void guest_sched_forget_cpu(CPUState *cpu) {
    if (!cpu)
        return;
    bool found = false;
    bool has_waiters = false;
    {
        const std::lock_guard<std::mutex> lock(g_sched_mutex);
        for (auto &h : g_sched_holders) {
            if (h.cpu == cpu) {
                LOG_WARN("[GUEST-SCHED] thread \"{}\" (#{} prio {}) destroyed while still holding the token - a release path is missing",
                    h.name ? h.name : "?", h.id, h.priority);
                h.cpu = nullptr;
                found = true;
                break;
            }
        }
        if (found && g_sched_running > 0)
            g_sched_running--;
        has_waiters = g_sched_waiters_total > 0;
    }
    if (tls_token_cpu == cpu) {
        tls_holds_token = false;
        tls_token_cpu = nullptr;
    }
    if (found && has_waiters)
        g_sched_cv.notify_all();
}

namespace {
constexpr int64_t WAKE_GATE_CPU_US = 20;
constexpr int64_t WAKE_GATE_LONG_HLE_NS = 300'000;
constexpr int64_t WAKE_GATE_STATE_POLL_NS = 100'000;
constexpr int64_t WAKE_GATE_NO_CLOCK_NS = 200'000;
constexpr int64_t WAKE_GATE_CAP_NS = 50'000'000;
constexpr int64_t WAKE_GATE_CPU_POLL_NS = 10'000;
#ifdef _WIN32
constexpr int64_t WAKE_GATE_CPU_TICKS_PER_US = 3000;
constexpr int64_t WAKE_GATE_SPIN_NS = 100'000;
constexpr auto WAKE_GATE_BLOCK_POLL = std::chrono::milliseconds(1);
#else
constexpr int64_t WAKE_GATE_CPU_TICKS_PER_US = 1000;
constexpr int64_t WAKE_GATE_SPIN_NS = 20'000;
constexpr auto WAKE_GATE_BLOCK_POLL = std::chrono::microseconds(100);
#endif

std::mutex g_gate_mutex;
std::condition_variable g_gate_cv;
std::atomic<int64_t> g_gate_last_cap_log_ns{ 0 };
std::atomic<bool> g_gate_clock_warned{ false };

int64_t gate_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void wake_gate_notify() {
    {
        const std::lock_guard<std::mutex> lock(g_gate_mutex);
    }
    g_gate_cv.notify_all();
}

#if !defined(_WIN32) && !defined(__APPLE__)
char host_thread_state(int tid) {
    if (tid <= 0)
        return '?';
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return '?';
    char buf[512];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0)
        return '?';
    buf[n] = 0;
    const char *const paren = std::strrchr(buf, ')');
    if (!paren || paren[1] != ' ' || !paren[2])
        return '?';
    return paren[2];
}
#endif
} // namespace

void ThreadState::capture_host_cpu_clock() {
    if (host_cpu_ready.load(std::memory_order_relaxed))
        return;
#ifdef _WIN32
    HANDLE handle = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        host_cpu_handle = handle;
        host_cpu_ready.store(true, std::memory_order_release);
    }
#elif !defined(__APPLE__)
    host_tid = static_cast<int>(syscall(SYS_gettid));
    clockid_t clock_id;
    if (pthread_getcpuclockid(pthread_self(), &clock_id) == 0) {
        host_cpu_clock = static_cast<int64_t>(clock_id);
        host_cpu_ready.store(true, std::memory_order_release);
    }
#endif
    if (!host_cpu_ready.load(std::memory_order_relaxed) && !g_gate_clock_warned.exchange(true))
        LOG_WARN("[WAKEGATE] no host thread CPU clock: a thread woken by a higher-priority thread waits at most {} us for its waker to move on", WAKE_GATE_NO_CLOCK_NS / 1000);
}

int64_t ThreadState::host_cpu_now() const {
    if (!host_cpu_ready.load(std::memory_order_acquire))
        return -1;
#ifdef _WIN32
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(static_cast<HANDLE>(host_cpu_handle), &cycles))
        return -1;
    return static_cast<int64_t>(cycles);
#elif !defined(__APPLE__)
    timespec ts{};
    if (clock_gettime(static_cast<clockid_t>(host_cpu_clock), &ts) != 0)
        return -1;
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#else
    return -1;
#endif
}

char ThreadState::host_state() const {
#if !defined(_WIN32) && !defined(__APPLE__)
    return host_thread_state(host_tid);
#else
    return '?';
#endif
}

void ThreadState::arm_wake_gate() {
    const ThreadState *const waker = g_tls_guest_thread;
    if (!waker || waker == this || waker->priority >= priority)
        return;
    gate_h0 = waker->hle_returns.load(std::memory_order_acquire);
    gate_kind = wait_prim_kind;
    gate_uid = wait_prim_uid;
    gate_waker.store(waker->id, std::memory_order_release);
}

void ThreadState::wait_wake_gate() {
    const SceUID waker_id = gate_waker.exchange(0, std::memory_order_acquire);
    if (!waker_id)
        return;
    const uint64_t h0 = gate_h0;
    const ThreadStatePtr waker = kernel.get_thread(waker_id);
    guest_sched_release_for_block();
    if (!waker)
        return;
    const int64_t start = gate_now_ns();
    const int64_t cpu0 = waker->host_cpu_now();
    const int64_t cpu_needed = WAKE_GATE_CPU_US * WAKE_GATE_CPU_TICKS_PER_US;
    uint64_t hle_seen = 0;
    int64_t hle_seen_ns = 0;
    int64_t hle_next_state_ns = 0;
    int64_t last_cpu_poll = 0;
    bool capped = false;
    waker->gated_wakees.fetch_add(1);
    for (uint32_t iter = 0;; iter++) {
        const int64_t now = gate_now_ns();
        const int64_t waited = now - start;
        if (delete_requested || exit_requested || suspend_requested || world_stop_requested)
            break;
        const uint64_t returns = waker->hle_returns.load();
        if (returns >= h0 + 2 || waker->status != ThreadStatus::run || waker->is_delete_requested())
            break;
        if (cpu0 >= 0 && (iter == 0 || now - last_cpu_poll >= WAKE_GATE_CPU_POLL_NS)) {
            last_cpu_poll = now;
            const int64_t cpu = waker->host_cpu_now();
            if (cpu < 0 || cpu - cpu0 >= cpu_needed)
                break;
        }
        const uint64_t enters = waker->hle_enters.load();
        if (enters > returns) {
            if (enters != hle_seen) {
                hle_seen = enters;
                hle_seen_ns = now;
                hle_next_state_ns = now + WAKE_GATE_STATE_POLL_NS;
            } else if (now >= hle_next_state_ns) {
                hle_next_state_ns = now + WAKE_GATE_STATE_POLL_NS;
                const char state = waker->host_state();
                if (state != 'R' && (state != '?' || now - hle_seen_ns >= WAKE_GATE_LONG_HLE_NS))
                    break;
            }
        } else {
            hle_seen = 0;
            if (cpu0 < 0 && waited >= WAKE_GATE_NO_CLOCK_NS)
                break;
        }
        if (waited >= WAKE_GATE_CAP_NS) {
            capped = true;
            break;
        }
        if (waited < WAKE_GATE_SPIN_NS) {
            std::this_thread::yield();
            continue;
        }
        std::unique_lock<std::mutex> lock(g_gate_mutex);
        if (waker->hle_returns.load() < h0 + 2 && waker->status == ThreadStatus::run)
            g_gate_cv.wait_for(lock, WAKE_GATE_BLOCK_POLL);
    }
    waker->gated_wakees.fetch_sub(1);

    if (capped) {
        const int64_t now = gate_now_ns();
        int64_t last = g_gate_last_cap_log_ns.load(std::memory_order_relaxed);
        if (now - last >= 1'000'000'000 && g_gate_last_cap_log_ns.compare_exchange_strong(last, now))
            LOG_WARN("[WAKEGATE] '{}' ({}) stopped waiting after {} ms for '{}' ({}), which woke it through {}#{} and has not moved on since (HLE calls entered {} returned {}, {} at the wake, last import 0x{:08X})",
                name, id, WAKE_GATE_CAP_NS / 1'000'000, waker->name, waker_id, gate_kind ? gate_kind : "?", gate_uid,
                waker->hle_enters.load(), waker->hle_returns.load(), h0, waker->last_import_nid);
    }
}

thread_local ThreadState *g_tls_guest_thread = nullptr;

void ThreadState::run_loop() {
    bool guest_returned = false;

    g_tls_guest_thread = this;
    struct TokenGuard {
        ~TokenGuard() { guest_sched_release_for_block(); }
    } token_guard;

    // Restore the previous state on exit so a nested run loop keeps the outer loop's CPU state
    CPUState *const prev_cpu_state = get_current_cpu_state();
    set_current_cpu_state(cpu.get());
    struct CpuStateGuard {
        CPUState *prev;
        ~CpuStateGuard() { set_current_cpu_state(prev); }
    } cpu_state_guard{ prev_cpu_state };

    std::unique_lock<std::mutex> lock(mutex);
    ++call_level;
    const bool top_level = call_level == 1;
    if (top_level)
        capture_host_cpu_clock();

    auto run_thread_end_callback = [&]() {
        if (!run_end_callback)
            return;
        run_end_callback = false;

        if (!kernel.thread_event_end)
            return;

        const ThreadStatus old_status = status;
        const uint32_t old_returned_value = returned_value;
        const bool old_exit_requested = exit_requested;
        const bool old_delete_requested = delete_requested;
        exit_requested = false;
        delete_requested = false;
        status = ThreadStatus::run;

        lock.unlock();
        const int ret = run_callback(kernel.thread_event_end.address(), { SCE_KERNEL_THREAD_EVENT_TYPE_END, static_cast<uint32_t>(id), 0, kernel.thread_event_end_arg });
        if (ret != 0)
            LOG_WARN("Thread end event handler returned {}", log_hex(ret));
        lock.lock();

        exit_requested = exit_requested || old_exit_requested;
        delete_requested = delete_requested || old_delete_requested;
        status = old_status;
        returned_value = old_returned_value;
    };

    while (true) {
        // Check if this call-level is done (normal exit, guest return, or delete).
        if (exit_requested || guest_returned || delete_requested) {
            // Top-level fires the end callback and parks dormant
            if (top_level && status != ThreadStatus::dormant) {
                run_thread_end_callback();
                update_status(ThreadStatus::dormant);
            }
            // Return from nested levels to the top level (or completely if deleted)
            if (!top_level || delete_requested)
                break;
            exit_requested = false;
            guest_returned = false;
            // Status is now dormant: we'll park until the next start() or exit_delete().
        }

        // Park until we have something to do.
        if (status != ThreadStatus::run) {
            guest_sched_release_for_block();
            status_cond.wait(lock, [&] {
                return status == ThreadStatus::run || delete_requested;
            });
            continue;
        }

        // Fire the thread-start event once per start.
        if (run_start_callback) {
            run_start_callback = false;
            lock.unlock();
            if (kernel.thread_event_start) {
                const int ret = run_callback(kernel.thread_event_start.address(), { SCE_KERNEL_THREAD_EVENT_TYPE_START, static_cast<uint32_t>(id), 0, kernel.thread_event_start_arg });
                if (ret != 0)
                    LOG_WARN("Thread start event handler returned {}", log_hex(ret));
            }
            lock.lock();
        }

        // Active JIT loop. Lock held on entry and exit; unlocked only around run/step.
        while (!delete_requested && !exit_requested && !guest_returned && status == ThreadStatus::run) {
            if (world_stop_requested) {
                world_stopped = true;
                guest_sched_release_for_block();
                update_status(ThreadStatus::suspend);
                continue;
            }

            const bool do_step = single_stepping;
            if (do_step)
                single_stepping = false;

            lock.unlock();

            if (gate_waker.load(std::memory_order_relaxed) != 0)
                wait_wake_gate();

            // Take the guest execution gate before running any guest code
            const bool gated = kernel.accurate_thread_scheduling;
            sched_acquire(priority, affinity_mask, gated, cpu.get(), name, id);

            // Single step or run
            const int res = do_step ? step(*cpu) : run(*cpu);

            // handle svc call if this was what stopped the cpu
            if (cpu->svc_called) {
                const uint32_t nid = *Ptr<uint32_t>(read_pc(*cpu) + 4).get(mem);
                // breadcrumbs for the hang dump: free here, a global lock inside call_import
                last_import_nid = nid;
                last_import_lr = read_lr(*cpu);
                push_import_ring(nid);
                hle_enters.fetch_add(1);
                kernel.call_import(*cpu, nid, id);
                hle_returns.fetch_add(1);
                if (gated_wakees.load() > 0)
                    wake_gate_notify();
                clear_exclusive(*cpu);
            }

            // hold the token across the HLE call; give it up on timeslice or preemption
            sched_maybe_yield(affinity_mask, gated, cpu.get());

            // handle pending abort (exception handler from page fault)
            if (cpu->abort_pending.exchange(false))
                dispatch_abort(*cpu);

            // Cue Probe
            bool probe_handled = false;
            bool probe_thumb = false;
            if (!do_step && hit_breakpoint(*cpu) && kernel.debugger.is_probe(read_pc(*cpu) & ~1u, probe_thumb)) {
                const uint32_t probe_pc = read_pc(*cpu) & ~1u;
                std::string ctx = fmt::format("[CUEPROBE] pc=0x{:08X} thread='{}' ({})", probe_pc, name, id);
                for (int ri = 0; ri <= 12; ri++)
                    ctx += fmt::format(" r{}=0x{:08X}", ri, read_reg(*cpu, ri));
                ctx += fmt::format(" sp=0x{:08X} lr=0x{:08X}", read_sp(*cpu), read_lr(*cpu));
                std::set<uint32_t> dump_bases;
                for (const int ri : { 0, 1, 4, 5, 6, 7 })
                    dump_bases.insert(read_reg(*cpu, ri) & ~3u);
                dump_bases.insert(read_sp(*cpu) & ~3u);
                for (const uint32_t base : dump_bases) {
                    // Bitmap validity != host accessibility
                    uint32_t words[48];
                    if (!debug_safe_copy_guest(mem, base, words, sizeof(words)))
                        continue;
                    ctx += fmt::format("\n[CUEPROBE] mem 0x{:08X}:", base);
                    for (const uint32_t w : words)
                        ctx += fmt::format(" {:08X}", w);
                }
                LOG_ERROR("{}\n{}", ctx, log_stack_traceback());
                {
                    const std::lock_guard<std::mutex> probe_guard(kernel.debugger.probe_step_mutex);
                    kernel.debugger.remove_breakpoint(mem, probe_pc);
                    step(*cpu);
                    kernel.debugger.add_breakpoint(mem, probe_pc, probe_thumb);
                }
                if (cpu->svc_called) {
                    const uint32_t nid = *Ptr<uint32_t>(read_pc(*cpu) + 4).get(mem);
                    last_import_nid = nid;
                    last_import_lr = read_lr(*cpu);
                    push_import_ring(nid);
                    hle_enters.fetch_add(1);
                    kernel.call_import(*cpu, nid, id);
                    hle_returns.fetch_add(1);
                    if (gated_wakees.load() > 0)
                        wake_gate_notify();
                    clear_exclusive(*cpu);
                }
                probe_handled = true;
            }

            lock.lock();

            if (do_step || suspend_requested || vm_suspended || world_stop_requested || (hit_breakpoint(*cpu) && !probe_handled)) {
                suspend_requested = false;
                if (world_stop_requested)
                    world_stopped = true;
                update_status(ThreadStatus::suspend);
            }

            // Guest function for this run_loop returned (or errored).
            if (res != 0) {
                if (res < 0) {
                    LOG_ERROR("Thread {} ({}) experienced a cpu error.", name, cpu->thread_id);
                    returned_value = 0xDEADDEAD;
                } else {
                    // Halt-sentinel (res = 1): guest function returned cleanly.
                    returned_value = read_reg(*cpu, 0);
                }
                guest_returned = true;
            }
        }
    }

    --call_level;
}

void ThreadState::push_arguments(const std::vector<uint32_t> &args) {
    Address sp = read_sp(*cpu);
    for (size_t i = 0; i < std::min(args.size(), static_cast<size_t>(4)); i++) {
        write_reg(*cpu, i, args[i]);
    }
    if (args.size() > 4) {
        // TODO align to 16 bytes
        const size_t remain_size = args.size() - 4;
        sp -= 4 * remain_size;
        memcpy(Ptr<uint32_t>(sp).get(mem), &args[4], remain_size * 4);
    }
    write_sp(*cpu, sp);
}

uint32_t ThreadState::run_callback(Address callback_address, const std::vector<uint32_t> &args) {
    std::unique_lock<std::mutex> thread_lock(mutex);
    if (call_level == 0) {
        LOG_ERROR("run_callback should not be called as the first thread entry");
        return 0;
    }

    // save the current context before overwriting PC/LR for the callback
    const CPUContext previous_ctx = save_context(*cpu);
    const uint32_t previous_tpidruro = read_tpidruro(*cpu);

    // we shouldn't have to clean the context I believe
    write_pc(*cpu, callback_address);
    write_lr(*cpu, kernel.halt_instruction_pc);
    push_arguments(args);
    thread_lock.unlock();

    // unlock but then immediately lock back in the run_loop function
    // shouldn't cause an issue, but maybe we could use a recursive mutex instead
    run_loop();

    thread_lock.lock();

    // restore the previous context
    // actually, in most case I don't think this is necessary as the caller
    // and the callee should respect the same calling convention
    // but do it just in case
    load_context(*cpu, previous_ctx);
    write_tpidruro(*cpu, previous_tpidruro);

    return returned_value;
}

void ThreadState::dispatch_abort(CPUState &cpu) {
    const uint32_t fault_addr = cpu.abort_fault_addr.load();
    // DABT = type 0
    const Address handler = kernel.exception_handlers[0].load();
    if (!handler)
        return;

    // Build KuKernelAbortContext on guest stack for the handler to read.
    // Note: by the time we get here, the page has already been unprotected
    // by the protect_tree mechanism, and the CPU may have executed past
    // the faulting instruction. The handler is called as a notification;
    // we don't restore from AbortContext afterward.
    // { r0-r12, sp, lr, pc, FAR } = 17 uint32_t = 68 bytes
    const uint32_t ctx_size = 17 * 4;
    const uint32_t sp_orig = read_sp(cpu);
    const uint32_t sp_aligned = align_down(sp_orig - ctx_size, 8);

    auto *ctx = Ptr<uint32_t>(sp_aligned).get(*cpu.mem);
    for (int i = 0; i < 13; i++)
        ctx[i] = read_reg(cpu, i);
    ctx[13] = sp_aligned + ctx_size;
    ctx[14] = read_lr(cpu);
    ctx[15] = read_pc(cpu);
    ctx[16] = fault_addr;

    LOG_DEBUG("DABT handler=0x{:08X} FAR=0x{:08X} PC=0x{:08X} SP=0x{:08X} sp_aligned=0x{:08X}",
        handler, fault_addr, ctx[15], sp_orig, sp_aligned);

    // run_callback saves/restores full CPU context internally
    run_callback(handler, { sp_aligned });
}

uint32_t ThreadState::run_guest_function(Address callback_address, SceSize args, const Ptr<void> argp) {
    // save the previous entry point, just in case
    const auto old_entry_point = entry_point;
    entry_point = callback_address;

    start(args, argp);
    {
        guest_sched_release_for_block();
        std::unique_lock<std::mutex> lock(mutex);
        status_cond.wait(lock, [&]() { return status == ThreadStatus::dormant; });
    }

    entry_point = old_entry_point;
    return returned_value;
}

ThreadState::ThreadState(SceUID id, KernelState &kernel, MemState &mem)
    : id(id)
    , kernel(kernel)
    , mem(mem) {
}

ThreadState::~ThreadState() {
    guest_sched_forget_cpu(cpu.get());
#ifdef _WIN32
    if (host_cpu_handle)
        CloseHandle(static_cast<HANDLE>(host_cpu_handle));
#endif
}

void ThreadState::update_status(ThreadStatus status, std::optional<ThreadStatus> expected) {
    if (expected)
        assert(expected.value() == this->status);

    if (status == ThreadStatus::wait && cpu && cpu.get() == guest_sched_token_cpu())
        guest_sched_release_for_block();

    // Don't apply the requested wait transition if being removed to not block deletion
    if (status == ThreadStatus::wait && delete_requested)
        return;

    if (status == ThreadStatus::run)
        kernel.thread_wake_counter.fetch_add(1, std::memory_order_relaxed);

    if (status == ThreadStatus::run && expected && *expected == ThreadStatus::wait)
        arm_wake_gate();

    this->status = status;
    status_cond.notify_all();

    if (status != ThreadStatus::run && gated_wakees.load() > 0)
        wake_gate_notify();

    if (status == ThreadStatus::dormant) {
        raise_waiting_threads();
    }
}

Address ThreadState::stack_top() const {
    return stack.get() + stack_size;
}

void ThreadState::suspend() {
    LOG_WARN("[SUSPLOG] suspend thread '{}' ({}) current status {}", name, id, static_cast<int>(status));
    assert(status == ThreadStatus::run);
    {
        const std::lock_guard<std::mutex> lock(mutex);
        suspend_requested = true;
        external_suspend = true;
    }
    stop(*cpu);
}

void ThreadState::suspend_and_wait() {
    LOG_INFO("[SUSPLOG] vm-suspend thread '{}' ({}) current status {}", name, id, static_cast<int>(status));
    guest_sched_release_for_block();
    std::unique_lock<std::mutex> lock(mutex);
    vm_suspended = true;
    external_suspend = true;

    if (status != ThreadStatus::run)
        return;

    suspend_requested = true;
    lock.unlock();
    stop(*cpu);
    lock.lock();

    if (!status_cond.wait_for(lock, std::chrono::seconds(5), [&] { return status != ThreadStatus::run || delete_requested; }))
        LOG_WARN("Timed out waiting for thread {} ({}) to suspend, context may be stale", name, id);
}

void ThreadState::resume(bool step) {
    LOG_WARN("[SUSPLOG] resume thread '{}' ({}) from status {}", name, id, static_cast<int>(status));
    assert(status == ThreadStatus::suspend || status == ThreadStatus::dormant);
    {
        const std::lock_guard<std::mutex> lock(mutex);
        single_stepping = step;
        external_suspend = false;
        update_status(ThreadStatus::run);
    }
}

void ThreadState::resume_if_suspended() {
    const std::lock_guard<std::mutex> lock(mutex);
    LOG_INFO("[SUSPLOG] vm-resume thread '{}' ({}) from status {}", name, id, static_cast<int>(status));
    vm_suspended = false;
    external_suspend = false;
    suspend_requested = false;
    if (status == ThreadStatus::suspend)
        update_status(ThreadStatus::run);
}

void ThreadState::release_pause() {
    const std::lock_guard<std::mutex> lock(mutex);
    // clear the pause request whether or not the thread parked and never touch a VM or world-stop suspension
    suspend_requested = false;
    external_suspend = false;
    if (status == ThreadStatus::suspend && !vm_suspended && !world_stopped)
        update_status(ThreadStatus::run);
}

void ThreadState::request_world_stop() {
    std::unique_lock<std::mutex> lock(mutex);
    world_stop_requested = true;

    if (status != ThreadStatus::run)
        return;

    suspend_requested = true;
    lock.unlock();
    stop(*cpu);
}

bool ThreadState::wait_world_stopped(std::chrono::steady_clock::time_point deadline) {
    guest_sched_release_for_block();
    std::unique_lock<std::mutex> lock(mutex);
    return status_cond.wait_until(lock, deadline, [&] { return status != ThreadStatus::run || delete_requested; });
}

bool ThreadState::resume_from_world() {
    const std::lock_guard<std::mutex> lock(mutex);
    // We asked this thread to stop, so anything we set on it is ours to clear.
    const bool we_requested = world_stop_requested;
    world_stop_requested = false;
    if (we_requested)
        suspend_requested = false;
    const bool parked_by_us = world_stopped;
    world_stopped = false;

    // Never touch a suspension that did not come from us.
    if (vm_suspended || external_suspend) {
        if (status == ThreadStatus::suspend)
            LOG_WARN("[WORLDSTOP] thread '{}' ({}) stays suspended after the world resumes: vm_suspended={} external_suspend={}", name, id, vm_suspended, external_suspend);
        return false;
    }

    if (parked_by_us && status == ThreadStatus::suspend) {
        update_status(ThreadStatus::run);
        return true;
    }

    if (we_requested && status == ThreadStatus::suspend) {
        LOG_ERROR("[WORLDSTOP] thread '{}' ({}) was left suspended after a world stop without world_stopped set - resuming it (this would otherwise be a permanent freeze)", name, id);
        update_status(ThreadStatus::run);
        return true;
    }
    return false;
}

// A retail game never executes a breakpoint on purpose. When one fires it is a library trapping on a broken invariant it found.
void ThreadState::report_guest_breakpoint(uint32_t pc) {
    guest_breakpoints++;
    static std::atomic<uint32_t> total{ 0 };
    static std::atomic<int64_t> last_verbose_us{ 0 };
    const uint32_t n = total.fetch_add(1, std::memory_order_relaxed) + 1;
    const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (n > 16 && now_us - last_verbose_us.load(std::memory_order_relaxed) < 60'000'000)
        return;
    last_verbose_us.store(now_us, std::memory_order_relaxed);

    std::string ctx = fmt::format("[BKPT] guest breakpoint #{} on thread '{}' ({}) at 0x{:08X} ({} on this thread) - no debugger attached, continuing at 0x{:08X}",
        n, name, id, pc, guest_breakpoints, read_pc(*cpu) & ~1u);
    ctx += fmt::format("\n  last import nid=0x{:08X} called from lr=0x{:08X}", last_import_nid, last_import_lr);
    uint32_t rv[13];
    ctx += "\n  regs:";
    for (int ri = 0; ri <= 12; ri++) {
        rv[ri] = read_reg(*cpu, ri);
        ctx += fmt::format(" r{}=0x{:X}", ri, rv[ri]);
    }
    ctx += fmt::format(" sp=0x{:X} lr=0x{:X}", read_sp(*cpu), read_lr(*cpu));
    uint32_t a = pc >= 32 ? pc - 32 : 0;
    while (a <= pc + 8) {
        uint32_t probe = 0;
        if (!debug_safe_copy_guest(mem, a, &probe, sizeof(probe)))
            break;
        uint16_t insn_size = 2;
        ctx += fmt::format("\n  {}0x{:08X}: {}", a == pc ? ">" : " ", a, disassemble(*cpu, a, &insn_size));
        a += insn_size ? insn_size : 2;
    }
    std::set<uint32_t> bases;
    for (int ri = 0; ri <= 7; ri++)
        bases.insert(rv[ri] & ~3u);
    bases.insert(read_sp(*cpu) & ~3u);
    for (const uint32_t base : bases) {
        uint32_t words[16];
        if (base < 0x1000 || !debug_safe_copy_guest(mem, base, words, sizeof(words)))
            continue;
        ctx += fmt::format("\n  mem 0x{:08X}:", base);
        for (const uint32_t w : words)
            ctx += fmt::format(" {:08X}", w);
    }
    ctx += "\n" + log_stack_traceback();
    LOG_ERROR("{}", ctx);
}

std::string ThreadState::describe_suspend_state() const {
    return fmt::format("suspend_requested={} vm_suspended={} external_suspend={} world_stop_requested={} world_stopped={} single_stepping={} hit_breakpoint={} guest_breakpoints={} call_level={}",
        suspend_requested, vm_suspended, external_suspend, world_stop_requested, world_stopped, single_stepping, hit_breakpoint(*cpu), guest_breakpoints, call_level);
}

std::string ThreadState::log_stack_traceback() const {
    constexpr Address END_OFFSET = 1024;
    std::string str;
    const Address sp = read_sp(*cpu);
    // A thread whose sp is near null (i.e. never started, or a non-guest/host thread) has no walkable stack
    if (sp < 0x1000)
        return str;

    uint32_t frames = 0;
    uint32_t last_printed = 0;
    for (Address addr = sp; addr <= sp + END_OFFSET && frames < 24; addr += 4) {
        uint32_t value;
        if (!debug_safe_copy_guest(mem, addr, &value, sizeof(value)))
            continue;
        if ((value & 1) == 0)
            continue; // not a Thumb return address
        const uint32_t target = value & ~1u;
        const auto mod = kernel.find_module_by_addr(target);
        if (!mod)
            continue;
        if (target == last_printed)
            continue; // collapse the repeats a saved register set produces
        last_printed = target;
        frames++;
        fmt::format_to(std::back_inserter(str), "0x{:08X} (module: {}) [sp+0x{:X}]\n",
            target, mod->module_name, addr - sp);
    }
    if (frames == 0)
        fmt::format_to(std::back_inserter(str), "(no Thumb return addresses in the top {} bytes of stack)\n", END_OFFSET);

    // the raw words as well, so a rejected frame can still be recovered by hand
    fmt::format_to(std::back_inserter(str), "  raw stack @0x{:08X}:", sp);
    for (int k = 0; k < 32; k++) {
        uint32_t w;
        if (!debug_safe_copy_guest(mem, sp + k * 4, &w, sizeof(w)))
            break;
        fmt::format_to(std::back_inserter(str), " {:08X}", w);
    }
    str += "\n";
    return str;
}
