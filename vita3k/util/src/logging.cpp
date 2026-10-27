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

#include <atomic>
#include <chrono>
#include <exception>
#include <typeinfo>
#include <util/fork_build.h>
#include <util/log.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dbghelp.h>

#include <csignal>
#endif

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/dup_filter_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#ifdef __ANDROID__
#include <spdlog/sinks/android_sink.h>
#else
#include <spdlog/sinks/stdout_color_sinks.h>
#endif

#include <algorithm>
#include <functional>
#include <iostream>
#include <mutex>
#include <vector>

namespace logging {

static const fs::path &LOG_FILE_NAME = "vita3k.log";
static const char *LOG_PATTERN = "%^[%H:%M:%S.%e] |%L| [%!]: %v%$";
static constexpr size_t ASYNC_LOG_QUEUE_SIZE = 65536;
static std::vector<spdlog::sink_ptr> sinks;
static std::once_flag s_async_logging_once;
#ifdef __ANDROID__
static std::shared_ptr<spdlog::sinks::android_sink_mt> s_logcat_sink;
#endif

static std::function<void(std::string, int)> s_log_callback;
static std::mutex s_log_callback_mutex;
static fs::path s_log_file_path;
static std::string (*s_crash_context_provider)() = nullptr;

static void register_log_exception_handler();
static void rebuild_default_logger();

void flush() {
    spdlog::details::registry::instance().flush_all();
}

template <typename Mutex>
class callback_sink final : public spdlog::sinks::base_sink<Mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg &msg) override {
        std::function<void(std::string, int)> callback;
        {
            const std::lock_guard<std::mutex> lock(s_log_callback_mutex);
            callback = s_log_callback;
        }

        if (!callback)
            return;

        spdlog::memory_buf_t formatted;
        spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);
        callback(fmt::to_string(formatted), static_cast<int>(msg.level));
    }

    void flush_() override {};
};

using callback_sink_mt = callback_sink<std::mutex>;

void set_log_callback(std::function<void(std::string, int)> cb) {
    const std::lock_guard<std::mutex> lock(s_log_callback_mutex);
    s_log_callback = std::move(cb);
}

#ifdef __ANDROID__
// flushing every line and copying it to logcat only pay for themselves at debug or trace level
static void apply_android_log_policy(const spdlog::level::level_enum log_level) {
    const bool verbose = log_level <= spdlog::level::debug;
    spdlog::flush_on(verbose ? spdlog::level::trace : spdlog::level::err);
    if (s_logcat_sink)
        s_logcat_sink->set_level(verbose ? spdlog::level::trace : spdlog::level::off);
}
#endif

