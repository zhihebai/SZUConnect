import AppKit
import SwiftUI

@main @MainActor
struct ProxyAssistantPresentationTests {
    static func main() {
        setbuf(stdout, nil)
        var count = 0
        func check(_ value: Bool, _ message: String) {
            guard value else { fatalError(message) }; count += 1; print("PASS: " + message)
        }
        let mixed = ProxyAssistantCheckPresentation(campusDirect: "reachable", campusSystem: "tls",
            publicDirect: "timeout", publicSystem: "notRun")
        check(mixed.rows.count == 2 && mixed.rows.map(\.id) == ["campus", "public"], "Two fixed targets retain their table order")
        check(mixed.rows[0].direct == .reachable && mixed.rows[0].system == .tls,
              "Campus direct and system responses remain distinct actual outcomes")
        check(mixed.rows[1].direct == .timeout && mixed.rows[1].system == .notRun,
              "Public timeout and untested system path are not promoted to success")
        check(mixed.hasSingleTimeout, "A single timeout permits the browser comparison suggestion")
        let twoTimeouts = ProxyAssistantCheckPresentation(campusDirect: "timeout", campusSystem: "reachable",
            publicDirect: "timeout", publicSystem: "reachable")
        check(!twoTimeouts.hasSingleTimeout, "Several timeouts are not described as a single failed item")
        let cancelled = ProxyAssistantCheckPresentation(campusDirect: "cancelled", campusSystem: "notRun",
            publicDirect: "notRun", publicSystem: "notRun")
        check(cancelled.rows[0].direct == .cancelled && !cancelled.hasSingleTimeout,
              "Cancellation keeps its actual token without inventing timeout advice")
        for result in ProxyAssistantCheckPresentation.Result.allCases {
            let value = ProxyAssistantCheckPresentation(campusDirect: result.rawValue, campusSystem: result.rawValue,
                publicDirect: result.rawValue, publicSystem: result.rawValue)
            check(value.rows.allSatisfy { $0.direct == result && $0.system == result },
                  "All table cells preserve reviewed state " + result.rawValue)
        }
        let unknown = ProxyAssistantCheckPresentation(campusDirect: "unreviewed fixture", campusSystem: "",
            publicDirect: "fixture-private-state", publicSystem: "reachable")
        check(unknown.rows[0].direct == .notRun && unknown.rows[0].system == .notRun
              && unknown.rows[1].direct == .notRun && unknown.rows[1].system == .reachable,
              "Unreviewed states cannot leak into text keys or turn into positive results")
        check(ProxyAssistantLayout.sheetHeight(content: 220, header: 64, screen: 900) == 285,
              "Short assistant content shrinks to its measured text height")
        check(ProxyAssistantLayout.sheetHeight(content: 900, header: 64, screen: 900) == 680,
              "Long assistant instructions are capped for scrolling")
        check(ProxyAssistantLayout.sheetHeight(content: 900, header: 64, screen: 620) == 540,
              "A short screen leaves room around the scrolling assistant")
        check(ProxyAssistantLayout.sheetHeight(content: .nan, header: 64, screen: 900) == 440,
              "Unmeasured or invalid heights use a finite initial size")
        check(ProxyAssistantLayout.sheetHeight(content: 0, header: 0, screen: 620) == 440,
              "Initial preference collection does not force the maximum modal height")
        // Native fitting checks use detached hosting views; no application, window,
        // pasteboard, preferences, file transaction or network work is started.
        let longTitle = String(repeating: "通用方法", count: 6)
        let wideHeader = NSHostingView(rootView: ProxyAssistantSelectedClientRow(title: longTitle, change: {}).frame(width: 500))
        let narrowHeader = NSHostingView(rootView: ProxyAssistantSelectedClientRow(title: longTitle, change: {}).frame(width: 240))
        check(wideHeader.fittingSize.height.isFinite && narrowHeader.fittingSize.height.isFinite
              && narrowHeader.fittingSize.height > wideHeader.fittingSize.height,
              "Native selected-client header keeps its whole action on a separate narrow line")
        let target = "IP-CIDR（网段）：172.30.255.42/32"
        let wideTarget = NSHostingView(rootView: ProxyAssistantTargetRow(text: target, buttonTitle: "复制网段", copied: false,
            identifier: "fixture.copy", copy: {}).frame(width: 500))
        let narrowTarget = NSHostingView(rootView: ProxyAssistantTargetRow(text: target, buttonTitle: "已复制", copied: true,
            identifier: "fixture.copy", copy: {}).frame(width: 180))
        check(wideTarget.fittingSize.height.isFinite && narrowTarget.fittingSize.height.isFinite
              && narrowTarget.fittingSize.height > wideTarget.fittingSize.height,
              "Native address row stacks the complete copied control when width is insufficient")
        let longResults = ProxyAssistantCheckPresentation(campusDirect: "securityPolicy", campusSystem: "redirect",
            publicDirect: "authentication", publicSystem: "httpStatus")
        for width: CGFloat in [440, 500] {
            let table = NSHostingController(rootView: ProxyAssistantCheckTable(results: longResults))
            let fitting = table.sizeThatFits(in: NSSize(width: width, height: CGFloat.greatestFiniteMagnitude))
            check(fitting.width.isFinite && fitting.height.isFinite && fitting.height > 0 && fitting.width <= width + 0.5,
                  "Actual assistant result table fits width \(width) without a horizontal expansion")
        }
        for key in ["compat.row.campus", "compat.row.public"] {
            let title = NSHostingController(rootView: ProxyAssistantCheckTable.rowTitle(key))
            let natural = title.sizeThatFits(in: NSSize(width: 1_000, height: CGFloat.greatestFiniteMagnitude))
            let constrained = title.sizeThatFits(in: NSSize(width: 1, height: CGFloat.greatestFiniteMagnitude))
            check(abs(natural.height - constrained.height) < 0.5 && abs(natural.width - constrained.width) < 0.5,
                  "Actual row title retains one intrinsic line rather than one character per line: " + key)
            let text = SharedCopy.t(key) as NSString
            let largeWidth = text.size(withAttributes: [.font: NSFont.systemFont(ofSize: SharedTokens.fontCalloutSize * 1.5, weight: .medium)]).width
            print("Native title metrics: \(key), base width \(natural.width), 150 percent width \(largeWidth)")
            check(largeWidth >= natural.width * 1.4,
                  "150 percent native title retains its full text width: " + key)
            if key == "compat.row.campus" {
                check(largeWidth > 80,
                      "The campus title requires more than the former 80pt column at 150 percent")
            }
        }
        print("\(count) proxy assistant presentation assertions passed; detached native views only")
    }
}
