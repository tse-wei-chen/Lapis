#if !defined(_WIN32)
#error Lapis playback soak runner is Windows-only.
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include "lapis/media_runtime.h"
#include "soak_resource_budget.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Options final {
    std::wstring media;
    std::wstring alternate_media;
    std::wstring output;
    std::wstring mode{L"mixed"};
    lapis_media_locator_kind media_kind{LAPIS_LOCATOR_FILE};
    std::uint32_t cycles{4};
    std::uint32_t timeout_seconds{15};
    std::uint32_t seek_milliseconds{250};
    std::uint32_t parent_recreates{16};
    std::uint32_t warmup_cycles{4};
    std::uint32_t max_handle_growth{0};
    std::uint32_t max_gdi_growth{0};
    std::uint32_t max_user_growth{0};
    std::uint32_t max_private_growth_mib{0};
    bool enforce_growth{};
};

using lapis::soak::ProcessMetrics;
using lapis::soak::ResourceGrowthEnvelope;
using lapis::soak::GrowthLimits;

struct CycleSample final {
    std::uint32_t cycle{};
    ProcessMetrics metrics{};
};

[[nodiscard]] std::uint32_t parse_positive(
    const wchar_t* value, std::uint32_t limit) {
    if (value == nullptr || *value == L'\0') {
        throw std::invalid_argument("Invalid numeric argument.");
    }
    std::wstring raw(value);
    if (raw.find_first_not_of(L"0123456789") != std::wstring::npos) {
        throw std::invalid_argument("Numeric argument must be positive.");
    }
    std::size_t consumed{};
    const auto number = std::stoull(raw, &consumed);
    if (consumed != raw.size() || number == 0 || number > limit) {
        throw std::out_of_range("Numeric argument out of range.");
    }
    return static_cast<std::uint32_t>(number);
}

[[nodiscard]] Options parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring flag(argv[i]);
        if (i + 1 >= argc) {
            throw std::invalid_argument("Missing value for argument.");
        }
        const std::wstring value(argv[++i]);
        if (flag == L"--media") {
            options.media = value;
        } else if (flag == L"--media-kind") {
            if (value == L"file") {
                options.media_kind = LAPIS_LOCATOR_FILE;
            } else if (value == L"directory") {
                options.media_kind = LAPIS_LOCATOR_DIRECTORY;
            } else {
                throw std::invalid_argument(
                    "Media kind must be file or directory.");
            }
        } else if (flag == L"--media-alternate") {
            options.alternate_media = value;
        } else if (flag == L"--json") {
            options.output = value;
        } else if (flag == L"--cycles") {
            options.cycles = parse_positive(value.c_str(), 10000);
        } else if (flag == L"--timeout-seconds") {
            options.timeout_seconds = parse_positive(value.c_str(), 86400);
        } else if (flag == L"--seek-ms") {
            options.seek_milliseconds = parse_positive(value.c_str(), 60000);
        } else if (flag == L"--parent-recreates") {
            options.parent_recreates = parse_positive(value.c_str(), 1000);
        } else if (flag == L"--warmup-cycles") {
            options.warmup_cycles = parse_positive(value.c_str(), 10000);
        } else if (flag == L"--max-handle-growth") {
            options.max_handle_growth = parse_positive(value.c_str(), 100000);
            options.enforce_growth = true;
        } else if (flag == L"--max-gdi-growth") {
            options.max_gdi_growth = parse_positive(value.c_str(), 100000);
            options.enforce_growth = true;
        } else if (flag == L"--max-user-growth") {
            options.max_user_growth = parse_positive(value.c_str(), 100000);
            options.enforce_growth = true;
        } else if (flag == L"--max-private-growth-mib") {
            options.max_private_growth_mib = parse_positive(value.c_str(), 65535);
            options.enforce_growth = true;
        } else if (flag == L"--mode") {
            if (value != L"mixed" && value != L"eof" &&
                value != L"stop" && value != L"churn" &&
                value != L"parent-churn") {
                throw std::invalid_argument(
                    "Mode must be mixed, eof, stop, churn, or parent-churn.");
            }
            options.mode = value;
        } else {
            throw std::invalid_argument("Unknown argument.");
        }
    }
    if (options.warmup_cycles >= options.cycles && options.enforce_growth) {
        throw std::invalid_argument("Growth validation needs cycles > warmup cycles.");
    }
    if (options.media.empty()) {
        throw std::invalid_argument("--media is required.");
    }
    return options;
}