ExitCode init(const Root &root_paths, bool use_stdout) {
    sinks.clear();
#ifdef __ANDROID__
    s_logcat_sink = use_stdout ? std::make_shared<spdlog::sinks::android_sink_mt>() : nullptr;
    if (s_logcat_sink)
        sinks.push_back(s_logcat_sink);
#else
    if (use_stdout)
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#endif

#ifndef __ANDROID__
    sinks.push_back(std::make_shared<callback_sink_mt>());
#endif

    constexpr bool RELEASE_VERSION = true; // Nick

    s_log_file_path = root_paths.get_log_path() / LOG_FILE_NAME;
    if (add_sink(root_paths.get_log_path() / LOG_FILE_NAME) != Success)
        return InitConfigFailed;
    LOG_INFO("================= Vita3K session start =================");
    LOG_INFO("FORK BUILD seq {} compiled {} {} | {}", FORK_BUILD_SEQ, __DATE__, __TIME__, RELEASE_VERSION ? "Release Version" : FORK_BUILD_CHANGES);

    std::set_terminate([]() {
        if (const std::exception_ptr eptr = std::current_exception()) {
            try {
                std::rethrow_exception(eptr);
            } catch (const std::exception &ex) {
                LOG_CRITICAL("[CRASH] std::terminate: uncaught exception {}: {}", typeid(ex).name(), ex.what());
            } catch (...) {
                LOG_CRITICAL("[CRASH] std::terminate: uncaught non-std exception");
            }
        } else {
            LOG_CRITICAL("[CRASH] std::terminate called with no active exception");
        }
        spdlog::default_logger()->flush();
        std::abort();
    });

    spdlog::set_error_handler([](const std::string &msg) {
        std::cerr << "spdlog error: " << msg << std::endl;
        assert(0);
    });

#ifdef _WIN32
    // set console codepage to UTF-8
    SetConsoleOutputCP(65001);
    SetConsoleTitle("Vita3K PSVita Emulator");
#endif

#ifdef __ANDROID__
    // the app is usually killed rather than exited, so buffered lines go out every second; set_level picks the per-line policy
    spdlog::flush_every(std::chrono::seconds(1));
    apply_android_log_policy(spdlog::level::trace);
#endif

    register_log_exception_handler();

    static std::terminate_handler old_terminate = nullptr;
    old_terminate = std::set_terminate([]() {
        try {
            auto eptr = std::current_exception();
            if (eptr) {
                std::rethrow_exception(eptr);
            } else {
                LOG_CRITICAL("Unhandled 'std::terminate()' call");
            }
        } catch (const std::exception &e) {
            LOG_CRITICAL("Unhandled C++ exception. {}", e.what());
        } catch (...) {
            LOG_CRITICAL("Unhandled C++ exception. UNKNOWN");
        }
        flush();
        if (old_terminate)
            old_terminate();
    });
    return Success;
}

void set_crash_context_provider(std::string (*provider)()) {
    s_crash_context_provider = provider;
}

void set_level(spdlog::level::level_enum log_level) {
    spdlog::set_level(log_level);
#ifdef __ANDROID__
    apply_android_log_policy(log_level);
#endif
}

ExitCode add_sink(const fs::path &log_path) {
    try {
        // One log file that appends across launches so the start of a session (GPU/driver init, etc is never lost by a relaunch
        constexpr uintmax_t LOG_SIZE_CAP = 30ull * 1024 * 1024;
        bool truncate = true;
        if (fs::exists(log_path))
            truncate = fs::file_size(log_path) > LOG_SIZE_CAP; // append while under the cap
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_path.generic_path().native(), truncate));
    } catch (const spdlog::spdlog_ex &ex) {
        std::cerr << "File log initialization failed: " << ex.what() << std::endl;
        return InitConfigFailed;
    }

#ifdef _MSC_VER
    sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
#endif

    rebuild_default_logger();
    return Success;
}

void rebuild_default_logger() {
    std::call_once(s_async_logging_once, []() {
        spdlog::init_thread_pool(ASYNC_LOG_QUEUE_SIZE, 1);
    });

    auto duplicate_filter = std::make_shared<spdlog::sinks::dup_filter_sink_mt>(std::chrono::seconds(2), spdlog::level::info);
    for (const auto &sink : sinks)
        duplicate_filter->add_sink(sink);

    auto logger = std::make_shared<spdlog::async_logger>(
        "vita3k logger",
        duplicate_filter,
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_pattern(LOG_PATTERN);
}

// log exceptions and flush log file on exceptions
#ifdef _WIN32
static std::string narrow(const wchar_t *text, int length = -1) {
    if (!text)
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};
    std::string out(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), bytes, nullptr, nullptr);
    if (length < 0 && !out.empty() && out.back() == '\0')
        out.pop_back();
    return out;
}

static HMODULE module_of(uint64_t address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module))
        return nullptr;
    return module;
}

static std::string module_name(HMODULE module) {
    wchar_t path[MAX_PATH] = {};
    const std::wstring_view full(path, GetModuleFileNameW(module, path, MAX_PATH));
    const size_t slash = full.find_last_of(L"\\/");
    const std::wstring_view name = slash == std::wstring_view::npos ? full : full.substr(slash + 1);
    return narrow(name.data(), static_cast<int>(name.size()));
}

