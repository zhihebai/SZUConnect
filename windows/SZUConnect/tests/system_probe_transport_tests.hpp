#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include "szu/portal_wire.hpp"
#include "szu/win/system_probe.hpp"

namespace systemprobetransport {
class Transport {
public:
    szu::win::HttpResponse get(const szu::win::HttpRequest& request, const szu::CancelToken& token) {
        const bool apple = request.url == "https://www.apple.com/library/test/success.html";
        const bool microsoft = request.url == "http://www.msftconnecttest.com/connecttest.txt";
        const auto registration = token.on_cancel([this] {
            std::lock_guard<std::mutex> lock(mutex_);
            cv_.notify_all();
        });
        {
            std::unique_lock<std::mutex> lock(mutex_);
            valid_requests = valid_requests && (apple || microsoft) && request.proxy == szu::win::ProxyMode::system &&
                request.timeout_ms == 6000 && request.max_bytes == szu::wire::kMaxProbeBytes &&
                !request.allow_cookies && !request.headers_only;
            ++started_; cv_.notify_all();
            if (!block_both && apple == fast_apple) {
                cv_.wait(lock, [&] { return started_ == 2 || token.cancelled(); });
                fast_returned_ = true; cv_.notify_all();
            } else {
                cv_.wait(lock, [&] { return release_slow_ || token.cancelled(); });
            }
            if (token.cancelled()) ++cancelled;
        }
        token.remove(registration);
        szu::win::HttpResponse response;
        response.status = 200; response.elapsed = apple ? 0.011 : 0.023;
        response.body = apple ? "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>" : "Microsoft Connect Test";
        if (!block_both && apple == fast_apple) {
            response.error = fast_error;
            if (fast_redirect) { response.status = 302; response.campus_area_hint = szu::CampusPortalHint::teaching; }
        }
        // Deliberately return a success body after cancellation as a native handle may
        // finish concurrently. The production probe must discard that peer result.
        return response;
    }
    bool wait_started(bool fast_returned = false) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(2), [&] { return started_ == 2 && (!fast_returned || fast_returned_); });
    }
    void release() {
        { std::lock_guard<std::mutex> lock(mutex_); release_slow_ = true; }
        cv_.notify_all();
    }
    bool valid_requests = true, block_both = false, fast_apple = true, fast_redirect = false;
    szu::win::HttpError fast_error = szu::win::HttpError::none;
    std::atomic<int> cancelled{0};
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int started_ = 0;
    bool fast_returned_ = false, release_slow_ = false;
};
}  // namespace systemprobetransport

template<class Check> void run_system_probe_transport_tests(Check check) {
    using namespace szu;
    using namespace szu::win;
    using namespace std::chrono_literals;
    for (const bool fast_apple : {true, false}) {
        systemprobetransport::Transport transport; transport.fast_apple = fast_apple;
        CancelToken token;
        auto task = std::async(std::launch::async, [&] {
            return probe_system_connectivity(ProbeMode::quick, token,
                [&](const HttpRequest& request, const CancelToken& child) { return transport.get(request, child); });
        });
        const bool started = transport.wait_started(true);
        const bool finished = task.wait_for(2s) == std::future_status::ready;
        if (!finished) transport.release();
        const auto report = task.get();
        check(started && finished && report && report->mode == ProbeMode::quick && report->online(),
            "native system quick scheduler starts both fixed HTTP probes and completes as soon as either succeeds");
        check(report && (fast_apple ? report->apple : report->microsoft).state == EndpointState::success &&
            (fast_apple ? report->microsoft : report->apple).state == EndpointState::notCompleted && transport.cancelled == 1,
            "quick success cancels the other native request and suppresses its late success as not waited");
        check(transport.valid_requests && !token.cancelled() && report && !report->has_recovery_evidence(),
            "system probes retain fixed targets, system proxy, TLS URL, timeout, bounded body and no cookies without recovery evidence");
    }
    {
        systemprobetransport::Transport transport; CancelToken token;
        auto task = std::async(std::launch::async, [&] {
            return probe_system_connectivity(ProbeMode::complete, token,
                [&](const HttpRequest& request, const CancelToken& child) { return transport.get(request, child); });
        });
        const bool started = transport.wait_started(true);
        const bool waits = task.wait_for(30ms) == std::future_status::timeout;
        check(started && waits && transport.cancelled == 0,
            "native system complete scheduler waits for both HTTP responses after the first success without cancelling its peer");
        transport.release(); const auto report = task.get();
        check(report && report->mode == ProbeMode::complete && report->apple.succeeded() && report->microsoft.succeeded() &&
            report->apple.elapsed == 0.011 && report->microsoft.elapsed == 0.023 && transport.valid_requests,
            "complete system results preserve both native HTTP responses, independent timings and actual scope");
    }
    for (const bool redirect : {false, true}) {
        systemprobetransport::Transport transport;
        transport.fast_error = redirect ? HttpError::none : HttpError::timeout; transport.fast_redirect = redirect;
        CancelToken token;
        auto task = std::async(std::launch::async, [&] {
            return probe_system_connectivity(ProbeMode::quick, token,
                [&](const HttpRequest& request, const CancelToken& child) { return transport.get(request, child); });
        });
        const bool started = transport.wait_started(true);
        const bool waits = task.wait_for(30ms) == std::future_status::timeout;
        check(started && waits && transport.cancelled == 0,
            "system quick timeout or campus redirect cannot cancel its pending peer or select an authentication protocol");
        transport.release(); const auto report = task.get();
        check(report && report->microsoft.succeeded() && report->apple.state ==
            (redirect ? EndpointState::redirect : EndpointState::timeout) &&
            !report->apple.campus_area_hint && !report->microsoft.campus_area_hint && !report->has_recovery_evidence(),
            "system column retains failures for display but drops every campus protocol hint");
    }
    for (const auto mode : {ProbeMode::quick, ProbeMode::complete}) {
        systemprobetransport::Transport transport; transport.block_both = true; CancelToken token;
        auto task = std::async(std::launch::async, [&] {
            return probe_system_connectivity(mode, token,
                [&](const HttpRequest& request, const CancelToken& child) { return transport.get(request, child); });
        });
        const bool started = transport.wait_started(); token.cancel();
        const bool finished = task.wait_for(2s) == std::future_status::ready;
        if (!finished) transport.release();
        const auto report = task.get();
        check(started && finished && !report && transport.cancelled == 2,
            "parent cancellation closes both pending system HTTP probes and publishes no late successful report in either scope");
    }
    {
        CancelToken token; token.cancel(); int calls = 0;
        const auto report = probe_system_connectivity(ProbeMode::complete, token,
            [&](const HttpRequest&, const CancelToken&) { ++calls; return HttpResponse{}; });
        check(!report && calls == 0, "a cancelled native system request never starts either HTTP target");
    }
}
