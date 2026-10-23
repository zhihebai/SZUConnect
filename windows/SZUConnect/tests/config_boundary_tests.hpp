#pragma once
#include <string>
#include <utility>
#include <vector>

inline void config_boundary_tests(Harness& t) {
    const auto page = [](const std::string& script, const std::string& attributes = "") {
        return "<script" + attributes + ">" + script + "</script>";
    };
    const std::string literal = "var CONFIG={acid:12,ip:'192.0.2.10'};";
    const auto valid = page(literal);
    const auto accepts = [](const std::string& html) {
        const auto parsed = wire::client_configuration(html, "12");
        return parsed.present && parsed.error == PortalError::none && parsed.value &&
            parsed.value->ac_id == "12" && parsed.value->ip == "192.0.2.10";
    };

    t.begin("config-number-and-key-boundaries");
    const std::vector<std::pair<std::string, std::string>> rejected_scripts{
        {"unquoted legacy octal controller", "var CONFIG={acid:00012,ip:'192.0.2.10'};"},
        {"unquoted legacy octal field", "var CONFIG={acid:12,ip:'192.0.2.10',01:1};"},
        {"legacy octal nested key aliases", "var CONFIG={acid:12,ip:'192.0.2.10',extra:{01:1,1:2}};"},
        {"numeric and quoted duplicate key", "var CONFIG={acid:12,ip:'192.0.2.10',extra:{1:1,'1':2}};"},
        {"invalid top-level field identifier", "var CONFIG={acid:12,ip:'192.0.2.10',1abc:0};"},
        {"invalid nested field identifier", "var CONFIG={acid:12,ip:'192.0.2.10',extra:{1abc:0}};"},
        {"Unicode numeric key", "var CONFIG={acid:12,ip:'192.0.2.10',extra:{١a:1}};"},
        {"split decimal whitespace", "var CONFIG={acid:12,ip:'192.0.2.10',extra:1 . 2};"},
        {"split decimal comment", "var CONFIG={acid:12,ip:'192.0.2.10',extra:1/* gap */.2};"},
        {"split fraction whitespace", "var CONFIG={acid:12,ip:'192.0.2.10',extra:1. 2};"},
        {"split fraction comment", "var CONFIG={acid:12,ip:'192.0.2.10',extra:1./* gap */2};"},
        {"leading dot expression", "var CONFIG={acid:12,ip:'192.0.2.10',extra:.2};"},
        {"malformed decimal", "var CONFIG={acid:12,ip:'192.0.2.10',extra:1.2.3};"},
        {"nested legacy octal value", "var CONFIG={acid:12,ip:'192.0.2.10',extra:[01]};"},
        {"incomplete hex escape", R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:'\x1'};)"},
        {"invalid hex escape", R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:'\x0Z'};)"},
        {"incomplete Unicode escape", R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:'\u123'};)"},
        {"invalid Unicode escape", R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:'\u12Q4'};)"},
        {"empty Unicode escape", R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:'\u{}'};)"},
        {"out-of-range Unicode escape", R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:'\u{110000}'};)"},
        {"overlong Unicode escape", R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:'\u{1234567}'};)"},
        {"legacy octal string escape", R"(var CONFIG={acid:12,ip:'192.0.2.10',extra:'\01'};)"},
        {"escaped property name", R"(var CONFIG={acid:12,ip:'192.0.2.10','e\x78tra':1};)"}
    };
    for (const auto& [label, script] : rejected_scripts)
        t.check(!wire::client_configuration(page(script), "12").value,
            "CONFIG rejects " + label + " without repairing or executing the script");
    for (const std::string& script : std::vector<std::string>{
            "var CONFIG={ip:'192.0.2.10','acid':'00012',flag:false,empty:{},list:[]}",
            "let CONFIG={ac_id:12,ip:'192.0.2.10',extra:{0:0,1:1,'01':2},$field_2:0};",
            "let CONFIG={ac_id:12,ip:'192.0.2.10',extra:{nested:[0,0.25,1.2,-1.2,true,null],labels:['CONFIG']},locale:'zh' || 'en'};",
            R"(const CONFIG={acid:12,ip:'192.0.2.10',extra:['\x41','\u0041','\u{1F600}']};)"})
        t.check(accepts(page(script)), "supported literal fields and canonical numbers preserve the exact bound candidate");

    std::string fields = "var CONFIG={acid:12,ip:'192.0.2.10',extra:{";
    for (int i = 0; i < 256; ++i) fields += (i ? "," : "") + std::string("f") + std::to_string(i) + ":0";
    t.check(accepts(page(fields + "}};")), "a nested object at its 256-field bound remains supported");
    t.check(!wire::client_configuration(page(fields + ",overflow:0}};"), "12").value,
        "nested objects cannot exceed the literal field bound");
    for (const std::string& script : std::vector<std::string>{
            "var CONFIG={acid:12,ip:'192.0.2.10',9007199254740992:1};",
            "var CONFIG={acid:12,ip:'192.0.2.10',extra:{9007199254740993:1}};",
            "var CONFIG={acid:12,ip:'192.0.2.10',9007199254740992:1,9007199254740993:2};",
            "var CONFIG={acid:12,ip:'192.0.2.10',extra:{9007199254740993:1,'9007199254740992':2}};"})
        t.check(!wire::client_configuration(page(script), "12").value,
            "unsafe numeric keys cannot hide JavaScript Number property-name aliases");
    for (const std::string& script : std::vector<std::string>{
            "var CONFIG={acid:12,ip:'192.0.2.10',9007199254740991:1};",
            "var CONFIG={acid:12,ip:'192.0.2.10',extra:{9007199254740991:1}};",
            "var CONFIG={acid:12,ip:'192.0.2.10','9007199254740993':1,extra:{'9007199254740992':2,'999999999999999999999999':3},number:9007199254740993};"})
        t.check(accepts(page(script)),
            "safe integer keys and quoted numeric strings retain literal compatibility without limiting unused number values");

    t.begin("config-computed-receiver-boundaries");
    const std::vector<std::string> mutations{
        "window?.[key].ip='192.0.2.11';",
        "((window))[key].ip='192.0.2.11';",
        "window.window[key].ip='192.0.2.11';",
        "globalThis['window'][key].ip='192.0.2.11';",
        "((self))?.[key].ip='192.0.2.11';",
        "window?.['window']?.[key].ip='192.0.2.11';",
        "globalThis?.window?.[key].ip='192.0.2.11';",
        "window['window']['self'][key].ip='192.0.2.11';",
        "(globalThis['window'])?.[key].ip='192.0.2.11';",
        "this[key].ip='192.0.2.11';",
        "window?.['CON'+'FIG'].ip='192.0.2.11';",
        R"(window?.['CON\x46IG'].ip='192.0.2.11';)",
        "Object.assign(window?.[key],{ip:'192.0.2.11'});",
        "Object.assign(globalThis?.['window']?.[key],{ip:'192.0.2.11'});"
    };
    for (const auto& script : mutations) {
        t.check(!wire::client_configuration(valid + page(script), "12").value,
            "optional or chained global computed access cannot preserve CONFIG authority: " + script);
        t.check(!wire::client_configuration(page(script) + valid, "12").value,
            "computed CONFIG ambiguity is rejected before as well as after the declaration");
    }
    for (const int depth : {4, 40}) {
        const auto script = std::string(static_cast<std::size_t>(depth), '(') + "window" +
            std::string(static_cast<std::size_t>(depth), ')') + "[key].ip='192.0.2.11';";
        t.check(!wire::client_configuration(valid + page(script), "12").value,
            "global receiver nesting cannot turn dynamic CONFIG ambiguity into authority");
        t.check(!wire::client_configuration(page(script) + valid, "12").value,
            "receiver depth exhaustion also fails closed before the CONFIG declaration");
    }
    for (const std::string& script : std::vector<std::string>{
            "const key='unrelated';const values=[1,2];values[key]=1;",
            "window?.['unrelated'];globalThis['window']['unrelated']=3;",
            "const value=window?.location;const list=[1,2];list?.[0];",
            R"(const x\u0041=1;)"})
        t.check(accepts(valid + page(script)),
            "unrelated identifiers, local indexing and known distinct global keys remain compatible");
    t.check(!wire::client_configuration(valid + page("var re=/[//]/;window[key].ip='192.0.2.11';"), "12").value,
        "unsupported regular-expression syntax cannot disguise a visible CONFIG mutation as a comment");
    t.check(accepts(page("var CONFIG={acid:12,ip:'192.0.2.10',path:'/assets/portal.js',url:'https://fixture.invalid/path'};")),
        "slashes inside literal paths and URLs remain ordinary string contents");
    t.check(accepts(valid + page("/* fixture / comment */ const unrelated=1; // ordinary comment")),
        "real line and block comments remain compatible without regular-expression evaluation");
    for (const unsigned codepoint : {0x00a0u, 0xfeffu, 0x1680u, 0x2000u, 0x2001u, 0x2002u,
            0x2003u, 0x2004u, 0x2005u, 0x2006u, 0x2007u, 0x2008u, 0x2009u, 0x200au,
            0x202fu, 0x205fu, 0x3000u}) {
        std::string separator;
        if (codepoint <= 0x7ffu) {
            separator.push_back(static_cast<char>(0xc0u | (codepoint >> 6)));
            separator.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else {
            separator.push_back(static_cast<char>(0xe0u | (codepoint >> 12)));
            separator.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
            separator.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
        t.check(!wire::client_configuration(valid + page("const unrelated=1;" + separator +
                "window[key].ip='192.0.2.11';"), "12").value,
            "ECMAScript whitespace cannot merge a global receiver into an unrelated identifier");
        t.check(accepts(page("var" + separator + "CONFIG={acid:12,ip:'192.0.2.10'};")),
            "supported ECMAScript whitespace separates a valid declaration keyword and CONFIG");
    }

    t.begin("config-html-execution-contexts");
    const std::vector<std::pair<std::string, std::string>> inert_or_ambiguous{
        {"MathML script", "<math>" + valid + "</math>"},
        {"SVG script", "<svg>" + valid + "</svg>"},
        {"frameset contents", "<frameset>" + valid + "</frameset>"},
        {"frameset insertion mode", "<frameset></frameset>" + valid},
        {"double-escaped script", "<script><!-- <script></script>" + valid + " --> </script>"},
        {"double-escaped template script", "<template><script><!-- <script></script></template>" + valid + " --> </script></template>"},
        {"single-quoted attribute", "<div title='<script>var CONFIG={acid:12,ip:\"192.0.2.10\"};</script>'></div>"},
        {"declaration attribute", "<!DOCTYPE html PUBLIC \"<script>" + literal + "</script>\">"},
        {"processing-like attribute", "<?fixture value=\"<script>" + literal + "</script>\">"},
        {"invalid closing-tag attribute", "</? value=\"<script>" + literal + "</script>\">"},
        {"xmp raw text", "<xmp>" + valid + "</xmp>"},
        {"iframe fallback", "<iframe>" + valid + "</iframe>"},
        {"noembed raw text", "<noembed>" + valid + "</noembed>"},
        {"noframes raw text", "<noframes>" + valid + "</noframes>"},
        {"SVG template script", valid + "<svg><template><script>window[key].ip='192.0.2.11';</script></template></svg>"},
        {"MathML template script", valid + "<math><template><script>window[key].ip='192.0.2.11';</script></template></math>"},
        {"processing-like markup followed by an active script", valid + "<?fixture value=\"><script>window[key].ip='192.0.2.11';</script>\">"},
        {"bogus declaration followed by an active script", valid + "<!fixture value=\"><script>window[key].ip='192.0.2.11';</script>\">"}
    };
    for (const auto& [label, html] : inert_or_ambiguous)
        t.check(!wire::client_configuration(html, "12").value,
            label + " cannot manufacture an active inline CONFIG declaration");
    for (const std::string& root : std::vector<std::string>{"svg", "math"}) {
        for (const std::string& element : std::vector<std::string>{"title", "style", "plaintext"}) {
            const auto foreign = "<" + root + "><" + element +
                "><script>window[key].ip='192.0.2.11';</script></" + element + "></" + root + ">";
            t.check(!wire::client_configuration(valid + foreign, "12").value,
                "HTML raw-text names cannot hide a script in foreign content: " + root + "/" + element);
            t.check(accepts(valid + "<template>" + foreign + "</template>"),
                "an outer HTML template keeps the entire foreign subtree inert: " + root + "/" + element);
        }
    }
    const auto mutation = page("window[key].ip='192.0.2.11';");
    for (const auto& html : std::vector<std::string>{
            valid + "<!-- x --!>" + mutation + "<!-- y -->",
            valid + "<!-->" + mutation + "<!-- later -->",
            valid + "<!--->" + mutation + "<!-- later -->",
            valid + "<!-- outer <!-->" + mutation + " -->",
            valid + "<!DOCTYPE html invalid \">" + mutation + "\">",
            valid + "<div title=x\">" + mutation + "</div>\">"})
        t.check(!wire::client_configuration(html, "12").value,
            "ambiguous comments or tag attributes cannot hide a later active CONFIG mutation");
    for (const std::string& separator : std::vector<std::string>{"\xE2\x80\xA8", "\xE2\x80\xA9"}) {
        t.check(!wire::client_configuration(valid + page("// fixture comment" + separator +
                "window[key].ip='192.0.2.11';"), "12").value,
            "JavaScript Unicode line separators end a comment before a visible global mutation");
        t.check(accepts(valid + page("// fixture comment" + separator + "const unrelated=1;")),
            "Unicode comment line endings without CONFIG access preserve normal resource compatibility");
    }
    for (const std::string& suffix : std::vector<std::string>{
            "<template><svg>" + valid + "</svg><frameset></frameset></template>",
            "<svg><path d='M0 0'/></svg><math><mi>x</mi></math>",
            "<svg><title>fixture graphic</title></svg><math><mi>x</mi></math>",
            "<svg/><math/>",
            "<script src='/assets/portal.js'></script><body onload='initialize()'></body>",
            "<!-- ordinary page comment -->",
            "<template><svg><template><script>window[key].ip='192.0.2.11';</script></template></svg></template>",
            "<template><math><template><script>window[key].ip='192.0.2.11';</script></template></math></template>",
            "<!DOCTYPE html>",
            "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\" \"https://fixture.invalid/html4.dtd\">",
            "<?fixture value='plain'>",
            "<!fixture value='plain'>"})
        t.check(accepts(valid + suffix), "ordinary foreign graphics, inert markup and resources coexist with active CONFIG");
    for (const std::string& attributes : std::vector<std::string>{" type='text/javascript'", " TYPE = \"application/ecmascript\" "})
        t.check(accepts(page(literal, attributes)), "supported JavaScript MIME attributes remain compatible");
    t.check(accepts("<div title=\"a > b\"></div><template>" + valid + "</template>" + valid),
        "quoted tag delimiters and inert declarations do not hide the active literal");
    t.check(accepts(page("var CONFIG={acid:12,ip:'192.0.2.10',extra:'</scripture>'};")),
        "a close-like script prefix inside a literal is not an HTML end tag");
    t.check(!wire::client_configuration(valid + "<math><svg></math></svg>", "12").value,
        "a foreign-content stack must close consistently before granting a candidate");
}
