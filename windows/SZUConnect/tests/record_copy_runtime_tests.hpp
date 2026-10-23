#pragma once
#include "shared_copy.hpp"
#include "wifi_runtime_tests.hpp"

template<class Check> void run_record_copy_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    const std::string previous_offline = "外网检测这次没通过，稍后会再测。";
    const std::string previous_tls = "服务器证书没验证通过，登录已停止。检查电脑时间和代理设置。";
    const std::string previous_uncertain = "登录已经提交，结果还没确认。先做一次完整检测。";
    records::EventLog stored;
    stored.append(100, previous_offline, true);
    stored.append(101, "登录没有完成：" + previous_tls, true);
    stored.append(102, previous_uncertain, false);
    const auto original = json::serialize(stored.to_json());
    for (const auto mode : {RuntimeMode::normal, RuntimeMode::observeOnly}) {
        auto services = std::make_unique<wifiruntime::Fake>(); auto* fake = services.get();
        fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
        fake->files[L"events.json"] = original;
        AppController app(nullptr, mode, std::move(services)); app.start();
        const auto& events = app.events().items();
        check(events.size() == 3 && events[0].message == previous_offline && events[0].failure &&
            events[1].message == "登录没有完成：" + previous_tls && events[1].failure &&
            events[2].message == previous_uncertain && !events[2].failure,
            "shared copy changes retain original event messages and durable failure classifications");
        check(fake->files.at(L"events.json") == original,
            "loading historical event wording does not rewrite the saved log");
        check(app.is_failure_event(previous_offline) && app.is_failure_event("登录没有完成：" + previous_tls) &&
            app.is_failure_event("教学／办公区登录被拒绝，这个区域的自动重连先暂停了（仅限当前网络）。"),
            "historical offline, wrapped login failure and region refusal wording remain recognized");
        check(app.is_failure_event(std::string(copy::text("event.offlineAfterOnline"))) &&
            app.is_failure_event(copy::format("event.failed", {{"reason", std::string(copy::text("portalError.tls"))}})),
            "current reviewed detection and login failures remain recognized");
        check(!app.is_failure_event(previous_uncertain) &&
            !app.is_failure_event("登录没有完成：" + previous_uncertain) &&
            !app.is_failure_event("登录未完成：登录请求已发出，但暂未收到结果。请先做一次完整检测。") &&
            !app.is_failure_event(copy::format("event.failed", {{"reason", std::string(copy::text("portalError.authenticationUncertain"))}})),
            "old or current event prefixes never turn an unknown submission into a confirmed failure");
        check(!app.is_failure_event(std::string(copy::text("event.online"))) &&
            !app.is_failure_event(std::string(copy::text("event.uncertainSettled"))) &&
            !app.is_failure_event(std::string(copy::text("status.cancelled.detail"))),
            "success, settled uncertainty and cancellation are not confirmed failures");
        app.shutdown();
        check(fake->evidence().fake_auth_submissions == 0 && fake->evidence().network_calls == 0 &&
            fake->evidence().production_storage_accesses == 0 && fake->evidence().vault_accesses == 0,
            "historical message regression uses only isolated storage and no authentication");
        if (mode == RuntimeMode::observeOnly)
            check(fake->files.at(L"events.json") == original, "read-only shutdown preserves all historical event bytes");
    }
}
