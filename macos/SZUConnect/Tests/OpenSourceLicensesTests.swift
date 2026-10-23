import Foundation

@main
struct OpenSourceLicensesTests {
    static func main() throws {
        // The runner constructs this dedicated disposable bundle. Never mutate
        // any real application or use its resources as a writable fixture.
        guard Bundle.main.bundleIdentifier == "local.SZUConnect.tests.licenses",
              let resources = Bundle.main.resourceURL else {
            fatalError("Run with run-open-source-licenses.sh in its isolated bundle")
        }
        var checks = 0
        func expect(_ value: @autoclosure () -> Bool, _ message: String) {
            checks += 1
            guard value() else { fatalError("FAIL: " + message) }
        }
        func snapshot(_ documents: [OpenSourceLicense]) -> [String?] { documents.map(\.text) }

        let initial = OpenSourceLicenses.documents()
        expect(initial.map(\.id) == ["project", "autoszuweb", "curl"], "The Mac component order is unchanged")
        expect(initial.allSatisfy { $0.text?.hasPrefix("fixture ") == true }, "All bundled files are complete and readable")
        let initialText = snapshot(initial)
        for name in ["LICENSE", "AutoSZUWeb-LICENSE.txt", "curl-COPYING.txt"] {
            try FileManager.default.removeItem(at: resources.appendingPathComponent(name))
        }
        for _ in 0..<100 {
            _ = OpenSourceLicensesView()
            expect(snapshot(OpenSourceLicenses.documents()) == initialText,
                   "Repeated view creation retains licenses without reading missing bundle files")
        }
        expect(snapshot(OpenSourceLicenses.documents(in: .main)) == initialText,
               "An explicit main bundle shares the same immutable cache")

        let fixtureRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("szu-license-injected-" + UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: fixtureRoot) }
        let fixtureResources = fixtureRoot.appendingPathComponent("Injected.bundle/Contents/Resources", isDirectory: true)
        try FileManager.default.createDirectory(at: fixtureResources, withIntermediateDirectories: true)
        let plist: [String: String] = ["CFBundleIdentifier": "local.SZUConnect.tests.injected-license", "CFBundlePackageType": "BNDL"]
        try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0)
            .write(to: fixtureResources.deletingLastPathComponent().appendingPathComponent("Info.plist"))
        let licenseFile = fixtureResources.appendingPathComponent("LICENSE")
        try "injected first\n".write(to: licenseFile, atomically: true, encoding: .utf8)
        let fixtureBundle = Bundle(url: fixtureResources.deletingLastPathComponent().deletingLastPathComponent())!
        expect(OpenSourceLicenses.documents(in: fixtureBundle).first?.text == "injected first\n",
               "An injected bundle reads its own license")
        try "injected second\n".write(to: licenseFile, atomically: true, encoding: .utf8)
        expect(OpenSourceLicenses.documents(in: fixtureBundle).first?.text == "injected second\n",
               "An injected bundle is not captured by the main-bundle cache")
        try " \n\t".write(to: licenseFile, atomically: true, encoding: .utf8)
        expect(OpenSourceLicenses.documents(in: fixtureBundle).first?.text == nil,
               "A blank injected license remains unavailable")
        expect(OpenSourceLicenses.documents(in: fixtureBundle).dropFirst().allSatisfy { $0.text == nil },
               "Missing injected component files remain unavailable")
        expect(snapshot(OpenSourceLicenses.documents()) == initialText,
               "Injected reads never replace the main-bundle contents")
        print("PASS: \(checks) bundled-license cache checks")
    }
}