[[nodiscard]] std::string utf8_from_wide(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    if (wide.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("Media path too long.");
    }
    const int width = static_cast<int>(wide.size());
    const int bytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), width,
        nullptr, 0, nullptr, nullptr);
    if (bytes == 0) {
        throw std::runtime_error("Media path could not be encoded as UTF-8.");
    }
    std::string output(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), width,
            output.data(), bytes, nullptr, nullptr) != bytes) {
        throw std::runtime_error("Media path UTF-8 encoding failed.");
    }
    return output;
}

[[nodiscard]] ProcessMetrics read_metrics() noexcept {
    ProcessMetrics metrics{};
    const auto process = GetCurrentProcess();
    DWORD handle_count{};
    if (GetProcessHandleCount(process, &handle_count)) {
        metrics.handles = handle_count;
    }
    metrics.gdi_objects = GetGuiResources(process, GR_GDIOBJECTS);
    metrics.user_objects = GetGuiResources(process, GR_USEROBJECTS);
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(
            process,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory))) {
        metrics.private_bytes = static_cast<std::uint64_t>(memory.PrivateUsage);
        metrics.working_set_bytes = static_cast<std::uint64_t>(memory.WorkingSetSize);
    }
    return metrics;
}

void pump_ui_messages() noexcept {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

struct RuntimeScope final {
    lapis_runtime* runtime{};
    lapis_session* session{};
    HWND window{};
    HWND pending_window{};
    HWND parent_window{};
    HWND parking_window{};

    ~RuntimeScope() {
        // This order is essential: the native render worker must join and
        // release DXGI before the borrowed HWND can be destroyed.
        if (session != nullptr) {
            lapis_session_destroy(session);
        }
        if (runtime != nullptr) {
            lapis_runtime_destroy(runtime);
        }
        if (pending_window != nullptr) {
            DestroyWindow(pending_window);
        }
        if (window != nullptr) {
            DestroyWindow(window);
        }
        if (parent_window != nullptr) {
            DestroyWindow(parent_window);
        }
        if (parking_window != nullptr) {
            DestroyWindow(parking_window);
        }
    }

    RuntimeScope(const RuntimeScope&) = delete;
    RuntimeScope& operator=(const RuntimeScope&) = delete;
    RuntimeScope() = default;
};

[[nodiscard]] lapis_playback_session_id make_session_id() {
    lapis_playback_session_id id{};
    for (std::size_t index = 0; index < 16; ++index) {
        id.bytes[index] = static_cast<std::uint8_t>(index + 1);
    }
    return id;
}

// A DXGI output change may require HWND-owner thread messages while its
// native acknowledgment waits. Pump UI on the owner; run control off-thread.
template <typename TCommand>
[[nodiscard]] lapis_result run_control_pumping_ui(TCommand&& command) {
    std::atomic<bool> finished{false};
    lapis_result outcome = LAPIS_E_INTERNAL;
    std::jthread worker([&] {
        outcome = command();
        finished.store(true, std::memory_order_release);
    });
    while (!finished.load(std::memory_order_acquire)) {
        pump_ui_messages();
        std::this_thread::sleep_for(1ms);
    }
    worker.join();
    return outcome;
}

[[nodiscard]] bool latest_diagnostic_sequence(
    lapis_session* session, std::uint64_t& sequence) {
    lapis_diagnostic_event events[LAPIS_DIAGNOSTIC_RING_CAPACITY]{};
    std::uint32_t count{};
    std::uint64_t dropped{};
    return lapis_session_read_diagnostic_events(
        session, 0, events, LAPIS_DIAGNOSTIC_RING_CAPACITY,
        &count, &sequence, &dropped) == LAPIS_OK;
}

[[nodiscard]] bool wait_for_swap_chain(
    lapis_session* session,
    std::uint64_t since_sequence,
    std::uint32_t timeout_seconds) {
    const auto deadline = Clock::now() + std::chrono::seconds(timeout_seconds);
    while (Clock::now() < deadline) {
        pump_ui_messages();
        lapis_diagnostic_event events[LAPIS_DIAGNOSTIC_RING_CAPACITY]{};
        std::uint32_t count{};
        std::uint64_t next{};
        std::uint64_t dropped{};
        if (lapis_session_read_diagnostic_events(
                session, since_sequence, events, LAPIS_DIAGNOSTIC_RING_CAPACITY,
                &count, &next, &dropped) != LAPIS_OK) {
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            if (events[i].kind == LAPIS_DIAG_SWAP_CHAIN_CREATED) {
                return true;
            }
        }
        since_sequence = next;
        lapis_playback_snapshot snapshot{};
        if (lapis_session_get_snapshot(session, &snapshot) != LAPIS_OK ||
            snapshot.transport == LAPIS_TRANSPORT_FAULTED ||
            snapshot.transport == LAPIS_TRANSPORT_ENDED) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

[[nodiscard]] bool wait_for_terminal(
    lapis_session* session,
    std::uint32_t timeout_seconds,
    lapis_transport_state& final_transport,
    std::uint64_t& observed_epoch,
    std::int64_t& observed_position_ns,
    std::uint32_t& observed_transport) {
    const auto deadline = Clock::now() +
        std::chrono::seconds(timeout_seconds);
    while (Clock::now() < deadline) {
        pump_ui_messages();
        lapis_playback_snapshot snapshot{};
        if (lapis_session_get_snapshot(session, &snapshot) != LAPIS_OK) {
            return false;
        }
        observed_epoch = snapshot.epoch.value;
        observed_position_ns = snapshot.position.nanoseconds;
        final_transport = snapshot.transport;
        observed_transport = static_cast<std::uint32_t>(snapshot.transport);
        if (final_transport == LAPIS_TRANSPORT_ENDED) {
            return true;
        }
        if (final_transport == LAPIS_TRANSPORT_FAULTED) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

void report_metrics(
    std::ostringstream& stream,
    const ProcessMetrics& metrics) {
    stream << "{\"handles\":" << metrics.handles
           << ",\"gdiObjects\":" << metrics.gdi_objects
           << ",\"userObjects\":" << metrics.user_objects
           << ",\"privateBytes\":" << metrics.private_bytes
           << ",\"workingSetBytes\":" << metrics.working_set_bytes << "}";
}

// Error labels are constants controlled by the runner, not filesystem paths.
[[nodiscard]] int run_soak(const Options& options) {
    const auto started = Clock::now();
    const auto media_utf8 = utf8_from_wide(options.media);
    const auto alternate_utf8 = utf8_from_wide(options.alternate_media);
    RuntimeScope state;
    std::string failure;
    std::uint32_t completed{};
    std::uint64_t epoch{};
    std::uint64_t successful_seeks{};
    std::uint64_t applied_output_rebinds{};
    std::uint64_t completed_parent_recreates{};
    std::uint64_t last_observed_epoch{};
    std::int64_t last_observed_position_ns{};
    std::int64_t last_opened_duration_ns{};
    std::uint32_t last_observed_transport{0xFFFFFFFFu};
    ProcessMetrics baseline{};
    ResourceGrowthEnvelope growth_envelope;
    ProcessMetrics last{};
    ProcessMetrics peak{};
    std::vector<CycleSample> samples;
    samples.reserve(130);

    if (lapis_runtime_get_abi_version() != LAPIS_MEDIA_RUNTIME_ABI_VERSION ||
        lapis_runtime_get_diagnostics_api_version() !=
            LAPIS_DIAGNOSTICS_API_VERSION) {
        failure = "abi_version_mismatch";
    }

    if (failure.empty() && lapis_runtime_create(&state.runtime) != LAPIS_OK) {
        failure = "runtime_create_failed";
    }
    if (failure.empty() &&
        lapis_session_create(
            state.runtime, make_session_id(), &state.session) != LAPIS_OK) {
        failure = "session_create_failed";
    }

    if (failure.empty()) {
        if (options.mode == L"parent-churn") {
            // Closely models WinForms' HWND island: the D3D target is a
            // persistent child of a disposable layout-container HWND, while
            // the hidden parking HWND is not owned by that container.
            state.parking_window = CreateWindowExW(
                0, L"STATIC", L"Lapis output parking",
                WS_POPUP, 0, 0, 1, 1,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (state.parking_window != nullptr) {
                state.parent_window = CreateWindowExW(
                    0, L"STATIC", L"Lapis test parent",
                    WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                    80, 80, 500, 360,
                    nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            }
            if (state.parent_window != nullptr) {
                state.window = CreateWindowExW(
                    0, L"STATIC", L"Lapis persistent D3D child",
                    WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS,
                    0, 0, 480, 320,
                    state.parent_window, nullptr,
                    GetModuleHandleW(nullptr), nullptr);
            }
        } else {
            state.window = CreateWindowExW(
                0, L"STATIC", L"Lapis native playback soak",
                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                80, 80, 400, 280,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        }
        if (state.window == nullptr) {
            failure = "window_creation_failed";
        }
    }

    if (failure.empty()) {
        lapis_video_output_surface_binding binding{};
        const lapis_video_output_surface surface{
            LAPIS_VIDEO_OUTPUT_SURFACE_WIN32_WINDOW,
            reinterpret_cast<std::uintptr_t>(state.window)
        };
        if (lapis_session_set_video_output_surface(
                state.session, surface, {epoch}, &binding) != LAPIS_OK ||
            binding != LAPIS_OUTPUT_SURFACE_DEFERRED) {
            failure = "initial_output_bind_failed";
        }
    }

    baseline = read_metrics();
    last = baseline;
    peak = baseline;

    // Keep a bounded sample count even for 10,000 iterations.
    const auto sampling_interval = std::max<std::uint32_t>(
        1, (options.cycles + 127) / 128);
    for (std::uint32_t i = 0; i < options.cycles && failure.empty(); ++i) {
        pump_ui_messages();
        // Alternate window sizes while no frame owns a borrow outside DXGI.
        // This exercises ResizeBuffers during the following playback phase.
        const int width = 360 + static_cast<int>(i % 5) * 53;
        const int height = 220 + static_cast<int>(i % 3) * 37;
        const int window_x = options.mode == L"parent-churn" ? 0 : 80;
        const int window_y = options.mode == L"parent-churn" ? 0 : 80;
        if (!MoveWindow(state.window, window_x, window_y, width, height, TRUE)) {
            failure = "window_resize_failed";
            break;
        }

        const auto& selected_media = !alternate_utf8.empty() && i % 2 == 1
            ? alternate_utf8 : media_utf8;
        lapis_media_locator locator{options.media_kind, selected_media.c_str()};
        lapis_open_result opened{};
        std::uint64_t prior_diagnostic_sequence{};
        if ((options.mode == L"churn" ||
             options.mode == L"parent-churn") &&
            !latest_diagnostic_sequence(state.session, prior_diagnostic_sequence)) {
            failure = "pre_open_diagnostics_read_failed";
            break;
        }
        const lapis_playback_epoch open_epoch{++epoch};
        const auto opened_status = lapis_session_open(
            state.session, &locator, open_epoch, &opened);
        if (opened_status != LAPIS_OK) {
            failure = "media_open_failed";
            break;
        }
        last_opened_duration_ns = opened.duration.nanoseconds;

        if (lapis_session_play(state.session, open_epoch) != LAPIS_OK) {
            failure = "media_play_failed";
            break;
        }

        if (options.mode == L"parent-churn") {
            // Recreating an HWND parent is different from swapping the
            // renderer's HWND: there must be NO native rebind or new output
            // target. The child HWND remains identical throughout.
            if (!wait_for_swap_chain(
                    state.session, prior_diagnostic_sequence,
                    options.timeout_seconds)) {
                failure = "parent_churn_swap_chain_not_ready";
                break;
            }
            const auto persistent_output = state.window;
            for (std::uint32_t attempt = 0;
                 attempt < options.parent_recreates; ++attempt) {
                // Like VideoSurfacePanel.WndProc(WM_DESTROY), reparent
                // BEFORE parent destruction, never waiting for GPU workers.
                if (SetParent(state.window, state.parking_window) !=
                        state.parent_window ||
                    GetParent(state.window) != state.parking_window) {
                    failure = "parent_churn_parking_failed";
                    break;
                }
                if (!DestroyWindow(state.parent_window)) {
                    failure = "parent_churn_old_parent_destroy_failed";
                    break;
                }
                state.parent_window = nullptr;
                if (!IsWindow(persistent_output)) {
                    failure = "parent_churn_output_destroyed";
                    break;
                }

                state.parent_window = CreateWindowExW(
                    0, L"STATIC", L"Lapis recreated parent",
                    WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                    80, 80,
                    450 + static_cast<int>(attempt % 5) * 30,
                    320 + static_cast<int>(attempt % 4) * 20,
                    nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                if (state.parent_window == nullptr) {
                    failure = "parent_churn_new_parent_failed";
                    break;
                }
                if (SetParent(state.window, state.parent_window) !=
                        state.parking_window ||
                    !SetWindowPos(state.window, nullptr, 0, 0,
                        400 + static_cast<int>(attempt % 5) * 30,
                        260 + static_cast<int>(attempt % 4) * 20,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW) ||
                    state.window != persistent_output ||
                    GetParent(state.window) != state.parent_window ||
                    !IsWindow(state.window)) {
                    failure = "parent_churn_reattach_failed";
                    break;
                }
                ++completed_parent_recreates;
                pump_ui_messages();
                std::this_thread::sleep_for(10ms);
                lapis_playback_snapshot during{};
                if (lapis_session_get_snapshot(state.session, &during) !=
                        LAPIS_OK ||
                    during.epoch.value != epoch ||
                    during.transport == LAPIS_TRANSPORT_FAULTED ||
                    during.transport == LAPIS_TRANSPORT_ENDED) {
                    failure = "parent_churn_playback_not_running";
                    break;
                }
            }
            if (!failure.empty()) {
                break;
            }
        }

        if (options.mode == L"churn") {
            // A fresh swap-chain creation proves this is a LIVE D3D binding.
            if (!wait_for_swap_chain(
                    state.session, prior_diagnostic_sequence,
                    options.timeout_seconds)) {
                failure = "initial_swap_chain_not_ready";
                break;
            }
            const auto seek_ns =
                static_cast<std::uint64_t>(options.seek_milliseconds) * 1000000u;
            if (opened.has_duration == 0 ||
                opened.duration.nanoseconds <= 0 ||
                seek_ns >= static_cast<std::uint64_t>(opened.duration.nanoseconds)) {
                failure = "media_too_short_for_seek";
                break;
            }
            state.pending_window = CreateWindowExW(
                0, L"STATIC", L"Lapis native churn output",
                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                150, 140, 460, 320,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (state.pending_window == nullptr) {
                failure = "replacement_window_creation_failed";
                break;
            }

            lapis_video_output_surface_binding binding{};
            const lapis_video_output_surface replacement{
                LAPIS_VIDEO_OUTPUT_SURFACE_WIN32_WINDOW,
                reinterpret_cast<std::uintptr_t>(state.pending_window)
            };
            const auto rebind_result = run_control_pumping_ui([&] {
                return lapis_session_set_video_output_surface(
                    state.session, replacement,
                    lapis_playback_epoch{epoch}, &binding);
            });
            if (rebind_result != LAPIS_OK ||
                binding != LAPIS_OUTPUT_SURFACE_APPLIED) {
                failure = "live_output_rebind_not_applied";
                break;
            }
            ++applied_output_rebinds;
            // Destroy old HWND only after native renderer releases it.
            if (!DestroyWindow(state.window)) {
                failure = "old_window_release_failed";
                break;
            }
            state.window = state.pending_window;
            state.pending_window = nullptr;

            if (run_control_pumping_ui([&] {
                    return lapis_session_seek(
                        state.session,
                        lapis_media_time{static_cast<std::int64_t>(seek_ns)},
                        lapis_playback_epoch{epoch + 1});
                }) != LAPIS_OK) {
                failure = "forward_seek_failed";
                break;
            }
            ++epoch;
            ++successful_seeks;
            lapis_playback_snapshot after_seek{};
            const auto forward_snapshot_result =
                lapis_session_get_snapshot(state.session, &after_seek);
            last_observed_epoch = after_seek.epoch.value;
            last_observed_transport = static_cast<std::uint32_t>(
                after_seek.transport);
            last_observed_position_ns = after_seek.position.nanoseconds;
            if (forward_snapshot_result != LAPIS_OK ||
                after_seek.epoch.value != epoch ||
                after_seek.transport != LAPIS_TRANSPORT_PLAYING) {
                failure = "forward_seek_state_invalid";
                break;
            }
            if (run_control_pumping_ui([&] {
                    return lapis_session_seek(
                        state.session, lapis_media_time{0},
                        lapis_playback_epoch{epoch + 1});
                }) != LAPIS_OK) {
                failure = "backward_seek_failed";
                break;
            }
            ++epoch;
            ++successful_seeks;
            const auto backward_snapshot_result =
                lapis_session_get_snapshot(state.session, &after_seek);
            last_observed_epoch = after_seek.epoch.value;
            last_observed_transport = static_cast<std::uint32_t>(
                after_seek.transport);
            last_observed_position_ns = after_seek.position.nanoseconds;
            if (backward_snapshot_result != LAPIS_OK ||
                after_seek.epoch.value != epoch ||
                after_seek.transport != LAPIS_TRANSPORT_PLAYING) {
                failure = "backward_seek_state_invalid";
                break;
            }
        }

        const bool await_eof = options.mode == L"eof" ||
            options.mode == L"parent-churn" ||
            (options.mode == L"mixed" && i % 2 == 0);
        if (await_eof) {
            lapis_transport_state transport = LAPIS_TRANSPORT_CLOSED;
            if (!wait_for_terminal(
                    state.session,
                    options.timeout_seconds,
                    transport,
                    last_observed_epoch,
                    last_observed_position_ns,
                    last_observed_transport)) {
                failure = transport == LAPIS_TRANSPORT_FAULTED
                    ? "playback_faulted"
                    : "playback_timeout_or_snapshot_failure";
                break;
            }
            last_observed_transport = static_cast<std::uint32_t>(transport);
        } else {
            // Close a still-active generation. This is deliberately shorter
            // than most video frames; no sleep loops can hide an actor race.
            pump_ui_messages();
            std::this_thread::sleep_for(20ms);
        }

        if (lapis_session_close(
                state.session, lapis_playback_epoch{++epoch}) != LAPIS_OK) {
            failure = "media_close_failed";
            break;
        }

        lapis_playback_snapshot after_close{};
        if (lapis_session_get_snapshot(state.session, &after_close) != LAPIS_OK ||
            after_close.transport != LAPIS_TRANSPORT_CLOSED ||
            after_close.epoch.value != epoch) {
            failure = "post_close_snapshot_invalid";
            break;
        }

        ++completed;
        last = read_metrics();
        // Every post-Close measurement contributes, not just the 128
        // bounded JSON checkpoints.
        growth_envelope.observe(completed, options.warmup_cycles, last);
        peak.handles = std::max(peak.handles, last.handles);
        peak.gdi_objects = std::max(peak.gdi_objects, last.gdi_objects);
        peak.user_objects = std::max(peak.user_objects, last.user_objects);
        peak.private_bytes = std::max(peak.private_bytes, last.private_bytes);
        peak.working_set_bytes = std::max(
            peak.working_set_bytes, last.working_set_bytes);

        if ((completed % sampling_interval) == 0 ||
            completed == options.cycles) {
            samples.push_back({completed, last});
        }
    }

    // Compare the post-warmup maximum of every post-Close sample, not
    // only the final sample (which can mask a large intermediate spike).
    if (failure.empty() && options.enforce_growth) {
        const GrowthLimits budgets{
            options.max_handle_growth,
            options.max_gdi_growth,
            options.max_user_growth,
            static_cast<std::uint64_t>(options.max_private_growth_mib) * 1048576u
        };
        failure = growth_envelope.evaluate(budgets);
    }

    // Diagnostics are read AFTER close, while the session journal still owns
    // recent events. No media path is retained in the diagnostic records.
    lapis_diagnostic_event events[LAPIS_DIAGNOSTIC_RING_CAPACITY]{};
    std::uint32_t event_count{};
    std::uint64_t next_sequence{};
    std::uint64_t dropped{};
    if (state.session != nullptr &&
        lapis_session_read_diagnostic_events(
            state.session, 0, events, LAPIS_DIAGNOSTIC_RING_CAPACITY,
            &event_count, &next_sequence, &dropped) != LAPIS_OK) {
        if (failure.empty()) {
            failure = "diagnostics_read_failed";
        }
    }

    // Treat an unexpectedly empty journal as an integration error after a
    // fully completed EOF test, where the D3D11 presenter must be initialized.
    if (failure.empty() && completed != 0 &&
        options.mode != L"stop" && event_count == 0) {
        failure = "missing_renderer_diagnostics";
    }

    const auto elapsed_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(Clock::now() - started).count();
    std::ostringstream report;
    report << "{\n  \"schemaVersion\":1"
           << ",\n  \"passed\":" << (failure.empty() ? "true" : "false")
           << ",\n  \"failure\":\"" << failure << "\""
           << ",\n  \"cyclesRequested\":" << options.cycles
           << ",\n  \"cyclesCompleted\":" << completed
           << ",\n  \"elapsedMilliseconds\":" << elapsed_ms
           << ",\n  \"mode\":\""
           << (options.mode == L"mixed" ? "mixed" :
               options.mode == L"eof" ? "eof" :
               options.mode == L"churn" ? "churn" :
               options.mode == L"parent-churn" ? "parent-churn" : "stop")
           << "\",\n  \"perCycleTimeoutSeconds\":" << options.timeout_seconds
           << ",\n  \"successfulSeeks\":" << successful_seeks
           << ",\n  \"appliedOutputRebinds\":" << applied_output_rebinds
           << ",\n  \"completedParentRecreations\":" << completed_parent_recreates
           << ",\n  \"lastObservedEpoch\":" << last_observed_epoch
           << ",\n  \"lastObservedTransport\":" << last_observed_transport
           << ",\n  \"lastObservedPositionNs\":" << last_observed_position_ns
           << ",\n  \"lastOpenedDurationNs\":" << last_opened_duration_ns
           << ",\n  \"resourceMetricsAreProcessWide\":true"
           << ",\n  \"growthValidationEnabled\":" << (options.enforce_growth ? "true" : "false")
           << ",\n  \"warmupCycles\":" << options.warmup_cycles
           << ",\n  \"warmupCaptured\":" << (growth_envelope.has_warmup() ? "true" : "false")
           << ",\n  \"postWarmupSamples\":" << growth_envelope.sample_count()
           << ",\n  \"growthLimitHandles\":" << options.max_handle_growth
           << ",\n  \"growthLimitGdi\":" << options.max_gdi_growth
           << ",\n  \"growthLimitUser\":" << options.max_user_growth
           << ",\n  \"growthLimitPrivateMiB\":" << options.max_private_growth_mib
           << ",\n  \"growthComparison\":\"post_close_peak_after_warmup\""
           << ",\n  \"postWarmup\":";
    report_metrics(report, growth_envelope.warmup());
    report << ",\n  \"postWarmupPeak\":";
    report_metrics(report, growth_envelope.peak());
    report << ",\n  \"baseline\":";
    report_metrics(report, baseline);
    report << ",\n  \"last\":";
    report_metrics(report, last);
    report << ",\n  \"peak\":";
    report_metrics(report, peak);
    report << ",\n  \"samples\":[";
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (i != 0) {
            report << ",";
        }
        report << "\n    {\"cycle\":" << samples[i].cycle << ",\"metrics\":";
        report_metrics(report, samples[i].metrics);
        report << "}";
    }
    report << "\n  ],\n  \"diagnosticEventsOverwritten\":" << dropped
           << ",\n  \"lastDiagnosticSequence\":" << next_sequence
           << ",\n  \"diagnosticEvents\":[";
    for (std::uint32_t i = 0; i < event_count; ++i) {
        const auto& ev = events[i];
        if (i != 0) {
            report << ",";
        }
        report << "\n    {\"sequence\":" << ev.sequence
               << ",\"epoch\":" << ev.epoch
               << ",\"monotonicMilliseconds\":"
               << ev.monotonic_milliseconds
               << ",\"kind\":" << ev.kind
               << ",\"failureStage\":" << ev.failure_stage
               << ",\"hresult\":" << ev.hresult
               << ",\"removedReason\":" << ev.removed_reason
               << ",\"detail\":" << ev.detail << "}";
    }
    report << "\n  ]\n}\n";

    // stdout allows CI to attach the evidence even if --json is omitted.
    const auto report_text = report.str();
    std::cout << report_text;
    if (!options.output.empty()) {
        std::ofstream output(std::filesystem::path(options.output),
                             std::ios::binary | std::ios::trunc);
        if (!output) {
            std::cerr << "Could not create the requested JSON report.\n";
            return 1;
        }
        output << report_text;
        if (!output) {
            std::cerr << "Could not finish writing the JSON report.\n";
            return 1;
        }
    }

    return failure.empty() ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        return run_soak(parse_options(argc, argv));
    } catch (const std::exception& exception) {
        std::cerr << "Lapis soak usage: --media <path> [--media-kind file|directory]"
                     " [--cycles N]"
                     " [--media-alternate <file>]"
                     " [--mode mixed|eof|stop|churn|parent-churn]"
                     " [--timeout-seconds N] [--seek-ms N]"
                     " [--parent-recreates N] [--warmup-cycles N]"
                     " [--max-handle-growth N] [--max-gdi-growth N]"
                     " [--max-user-growth N] [--max-private-growth-mib N]"
                     " [--json report.json]\n"
                  << "Error: " << exception.what() << '\n';
        return 2;
    }
}