static std::string code_location(uint64_t address) {
    const HMODULE module = module_of(address);
    if (!module)
        return fmt::format("0x{:X} (in no module: JIT code or freed memory)", address);
    return fmt::format("{}+0x{:X}", module_name(module), address - reinterpret_cast<uint64_t>(module));
}

static LONG WINAPI exception_handler(PEXCEPTION_POINTERS pExp) noexcept {
    const unsigned ec = pExp->ExceptionRecord->ExceptionCode;

    static std::atomic<int64_t> window_start_ms{ 0 };
    static std::atomic<uint32_t> in_window{ 0 };
    static std::atomic<uint64_t> suppressed{ 0 };
    constexpr uint32_t MAX_PER_WINDOW = 32;
    constexpr int64_t WINDOW_MS = 5000;

    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    int64_t started = window_start_ms.load(std::memory_order_relaxed);
    if (now_ms - started >= WINDOW_MS
        && window_start_ms.compare_exchange_strong(started, now_ms, std::memory_order_relaxed)) {
        const uint64_t dropped = suppressed.exchange(0, std::memory_order_relaxed);
        in_window.store(0, std::memory_order_relaxed);
        if (dropped)
            LOG_CRITICAL("Suppressed {} further exception report(s) in the previous 5s window", dropped);
    }
    const uint32_t seen = in_window.fetch_add(1, std::memory_order_relaxed);
    if (seen >= MAX_PER_WINDOW) {
        suppressed.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const bool should_flush = seen < 4;

    switch (ec) {
    case EXCEPTION_ACCESS_VIOLATION:
        LOG_CRITICAL("Exception EXCEPTION_ACCESS_VIOLATION ({}). ", log_hex(ec));
        switch (pExp->ExceptionRecord->ExceptionInformation[0]) {
        case 0:
            LOG_CRITICAL("Read violation at address {}.", log_hex(pExp->ExceptionRecord->ExceptionInformation[1]));
            break;
        case 1:
            LOG_CRITICAL("Write violation at address {}.", log_hex(pExp->ExceptionRecord->ExceptionInformation[1]));
            break;
        case 8:
            LOG_CRITICAL("DEP violation at address {}.", log_hex(pExp->ExceptionRecord->ExceptionInformation[1]));
            break;
        default:
            break;
        }
        break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        LOG_CRITICAL("Exception EXCEPTION_ARRAY_BOUNDS_EXCEEDED ({}). ", log_hex(ec));
        break;
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        LOG_CRITICAL("Exception EXCEPTION_DATATYPE_MISALIGNMENT ({}). ", log_hex(ec));
        break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        LOG_CRITICAL("Exception EXCEPTION_FLT_DIVIDE_BY_ZERO ({}). ", log_hex(ec));
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        LOG_CRITICAL("Exception EXCEPTION_ILLEGAL_INSTRUCTION ({}). ", log_hex(ec));
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        LOG_CRITICAL("Exception EXCEPTION_IN_PAGE_ERROR ({}). ", log_hex(ec));
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        LOG_CRITICAL("Exception EXCEPTION_INT_DIVIDE_BY_ZERO ({}). ", log_hex(ec));
        break;
    case EXCEPTION_PRIV_INSTRUCTION:
        LOG_CRITICAL("Exception EXCEPTION_PRIV_INSTRUCTION ({}). ", log_hex(ec));
        break;
    case EXCEPTION_STACK_OVERFLOW:
        LOG_CRITICAL("Exception EXCEPTION_STACK_OVERFLOW ({}). ", log_hex(ec));
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    LOG_CRITICAL("Faulting code: {}.", code_location(reinterpret_cast<uint64_t>(pExp->ExceptionRecord->ExceptionAddress)));
    if (should_flush)
        flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

struct DbgHelpApi {
    BOOL(WINAPI *initialize)(HANDLE, PCWSTR, BOOL) = nullptr;
    DWORD(WINAPI *set_options)(DWORD) = nullptr;
    BOOL(WINAPI *from_addr)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFOW) = nullptr;
    BOOL(WINAPI *line_from_addr)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINEW64) = nullptr;
    BOOL(WINAPI *write_dump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION) = nullptr;
    bool symbols = false;
};

static DbgHelpApi load_dbghelp() {
    DbgHelpApi api;
    const HMODULE dll = LoadLibraryW(L"dbghelp.dll");
    if (!dll)
        return api;
    api.initialize = reinterpret_cast<decltype(api.initialize)>(GetProcAddress(dll, "SymInitializeW"));
    api.set_options = reinterpret_cast<decltype(api.set_options)>(GetProcAddress(dll, "SymSetOptions"));
    api.from_addr = reinterpret_cast<decltype(api.from_addr)>(GetProcAddress(dll, "SymFromAddrW"));
    api.line_from_addr = reinterpret_cast<decltype(api.line_from_addr)>(GetProcAddress(dll, "SymGetLineFromAddrW64"));
    api.write_dump = reinterpret_cast<decltype(api.write_dump)>(GetProcAddress(dll, "MiniDumpWriteDump"));
    if (api.initialize && api.set_options) {
        api.set_options(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
        wchar_t exe[MAX_PATH] = {};
        std::wstring dir(exe, GetModuleFileNameW(nullptr, exe, MAX_PATH));
        dir = dir.substr(0, dir.find_last_of(L"\\/"));
        api.symbols = api.initialize(GetCurrentProcess(), dir.c_str(), TRUE) != FALSE;
    }
    return api;
}

static std::string symbol_text(const DbgHelpApi &dbg, uint64_t address) {
    if (!dbg.symbols || !dbg.from_addr)
        return {};
    alignas(SYMBOL_INFOW) unsigned char buffer[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)] = {};
    auto *symbol = reinterpret_cast<SYMBOL_INFOW *>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    if (!dbg.from_addr(GetCurrentProcess(), address, &displacement, symbol))
        return {};
    std::string text = fmt::format(" {}+0x{:X}", narrow(symbol->Name, static_cast<int>(symbol->NameLen)), displacement);
    IMAGEHLP_LINEW64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD line_displacement = 0;
    if (dbg.line_from_addr && dbg.line_from_addr(GetCurrentProcess(), address, &line_displacement, &line) && line.FileName) {
        const std::string file = narrow(line.FileName);
        const size_t slash = file.find_last_of("\\/");
        text += fmt::format(" [{}:{}]", slash == std::string::npos ? file : file.substr(slash + 1), line.LineNumber);
    }
    return text;
}

static std::string module_identity(HMODULE module) {
    const auto *base = reinterpret_cast<const unsigned char *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    return fmt::format("{} base 0x{:X} size 0x{:X} timestamp 0x{:08X}", module_name(module), reinterpret_cast<uint64_t>(module),
        nt->OptionalHeader.SizeOfImage, nt->FileHeader.TimeDateStamp);
}

static int unwind_stack(const CONTEXT *start, uint64_t *frames, int max_frames) {
    CONTEXT context = *start;
    int count = 0;
    __try {
        while (count < max_frames && context.Rip) {
            frames[count++] = context.Rip;
            DWORD64 image_base = 0;
            const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
            const DWORD64 previous_rsp = context.Rsp;
            if (function) {
                PVOID handler_data = nullptr;
                DWORD64 establisher_frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher_frame, nullptr);
            } else {
                context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
                context.Rsp += 8;
            }
            if (context.Rsp <= previous_rsp)
                break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

static const char *exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case 0xC0000374: return "STATUS_HEAP_CORRUPTION";
    case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN";
    case 0xE06D7363: return "C++ exception";
    case 0x40000015: return "fatal application exit";
    default: return "exception";
    }
}

static void append_report(const std::vector<std::string> &lines) {
    if (const auto logger = spdlog::default_logger()) {
        logger->flush();
        if (const auto pool = spdlog::thread_pool()) {
            for (int waited = 0; waited < 1000 && pool->queue_size() != 0; waited += 10)
                Sleep(10);
        }
        logger->flush();
        Sleep(50);
    }
    const fs::path path = s_log_file_path.empty() ? fs::path(LOG_FILE_NAME) : s_log_file_path;
    const HANDLE file = CreateFileW(path.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    SYSTEMTIME now;
    GetLocalTime(&now);
    const std::string prefix = fmt::format("[{:02}:{:02}:{:02}.{:03}] |C| [crash_report]: ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    for (const std::string &line : lines) {
        const std::string text = prefix + line + "\r\n";
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        }
        OutputDebugStringA(text.c_str());
    }
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
}

static std::string write_minidump(const DbgHelpApi &dbg, PEXCEPTION_POINTERS exception, DWORD thread_id) {
    if (!dbg.write_dump)
        return "minidump not written: dbghelp.dll has no MiniDumpWriteDump";
    SYSTEMTIME now;
    GetLocalTime(&now);
    const fs::path dir = s_log_file_path.empty() ? fs::current_path() : s_log_file_path.parent_path();
    const fs::path path = dir / fmt::format("vita3k_crash_{:04}{:02}{:02}_{:02}{:02}{:02}.dmp", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    const HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return fmt::format("minidump not written: could not create {} (error {})", fs_utils::path_to_utf8(path), GetLastError());
    MINIDUMP_EXCEPTION_INFORMATION exception_info{ thread_id, exception, FALSE };
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);
    const BOOL written = dbg.write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &exception_info, nullptr, nullptr);
    const DWORD error = GetLastError();
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    CloseHandle(file);
    if (!written)
        return fmt::format("minidump failed (error 0x{:X}): {}", error, fs_utils::path_to_utf8(path));
    return fmt::format("minidump: {} ({} KiB), send it together with this log", fs_utils::path_to_utf8(path), size.QuadPart / 1024);
}

