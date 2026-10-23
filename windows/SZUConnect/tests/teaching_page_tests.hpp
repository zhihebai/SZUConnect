#pragma once
#include <string>
#include <vector>

void test_teaching_page_reuse(Harness& t) {
    t.begin("teaching-page-same-response-reuse");
    const std::string valid = "<script>var CONFIG={acid:12,ip:'192.0.2.7'};</script>";
    std::vector<std::string> pages{
        valid,
        "<!DOCTYPE html>" + valid,
        "<input name='ac_id' value='12'>" + valid,
        "<script>const CONFIG={ip:'192.0.2.7',ac_id:'00012',nested:[1,2,{x:true}]};</script>",
        "<script>let CONFIG={acid:7,ip:'192.0.2.8'};</script>",
        "<script>var CONFIG={acid:99999999,ip:'192.0.2.9'};</script>",
        "<input name='ac_id' value='12'>",
        "<html><body>No controller</body></html>",
        "<input name='ac_id' value='7'>" + valid,
        valid + "<script>var ac_id=7;</script>",
        valid + "<script>window['CONFIG'].ip='192.0.2.8';</script>",
        valid + "<script>window?.['CONFIG'].ip='192.0.2.8';</script>",
        "<script>var CONFIG={acid:12,ip:'192.0.2.7',ip:'192.0.2.8'};</script>",
        "<script>var CONFIG={acid:12,ac_id:12,ip:'192.0.2.7'};</script>",
        "<script>var CONFIG={acid:12,ip:'192.0.2.7',1e2:3};</script>",
        "<script>var CONFIG={acid:12,ip:'192.0.2.7',x:1 2};</script>",
        "<script>var CONFIG={acid:12,ip:'192.0.2.7',x:9007199254740993};</script>",
        "<script>var CONFIG={acid:12,ip:'192.0.2.7',x:Math.random()};</script>",
        "<script>var CONFIG={acid:12,ip:'192.0.2.7',x:1/2};</script>",
        "<script>var CONFIG={acid:12,ip:'192.0.2.7',x:'unterminated};</script>",
        "<script>var CONFIG={acid:12,ip:'192.0.2.7',x:null};</script><frameset></frameset>",
        "<math><script>var CONFIG={acid:12,ip:'192.0.2.7'};</script></math>",
        "<script><!--<script>var CONFIG={acid:12,ip:'192.0.2.7'};</script></script>",
        "<template>" + valid + "</template>",
        "<script src='config.js'>var CONFIG={acid:12,ip:'192.0.2.7'};</script>",
        valid + "<script>var CONFIG={acid:12,ip:'192.0.2.7'};</script>",
        "<script>var CONFIG={acid:0,ip:'192.0.2.7'};</script>",
        "<script>var CONFIG={acid:12,ip:'not-an-ip'};</script>",
        std::string("<script>var ac_id=12;</script>\xc0\xaf"),
        std::string(wire::kMaxResponseBytes + 1, 'x')
    };
    for (const auto& page : pages) {
        const auto old_controller = wire::ac_id(page);
        const auto combined = wire::teaching_page(page);
        t.check(combined.controller.value == old_controller.value && combined.controller.error == old_controller.error,
                "Combined parser retains controller values and error classification");
        if (old_controller.error == PortalError::none) {
            const auto old_config = wire::client_configuration(page, old_controller.value);
            t.check(combined.configuration.present == old_config.present && combined.configuration.error == old_config.error &&
                    combined.configuration.value.has_value() == old_config.value.has_value(),
                    "Combined parser retains CONFIG presence, rejection and error classification");
            if (old_config.value)
                t.check(combined.configuration.value->ip == old_config.value->ip &&
                        combined.configuration.value->ac_id == old_config.value->ac_id,
                        "Combined parser retains the exact same-response literals");
        } else {
            t.check(!combined.configuration.value, "A rejected controller never produces a bound CONFIG");
        }
    }
    const auto first = wire::teaching_page(valid);
    const auto second = wire::teaching_page("<script>var CONFIG={acid:7,ip:'192.0.2.8'};</script>");
    t.check(first.configuration.value && first.configuration.value->ac_id == "12" && first.configuration.value->ip == "192.0.2.7" &&
            second.configuration.value && second.configuration.value->ac_id == "7" && second.configuration.value->ip == "192.0.2.8",
            "Independent responses never share controller or address authority");
    t.check(!wire::client_configuration(valid, "7").value && !wire::client_configuration(valid, "00012").value,
            "The original public parser still independently rejects mismatched controller arguments");
    t.check(!wire::teaching_page(valid + "<input name='ac_id' value='7'>").configuration.value,
            "A preceding success cannot authorize a later conflicting response");
}
