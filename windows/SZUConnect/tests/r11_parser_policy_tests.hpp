#pragma once
#include <optional>
#include <string>
#include <vector>

inline void r11_parser_policy_tests(Harness& t) {
    t.begin("literal-client-configuration");
    const auto page = [](const std::string& script, const std::string& attributes = "") {
        return "<title>深圳大学 SRun login</title><script" + attributes + ">" + script + "</script>";
    };
    const auto literal = [](const std::string& controller) {
        return "var CONFIG={acid:" + controller + ",ip:'192.0.2.10'};";
    };
    for (const std::string controller : {"6", "12", "37", "99999999"}) {
        const auto parsed = wire::client_configuration(page(literal(controller)), controller);
        t.check(parsed.present && parsed.error == PortalError::none && parsed.value &&
            parsed.value->ac_id == controller && parsed.value->ip == "192.0.2.10",
            "unique literal CONFIG retains the actual controller and client IPv4 without device assumptions");
    }
    for (const std::string script : {
            R"(let CONFIG={"ac_id":12,"ip":"192.0.2.10",extra:{nested:[1,2,true,null,-1.2],labels:['CONFIG']},locale:'zh' || 'en'};)",
            R"(const CONFIG={ /* fixture */ acid:'12',ip:'192.0.2.10',}; // fixture comment)",
            R"(var CONFIG = {ip:'192.0.2.10','acid':'00012',flag:false,empty:{},list:[]})"}) {
        const auto parsed = wire::client_configuration(page(script), "12");
        t.check(parsed.value && parsed.value->ac_id == "12" && parsed.value->ip == "192.0.2.10",
            "supported literals, quoting, comments, nested settings and language fallbacks are read without execution");
    }
    for (const std::string attribute : {" type='text/javascript'", " TYPE = \"application/ecmascript\" "})
        t.check(wire::client_configuration(page(literal("12"), attribute), "12").value.has_value(),
            "an inline JavaScript MIME attribute is accepted");
    const std::vector<std::string> rejected_scripts{
        R"(var other={acid:12,ip:'192.0.2.10'};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10',ip:'192.0.2.11'};)",
        R"(var CONFIG={acid:12,ac_id:12,ip:'192.0.2.10'};)",
        R"(var CONFIG={acid:12,ip:currentIP};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.'+'10'};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.\x31\x30'};)",
        R"(var CONFIG={acid:12,ip:'192.000.2.10'};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.256'};)",
        R"(var CONFIG={acid:12,ip:'2001:db8::10'};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10.'};)",
        R"(var CONFIG={acid:12};)",
        R"(var CONFIG={acid:0,ip:'192.0.2.10'};)",
        R"(var CONFIG={acid:123456789,ip:'192.0.2.10'};)",
        R"(var CONFIG={acid:-12,ip:'192.0.2.10'};)",
        R"(var CONFIG={acid:12.0,ip:'192.0.2.10'};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10'}; CONFIG.ip='192.0.2.11';)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10'}; var CONFIG={acid:12,ip:'192.0.2.10'};)",
        R"(// var CONFIG={acid:12,ip:'192.0.2.10'};)",
        R"(var text="var CONFIG={acid:12,ip:'192.0.2.10'};";)",
        R"(function wrapper(){var CONFIG={acid:12,ip:'192.0.2.10'};})",
        R"(var CONFIG={acid:12,ip:`192.0.2.10`};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:[1,2};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:loadValue()};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:(target=1)};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:function(){return 1;}};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:[1,,2]};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:1.2.3};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:1-2};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:'a'|'b'};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:true false};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:{a:1,a:2}};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10','e\x78tra':1};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.1<!--x-->'};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.<!--x-->10'};)",
        R"(var CONFIG={acid:1<!--x-->2,ip:'192.0.2.10'};)",
        R"(var CON<!--x-->FIG={acid:12,ip:'192.0.2.10'};)",
        R"(var \u0043ONFIG={acid:12,ip:'192.0.2.10'};)",
        R"(v\u0061r CONFIG={acid:12,ip:'192.0.2.10'};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10'};other();)",
        R"(var before=1;var CONFIG={acid:12,ip:'192.0.2.10'};)",
        R"(var CONFIG={acid:12,ip:'192.0.2.10'} /* unterminated)",
        "var CONFIG={acid:12,ip:'192.0.2.\n10'};"
    };
    for (const auto& script : rejected_scripts)
        t.check(!wire::client_configuration(page(script), "12").value,
            "ambiguous, forged, malformed or executable CONFIG supplies no client-bound candidate");
    const auto valid = page(literal("12"));
    for (const std::string& html : {
            "<div>" + literal("12") + "</div>", valid + valid,
            "<div title=\"<script>" + literal("12") + "</script>\"></div>",
            "<textarea>" + valid + "</textarea>",
            "<plaintext>" + valid,
            "<title><script>" + literal("12") + "</script></title>",
            "<style>" + valid + "</style>",
            "<noscript>" + valid + "</noscript>",
            "<template>" + valid + "</template>",
            "<template><template>" + valid + "</template>" + valid + "</template>",
            valid + "<input name='ac_id' value='6'>",
            page(literal("12"), " src='https://fixture.invalid/client.js'"),
            page(literal("12"), " type='application/json'"),
            page(literal("12"), " type='text/javascript' defer"),
            page(literal("12"), " type='text/javascript' type='text/javascript'"),
            valid + "<script>CONFIG.ip='192.0.2.11';</script>",
            valid + "<script>window.CONFIG.ip='192.0.2.11';</script>",
            valid + "<script>window['CONFIG'].ip='192.0.2.11';</script>",
            valid + "<script>globalThis[\"CONFIG\"][\"ip\"]='192.0.2.11';</script>",
            valid + "<script>self['CON'+'FIG'].ip='192.0.2.11';</script>",
            valid + R"(<script>window['CON\x46IG'].ip='192.0.2.11';</script>)",
            valid + R"(<script>\u0043ONFIG.ip='192.0.2.11';</script>)",
            valid + R"(<script>C\u004fNFIG.ip='192.0.2.11';</script>)",
            valid + R"(<script>\u{43}ONFIG.ip='192.0.2.11';</script>)",
            valid + "<script>const key='CON'+'FIG';window[key].ip='192.0.2.11';</script>",
            valid + "<script>globalThis[key]['ip']='192.0.2.11';</script>",
            valid + "<script>self[chooseKey()].ip='192.0.2.11';</script>",
            valid + "<script>(window)[key].ip='192.0.2.11';</script>",
            valid + "<script>this[key].ip='192.0.2.11';</script>",
            "<!--" + valid + "-->",
            "<script>" + literal("12"),
            "<script type='text/javascript>" + literal("12") + "</script>"})
        t.check(!wire::client_configuration(html, "12").value,
            "duplicate, external, non-JavaScript, mutated, conflicting or non-script declarations cannot supply proof");
    t.check(wire::client_configuration("<div title=\"a > b\"></div><template>" + valid + "</template>" + valid, "12").value.has_value(),
        "only the active inline CONFIG counts; quoted tag text and inert template contents are not script declarations");
    t.check(wire::client_configuration(valid + R"(<script>const x\u0041=1;</script>)", "12").value.has_value(),
        "an unrelated escaped identifier does not get confused with the CONFIG binding");
    t.check(wire::client_configuration(valid + "<script src='/assets/portal.js'></script><body onload='initialize()'></body>", "12").value.has_value(),
        "ordinary external resources and unrelated event attributes do not disable the literal CONFIG candidate");
    t.check(wire::client_configuration(valid + "<script>const key='unrelated';const values=[1,2];values[key]=1;window['unrelated']=2;</script>", "12").value.has_value(),
        "unrelated local computed access and a known distinct global property retain the literal CONFIG candidate");
    t.check(!wire::client_configuration(valid, "6").value,
        "caller controller disagreement cannot be ignored");
    const auto missing = wire::client_configuration("深圳大学 SRun login ac_id=12", "12");
    t.check(!missing.present && !missing.value && missing.error == PortalError::none,
        "legacy pages without CONFIG remain distinguishable from invalid strong configuration");
    const auto bad = wire::client_configuration(page("var CONFIG={acid:12};"), "12");
    t.check(bad.present && !bad.value && bad.error == PortalError::protocolConflict,
        "an invalid present CONFIG is classified without retaining page text");
    t.check(wire::client_configuration(std::string(wire::kMaxResponseBytes + 1, 'a'), "12").error == PortalError::responseTooLarge,
        "CONFIG parser bounds the complete page");
    t.check(!wire::client_configuration(page("var CONFIG={acid:12,ip:'192.0.2.10',extra:'" + std::string(65536, 'a') + "'};"), "12").value,
        "CONFIG parser separately bounds an inline script");
    t.check(wire::client_configuration(std::string("bad\xff", 4), "12").error == PortalError::response,
        "invalid UTF-8 is rejected before tokenization");
    t.check(!wire::client_configuration(page("var CONFIG={acid:12,ip:'192.0.2.10',extra:" + std::string(33, '[') + "0" + std::string(33, ']') + "};"), "12").value,
        "nested literals have a fixed depth limit");
    std::string many_fields = "var CONFIG={acid:12,ip:'192.0.2.10'";
    for (int i = 0; i < 256; ++i) many_fields += ",f" + std::to_string(i) + ":0";
    t.check(!wire::client_configuration(page(many_fields + "};"), "12").value,
        "top-level literal field count is bounded");
    std::string many_tokens = "var CONFIG={acid:12,ip:'192.0.2.10',extra:[";
    for (int i = 0; i < 9000; ++i) many_tokens += "0,";
    t.check(!wire::client_configuration(page(many_tokens + "]};"), "12").value,
        "token count is bounded independently of byte limits");

    t.begin("r11-routing-priority");
    const auto offline = ConnectivityReport::simple(false, false);
    auto hint = offline;
    hint.microsoft.state = EndpointState::redirect;
    hint.microsoft.campus_area_hint = CampusPortalHint::dormitory;
    CampusDiscovery both{Discovery::confirmed, Discovery::confirmed, true};
    const auto client = resolve_campus_routing(CampusArea::automatic, offline, both, true);
    t.check(client.decision == RoutingDecision::selected && client.choice == CampusArea::teaching &&
        client.require_client_config && !client.allow_unsent_fallback,
        "fresh bound CONFIG selects teaching and requires fresh strong preflight without protocol fallback");
    const auto dormitory = resolve_campus_routing(CampusArea::automatic, hint, both, true, true);
    t.check(dormitory.decision == RoutingDecision::selected && dormitory.choice == CampusArea::dormitory &&
        !dormitory.require_client_config && !dormitory.allow_unsent_fallback,
        "current dormitory hint outranks client CONFIG and historical teaching preference");
    hint.apple.state = EndpointState::redirect; hint.apple.campus_area_hint = CampusPortalHint::teaching;
    t.check(resolve_campus_routing(CampusArea::automatic, hint, both, true, true).decision == RoutingDecision::conflictingHints,
        "a same-round physical hint conflict outranks CONFIG and remembered preference");
    const auto explicit_teaching = resolve_campus_routing(CampusArea::teaching, hint, both, true, true);
    const auto explicit_dormitory = resolve_campus_routing(CampusArea::dormitory, hint, both, true, true);
    t.check(explicit_teaching.choice == CampusArea::teaching && !explicit_teaching.require_client_config &&
        explicit_dormitory.choice == CampusArea::dormitory && !explicit_dormitory.allow_unsent_fallback,
        "explicit region choices remain authoritative and do not invent a CONFIG requirement");
    both.teaching_client_bound = false;
    const auto remembered = resolve_campus_routing(CampusArea::automatic, offline, both, true, true);
    t.check(remembered.choice == CampusArea::teaching && !remembered.require_client_config && !remembered.allow_unsent_fallback,
        "historical teaching preference supplies only a weaker candidate after fresh clues are absent");
    t.check(resolve_campus_routing(CampusArea::automatic, offline, both, true).decision == RoutingDecision::needsSelection,
        "two reachable homepages without a candidate require a visible region selection");
    const auto unbound = resolve_campus_routing(CampusArea::automatic, hint, {Discovery::confirmed, Discovery::confirmed, true}, false, true);
    t.check(unbound.choice == CampusArea::automatic && unbound.allow_unsent_fallback && !unbound.require_client_config,
        "system-path evidence cannot assert physical CONFIG, hints or remembered interface preference");
    t.check(resolve_campus_routing(CampusArea::automatic, offline, {Discovery::unconfirmed, Discovery::confirmed, true}, true, true).choice == CampusArea::automatic,
        "client CONFIG and historical preference cannot replace independent campus HTTPS identity");
    hint.apple = {}; hint.microsoft.campus_area_hint = CampusPortalHint::dormitory;
    t.check(resolve_campus_routing(CampusArea::automatic, hint, {Discovery::confirmed, Discovery::unconfirmed, true}, true, true).decision == RoutingDecision::unconfirmed,
        "dormitory hints require their own current applicable proof even when teaching identity and CONFIG pass");
    hint.microsoft.state = EndpointState::network;
    t.check(resolve_campus_routing(CampusArea::automatic, hint, {Discovery::confirmed, Discovery::unconfirmed}, true, true).choice == CampusArea::teaching,
        "an old hint enum on a non-redirect result is not current routing evidence");

    t.begin("r11-campus-hint-only-paths");
    for (const std::string scheme : {"http", "https"}) for (const std::string path : {
            "/", "/srun_portal_pc", "/srun_portal_phone", "/srun_portal_success", "/srun_portal_success_mobile", "/index_1.html"}) {
        const auto destination = scheme + "://net.szu.edu.cn" + path + "?ignored=fixture";
        t.equal(wire::campus_redirect_hint(destination, std::string(wire::kMicrosoftBase) + "/connecttest.txt"),
            std::optional<CampusPortalHint>(CampusPortalHint::teaching), "known teaching display pages supply only an enum hint");
        if (scheme == "http" || (path != "/" && path != "/srun_portal_pc"))
            t.check(!wire::is_allowed(scheme + "://net.szu.edu.cn" + path),
                "hint support does not expand actual fixed HTTPS request targets");
    }
}