static std::string thread_description(HANDLE thread) {
    using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
    const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    PWSTR description = nullptr;
    if (!get_description || FAILED(get_description(thread, &description)) || !description)
        return {};
    std::string name = narrow(description);
    LocalFree(description);
    return name;
}

struct CrashReportRequest {
    const char *what;
    PEXCEPTION_POINTERS exception;
    DWORD thread_id;
    std::string thread_name;
    std::string guest;
};

static void build_crash_report(const CrashReportRequest &request) {
    const DbgHelpApi dbg = load_dbghelp();
    const EXCEPTION_RECORD &record = *request.exception->ExceptionRecord;
    const CONTEXT &context = *request.exception->ContextRecord;
    std::vector<std::string> lines;
    lines.push_back(fmt::format("===== Vita3K crashed: {}: {} (0x{:08X}) on host thread {}{} =====", request.what, exception_name(record.ExceptionCode),
        record.ExceptionCode, request.thread_id, request.thread_name.empty() ? std::string() : " '" + request.thread_name + "'"));
    lines.push_back(fmt::format("build: FORK BUILD seq {} compiled {} {}; {}", FORK_BUILD_SEQ, __DATE__, __TIME__, module_identity(GetModuleHandleW(nullptr))));
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        const ULONG_PTR kind = record.ExceptionInformation[0];
        lines.push_back(fmt::format("{} address 0x{:X}", kind == 0 ? "read of" : kind == 1 ? "write to"
                : kind == 8                                                                ? "execution (DEP) at"
                                                                                           : "access to",
            record.ExceptionInformation[1]));
    }
    const uint64_t fault_pc = reinterpret_cast<uint64_t>(record.ExceptionAddress);
    lines.push_back(fmt::format("at {}{}", code_location(fault_pc), symbol_text(dbg, fault_pc)));
    size_t start = 0;
    while (start < request.guest.size()) {
        const size_t end = request.guest.find('\n', start);
        lines.push_back("guest: " + request.guest.substr(start, end == std::string::npos ? std::string::npos : end - start));
        start = end == std::string::npos ? request.guest.size() : end + 1;
    }
    lines.push_back(fmt::format("registers: RAX={:016X} RBX={:016X} RCX={:016X} RDX={:016X}", context.Rax, context.Rbx, context.Rcx, context.Rdx));
    lines.push_back(fmt::format("registers: RSI={:016X} RDI={:016X} RBP={:016X} RSP={:016X}", context.Rsi, context.Rdi, context.Rbp, context.Rsp));
    lines.push_back(fmt::format("registers: R8={:016X} R9={:016X} R10={:016X} R11={:016X}", context.R8, context.R9, context.R10, context.R11));
    lines.push_back(fmt::format("registers: R12={:016X} R13={:016X} R14={:016X} R15={:016X}", context.R12, context.R13, context.R14, context.R15));
    lines.push_back(fmt::format("registers: RIP={:016X} EFLAGS={:08X}", context.Rip, context.EFlags));
    unsigned char code[32] = {};
    SIZE_T before = 0;
    SIZE_T after = 0;
    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(context.Rip - 16), code, 16, &before);
    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(context.Rip), code + 16, 16, &after);
    std::string bytes;
    for (size_t i = 0; i < 32; i++) {
        const bool known = i < 16 ? before == 16 : i - 16 < after;
        bytes += i == 16 ? " >" : " ";
        bytes += known ? fmt::format("{:02X}", code[i]) : std::string("??");
    }
    lines.push_back("code around RIP:" + bytes);
    uint64_t frames[64];
    const int frame_count = unwind_stack(&context, frames, 64);
    lines.push_back(fmt::format("call stack ({} frames):", frame_count));
    std::vector<HMODULE> modules{ GetModuleHandleW(nullptr) };
    for (int i = 0; i < frame_count; i++) {
        lines.push_back(fmt::format("  #{:<2} {}{}", i, code_location(frames[i]), symbol_text(dbg, i == 0 ? frames[i] : frames[i] - 1)));
        const HMODULE module = module_of(frames[i]);
        if (module && std::find(modules.begin(), modules.end(), module) == modules.end())
            modules.push_back(module);
    }
    for (const HMODULE module : modules)
        lines.push_back("module: " + module_identity(module));
    if (!dbg.symbols)
        lines.push_back("no function names: dbghelp.dll could not be loaded; the module offsets above still locate the code");
    append_report(lines);
    append_report({ write_minidump(dbg, request.exception, request.thread_id), "===== end of crash report =====" });
}

