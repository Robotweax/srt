// SPDX-License-Identifier: MIT
#pragma once
#include "compat/hybrid_native.hpp"
#include "compat/hybrid_reporting.hpp"
#include "compat/hybrid_detector.hpp"
#include "compat/hybrid_return.hpp"
#include "compat/hybrid_automatic_types.hpp"

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace robotweax::srt::compat::hybrid {
class AutomaticFallback;
// Caller policy owner with optional read-only reports and path assessments.
// Does not select DATA paths, ACK data or initiate fallback automatically.
class ReceiverBridge {
public:
    struct Configuration {
        ReportingConfiguration reporting;
        std::optional<DetectionConfiguration> detection;
        std::uint32_t retry_interval_us = 100'000, round_timeout_us = 500'000,
                      operation_timeout_us = 2'000'000,
                      query_interval_us = 500'000,
                      confirmation_lifetime_us = 1'000'000;
        std::uint32_t maximum_sends = 4, maximum_rounds = 4;
    };
    enum class Phase {
        observing,
        requesting,
        reconciling,
        known,
        denied,
        unknown,
        closed
    };
    enum class RequestState { none, pending, confirmed, denied, unknown };
    enum class Admission { accepted, busy, invalid, already_unicast, closed };
    struct Snapshot {
        Phase phase = Phase::observing;
        bool attached = false, closed = false, observed = false, fresh = false,
             has_commit = false;
        Mode mode = Mode::multicast;
        Outcome outcome = Outcome::ok;
        RequestState fallback = RequestState::none;
        Outcome fallback_outcome = Outcome::ok;
        RequestState returning = RequestState::none;
        Outcome return_outcome = Outcome::ok;
        Mode return_target = Mode::overlap;
        std::uint64_t revision = 0, cursor = 0, retained_floor = 0,
                      commit_position = 0, confirmation_origin_us = 0,
                      request_serial = 0, next_wakeup_us = UINT64_MAX;
        std::uint64_t report_attempts = 0, reports_queued = 0,
                      reports_submitted = 0, reports_skipped = 0;
        std::uint64_t attempts = 0, refused = 0, rounds = 0, queries = 0,
                      confirmed = 0, stale = 0, unmatched = 0, duplicates = 0,
                      conflicts = 0, ignored = 0, exhausted = 0;
    };
    [[nodiscard]] static std::shared_ptr<ReceiverBridge> create(
        std::shared_ptr<NativeService>, Configuration) noexcept;
    ~ReceiverBridge();
    // Host supplies its validated absolute merge cursor. No cursor or DATA
    // state is inferred from an authenticated peer policy assertion.
    [[nodiscard]] Admission request_fallback(
        std::uint64_t next_position) noexcept;
    [[nodiscard]] std::optional<CoverageProof> coverage_proof(
        std::uint64_t through, std::uint32_t lifetime_us) const noexcept;
    [[nodiscard]] Admission request_overlap() noexcept;
    [[nodiscard]] Admission request_multicast(const CoverageProof&) noexcept;
    void pump() noexcept;
    void close() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;
    [[nodiscard]] std::optional<PathDetector::Assessment>
    path_assessment() const noexcept;
    [[nodiscard]] std::optional<ReceiveDataProgress>
    data_progress() const noexcept;
    [[nodiscard]] std::optional<ReportObservation> peer_report(
        std::uint32_t maximum_age) const noexcept;

private:
    friend class NativeService;
    friend class AutomaticFallback;
    [[nodiscard]] bool claim_automatic() noexcept;
    [[nodiscard]] std::optional<AutomaticRequest>
    automatic_candidate() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> admit_automatic(
        const AutomaticRequest&, const AutomaticCursor&, std::uint64_t ticket,
        std::uint32_t cursor_lifetime_us) noexcept;
    struct AutomaticReceipt {
        std::uint64_t ticket = 0;
        RequestState result = RequestState::none;
        Outcome outcome = Outcome::ok;
    };
    [[nodiscard]] AutomaticReceipt automatic_receipt() const noexcept;
    explicit ReceiverBridge(
        std::shared_ptr<NativeService>, Configuration) noexcept;
    void work() noexcept;
    void report() noexcept;
    void detect() noexcept;
    void event(const NativeService::Event&, std::uint64_t) noexcept;
    void start(bool fallback, std::uint64_t position, std::uint64_t now,
        std::uint64_t origin, std::uint64_t not_after = UINT64_MAX,
        std::uint64_t automatic_ticket = 0, Mode target = Mode::unicast,
        std::uint64_t proof_begin = 0, std::uint32_t proof_interval = 0,
        std::uint64_t proof_observed = 0) noexcept;
    void round(Kind, std::uint64_t) noexcept;
    void finish(Phase, std::uint64_t) noexcept;
    std::shared_ptr<NativeService> service_;
    Configuration configuration_;
    mutable std::mutex mutex_;
    std::atomic_bool cancelled_ = false;
    ReportTracker peer_report_;
    std::unique_ptr<PathDetector> detector_;
    std::uint64_t next_report_ = 0, pending_report_ = 0;
    Snapshot state_;
    Context context_;
    Message request_;
    struct Posted {
        std::uint64_t position = 0, at = 0, not_after = UINT64_MAX,
                      automatic_ticket = 0;
        Mode target = Mode::unicast;
        std::uint64_t proof_begin = 0, proof_observed = 0;
        std::uint32_t proof_interval = 0;
    };
    std::optional<Posted> posted_;
    bool automatic_claimed_ = false;
    std::uint64_t manual_epoch_ = 0, active_automatic_ticket_ = 0;
    AutomaticReceipt automatic_receipt_;
    std::uint64_t generation_ = 0, next_serial_ = 1, highest_reply_ = 0,
                  last_now_ = 0, deadline_ = 0, round_deadline_ = 0,
                  next_send_ = 0, next_query_ = 0, first_send_ = 0,
                  requested_position_ = 0;
    std::uint32_t sends_ = 0, rounds_ = 0;
    bool want_return_ = false;
    Mode target_mode_ = Mode::unicast;
    std::uint64_t expected_revision_ = 0, proof_begin_ = 0, proof_observed_ = 0;
    std::uint32_t proof_interval_ = 0;
    bool active_ = false, want_fallback_ = false, confirmed_ = false,
         sent_ = false, queued_ = false;
};
}
#endif
