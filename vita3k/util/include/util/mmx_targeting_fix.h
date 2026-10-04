#pragma once

#include <atomic>
#include <cstdint>

// Metal Max Xeno (PCSG00972) targeting fix state.
//
// This is the minimal integration of the gameplay behavior proven by the
// v1.57 test build:
//   * retain the v76/v77 validator semantic corrections for the first
//     lock-on validator pass after arming;
//   * classify the decisive 0x810EDFA0 geometry sample by sign;
//   * only bypass the special type-6 rejection at 0x810ED458 while that
//     geometry sample is positive (clear LOS).
//
// No Fire/Blocked UI fields are modified here.
namespace mmx_targeting_fix {

inline std::atomic<bool> enabled{ false };
inline std::atomic<bool> combo_latched{ false };

inline std::atomic<bool> validator_semantic_armed{ false };
inline std::atomic<bool> validator_restore_pending{ false };
inline std::atomic<bool> validator_remap_translated{ false };
inline std::atomic<bool> validator_saved_store_suppressed{ false };
inline std::atomic<std::uint32_t> validator_selected_ordinal{ 0 };
inline std::atomic<std::uint32_t> validator_resolved_id{ 0 };

inline std::atomic<bool> geometry_clear{ false };
inline std::atomic<std::uint32_t> geometry_raw{ 0 };
inline std::atomic<bool> type6_branch_translated{ false };

} // namespace mmx_targeting_fix