static DWORD WINAPI crash_report_thread(LPVOID parameter) {
    build_crash_report(*static_cast<const CrashReportRequest *>(parameter));
    return 0;
}

static std::atomic<bool> s_crash_reported{ false };

static void write_crash_report(const char *what, PEXCEPTION_POINTERS exception) {
    if (s_crash_reported.exchange(true))
        return;
    CrashReportRequest request{ what, exception, GetCurrentThreadId() };
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        request.guest = "not described: the crashing thread has no stack left";
        const HANDLE thread = CreateThread(nullptr, 0, crash_report_thread, &request, 0, nullptr);
        if (thread) {
            WaitForSingleObject(thread, 30000);
            CloseHandle(thread);
        }
        return;
    }
    request.thread_name = thread_description(GetCurrentThread());
    if (s_crash_context_provider)
        request.guest = s_crash_context_provider();
    build_crash_report(request);
}

static LONG WINAPI unhandled_exception_filter(PEXCEPTION_POINTERS exception) {
    write_crash_report("unhandled exception", exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void report_abort(int) {
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0x40000015;
    record.ExceptionAddress = reinterpret_cast<PVOID>(context.Rip);
    EXCEPTION_POINTERS pointers{ &record, &context };
    write_crash_report("abort() was called (std::terminate, a failed check or a library abort)", &pointers);
}

void register_log_exception_handler() {
    if (!AddVectoredExceptionHandler(0, exception_handler)) {
        LOG_CRITICAL("Failed to register an exception handler");
    }
    SetUnhandledExceptionFilter(unhandled_exception_filter);
    std::signal(SIGABRT, report_abort);
}

#else
void register_log_exception_handler() {}
#endif
} // namespace logging
