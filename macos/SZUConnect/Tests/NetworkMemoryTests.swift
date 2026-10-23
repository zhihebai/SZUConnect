import Foundation

// Network memory: shared fixtures (match / verdict / known-answer hashes), store behaviour, persistence,
// corruption handling and the descriptor helpers. Pure value logic: no network, no Keychain, no OS network state.

@main
struct NetworkMemoryTests {
    static func main() {
        var passed = 0
        func check(_ condition: @autoclosure () -> Bool, _ name: String) {
            guard condition() else { fatalError("FAIL: \(name)") }
            passed += 1
            print("PASS: \(name)")
        }
        let source = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        func fixture(_ name: String) -> [String: Any] {
            guard let data = try? Data(contentsOf: source.appendingPathComponent("shared/fixtures/" + name)),
                  let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { fatalError("cannot load \(name)") }
            return object
        }
        func descriptor(_ raw: Any?) -> NetworkDescriptor {
            NetworkDescriptor(kind: "wifi", raw: (raw as? [String: Any] ?? [:]).compactMapValues { $0 as? String })
        }
        let doc = fixture("network-memory-cases.json")
        let salt = doc["salt"] as? String ?? ""
        let hasher = NetworkMemory(salt: salt)
        for item in doc["match"] as? [[String: Any]] ?? [] {
            let level = NetworkMemory.compare(stored: hasher.hashed(descriptor(item["stored"])), current: hasher.hashed(descriptor(item["current"])))
            check(level.rawValue == item["expected"] as? String, "match \(item["id"] as? String ?? "?")")
        }
        for item in doc["verdict"] as? [[String: Any]] ?? [] {
            let verdict = memoryVerdict(level: MatchLevel(rawValue: item["level"] as? String ?? "") ?? .none,
                                        teachingConfirmed: item["teaching"] as? String == "confirmed",
                                        dormitoryConfirmed: item["dormitory"] as? String == "confirmed",
                                        ambiguous: item["ambiguous"] as? Bool ?? false)
            check(verdict.rawValue == item["expected"] as? String, "verdict \(item["id"] as? String ?? "?")")
        }
        for item in doc["hash"] as? [[String: Any]] ?? [] {
            let label = item["label"] as? String ?? ""
            let hashed = hasher.hashed(NetworkDescriptor(kind: "wifi", raw: [label: item["raw"] as? String ?? ""]))
            check(hashed[label] == item["expected"] as? String, "known-answer hash \(label) interoperates with C++ and Python")
        }
        for item in doc["refresh"] as? [[String: Any]] ?? [] {
            let date = Date(timeIntervalSince1970: item["now"] as? Double ?? 0)
            func stored(_ records: [[String: Any]]) -> NetworkMemory {
                let hashedRecords = records.map { raw -> [String: Any] in
                    var record = raw
                    record["features"] = hasher.hashed(descriptor(raw["features"]))
                    return record
                }
                let bytes = try! JSONSerialization.data(withJSONObject: ["schema": 1, "salt": salt, "records": hashedRecords])
                return NetworkMemory.decode(bytes, now: date)!
            }
            var before = stored(item["before"] as? [[String: Any]] ?? [])
            let expected = item["expected"] as? [String: Any] ?? [:]
            let result = before.remember(descriptor(item["current"]), area: item["area"] as? String ?? "",
                                         proofTeaching: item["proofTeaching"] as? Bool ?? false,
                                         proofDormitory: item["proofDormitory"] as? Bool ?? false,
                                         now: date, newID: item["newID"] as? String ?? "")
            check(result == expected["selectedID"] as? String
                  && before.records == stored(expected["records"] as? [[String: Any]] ?? []).records,
                  "shared refresh \(item["id"] as? String ?? "?") agrees with C++ and Python")
        }

        // ---- store behaviour
        let now = Date(timeIntervalSince1970: 1_750_000_000)
        let gateway = "a4:5e:60:11:22:33"
        func desc(_ gw: String?, _ gwip: String? = "172.30.0.1", _ net: String? = "172.30.0.0/20") -> NetworkDescriptor {
            var raw: [String: String] = [:]
            if let gw, !gw.isEmpty { raw["gw"] = gw }
            if let gwip, !gwip.isEmpty { raw["gwip"] = gwip }
            if let net, !net.isEmpty { raw["net"] = net }
            return NetworkDescriptor(kind: "ethernet", raw: raw)
        }
        func identifier(_ i: Int) -> String { String(format: "%032x", 0x5000 + i * 31) }
        var store = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
        check(store.remember(NetworkDescriptor(kind: "wifi", raw: ["dns": "1.1.1.1"]), area: "dormitory", proofTeaching: true,
                             proofDormitory: true, now: now, newID: identifier(1)) == nil, "descriptor without gateway evidence is not remembered")
        let first = store.remember(desc(gateway), area: "dormitory", proofTeaching: true, proofDormitory: true, now: now, newID: identifier(1))
        check(first != nil && store.count == 1, "reliable descriptor remembered")
        check(store.match(desc(gateway), now: now).level == .strong, "same network matches strongly")
        let again = store.remember(desc(gateway, "172.30.16.1", "172.30.16.0/20"), area: "dormitory", proofTeaching: true,
                                   proofDormitory: true, now: now.addingTimeInterval(10), newID: identifier(2))
        check(again == first && store.count == 1, "same gateway hardware refreshes the record instead of duplicating")
        check(store.match(desc("b8:27:eb:44:55:66", "172.30.16.1", "172.30.16.0/20"), now: now.addingTimeInterval(10)).level == .conflict,
              "same address, other gateway: conflict")
        check(store.match(desc("b8:27:eb:44:55:66", "192.168.1.1", "192.168.1.0/24"), now: now.addingTimeInterval(10)).level == .none,
              "other gateway elsewhere: just another network")
        let text = String(decoding: store.encoded(), as: UTF8.self)
        for raw in ["a4:5e:60", "172.30", "Wi-Fi", "ssid"] { check(!text.contains(raw), "encoded memory hides \(raw)") }
        let decoded = NetworkMemory.decode(store.encoded(), now: now)
        check(decoded?.count == 1 && decoded?.encoded() == store.encoded(), "encode/decode round trip is byte-stable")
        check(decoded?.match(desc(gateway, "172.30.16.1", "172.30.16.0/20"), now: now).level == .strong, "decoded record still matches")
        check(store.match(desc(gateway, "172.30.16.1", "172.30.16.0/20"), now: now.addingTimeInterval(10), exclude: [first ?? ""]).level == .none,
              "suppressed record is ignored")
        let late = now.addingTimeInterval(NetworkMemory.expiry + 3_600)
        check(store.match(desc(gateway, "172.30.16.1", "172.30.16.0/20"), now: late).level == .none, "expired record is ignored")
        check(NetworkMemory.decode(store.encoded(), now: late)?.count == 0, "expired record pruned on load")
        check(!store.touch(id: first ?? "", now: now.addingTimeInterval(20), area: "dormitory"), "touch within an hour is throttled")
        check(store.touch(id: first ?? "", now: now.addingTimeInterval(7_200), area: "dormitory"), "touch after an hour refreshes")

        var overlapping = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
        let teaching = overlapping.remember(desc(gateway, nil, "172.30.0.0/20"), area: "teaching",
                                            proofTeaching: true, proofDormitory: false, now: now, newID: identifier(3))
        let dormitory = overlapping.remember(desc(nil, "172.30.0.1", "172.30.0.0/20"), area: "dormitory",
                                             proofTeaching: true, proofDormitory: true, now: now, newID: identifier(4))
        let complete = desc(gateway, "172.30.0.1", "172.30.0.0/20")
        check(overlapping.count == 2 && overlapping.matchAny([complete], now: now).record == teaching,
              "A stronger teaching-only record can be the best display match")
        check(overlapping.matchAny([complete], now: now, requireDormitoryProof: true).record == dormitory
              && overlapping.matchAny([complete], now: now, requireDormitoryProof: true).level == .medium,
              "Dormitory authorization selects a weaker eligible record rather than the teaching-only best match")
        let merged = overlapping.remember(complete, area: "teaching", proofTeaching: true, proofDormitory: false,
                                          now: now.addingTimeInterval(60), newID: identifier(5))
        check(merged == teaching && overlapping.count == 1, "Fresh complete evidence folds overlapping partial records into the strongest match")
        check(overlapping.records.first?.proofDormitory == false && overlapping.records.first?.area == "teaching",
              "Combining records retains only the current operation's campus proof")
        check(overlapping.records.first?.created == now.timeIntervalSince1970,
              "Combining records preserves the retained ID and original creation date")
        _ = overlapping.remember(desc(nil, "172.30.0.1", "172.30.0.0/20"), area: "teaching", proofTeaching: true,
                                  proofDormitory: false, now: now.addingTimeInterval(120), newID: identifier(6))
        check(overlapping.records.first?.features["gw"] == hasher.hashed(complete)["gw"],
              "Temporary loss of gateway hardware metadata does not erase known identity evidence")
        check(overlapping.match(desc("b8:27:eb:44:55:66"), now: now.addingTimeInterval(120)).level == .conflict,
              "Preserved gateway metadata still rejects a different router using the same address")

        var strongest = NetworkMemory(salt: salt)
        let weakerID = strongest.remember(desc(nil, "172.30.0.1", "172.30.0.0/20"), area: "teaching",
                                          proofTeaching: true, proofDormitory: false, now: now, newID: identifier(50))
        let strongerID = strongest.remember(desc(gateway, nil, "172.30.0.0/20"), area: "dormitory",
                                            proofTeaching: true, proofDormitory: true, now: now, newID: identifier(51))
        check(weakerID != strongerID && strongest.count == 2, "Disjoint partial observations remain separate until fresh combined evidence")
        let strongestID = strongest.remember(complete, area: "dormitory", proofTeaching: true, proofDormitory: true,
                                             now: now.addingTimeInterval(60), newID: identifier(52))
        check(strongestID == strongerID && strongest.count == 1, "The strongest match wins even when it is not first")

        var conflicting = NetworkMemory(salt: salt)
        let gatewayOne = conflicting.remember(complete, area: "dormitory", proofTeaching: true, proofDormitory: true,
                                              now: now, newID: identifier(53))!
        let gatewayTwo = conflicting.remember(desc("b8:27:eb:44:55:66"), area: "dormitory", proofTeaching: true,
                                              proofDormitory: true, now: now, newID: identifier(54))!
        _ = conflicting.remember(desc(nil), area: "teaching", proofTeaching: true, proofDormitory: false,
                                  now: now.addingTimeInterval(60), newID: identifier(55))
        check(conflicting.count == 2 && Set(conflicting.records.compactMap { $0.features["gw"] }).count == 2,
              "A gateway-free observation cannot join records from different routers")
        check(conflicting.equivalentIDs(id: gatewayOne) == [gatewayOne]
              && conflicting.equivalentIDs(id: gatewayTwo) == [gatewayTwo],
              "Per-record revocation never bridges different gateways through an address or subnet")

        var duplicateJSON = try! JSONSerialization.jsonObject(with: strongest.encoded()) as! [String: Any]
        var duplicateRecords = duplicateJSON["records"] as! [[String: Any]]
        var duplicate = duplicateRecords[0]
        let duplicateID = identifier(56)
        duplicate["id"] = duplicateID
        duplicateRecords.append(duplicate)
        duplicateJSON["records"] = duplicateRecords
        let oldDuplicates = NetworkMemory.decode(try! JSONSerialization.data(withJSONObject: duplicateJSON), now: now)!
        check(Set(oldDuplicates.equivalentIDs(id: strongestID!)) == [strongestID!, duplicateID],
              "Revocation finds all historical copies with the same reliable identity")
        check(oldDuplicates.equivalentIDs(id: identifier(57)).isEmpty, "An unknown record has no revocation group")
        var partialFeatures = duplicateRecords[0]["features"] as! [String: String]
        partialFeatures.removeValue(forKey: "gw")
        duplicateRecords[0]["features"] = partialFeatures
        duplicateJSON["records"] = duplicateRecords
        var equalRanks = NetworkMemory.decode(try! JSONSerialization.data(withJSONObject: duplicateJSON), now: now)!
        let keptGateway = equalRanks.remember(desc(nil), area: "teaching", proofTeaching: true, proofDormitory: false,
                                               now: now.addingTimeInterval(120), newID: identifier(58))
        check(keptGateway == duplicateID && equalRanks.count == 1
              && equalRanks.records.first?.features["gw"] == hasher.hashed(complete)["gw"],
              "Equal-rank duplicates retain the record with known gateway hardware evidence")

        var many = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
        for i in 0..<10 {
            _ = many.remember(desc(String(format: "aa:00:00:00:00:%02x", i), "10.\(i).0.1", nil), area: "", proofTeaching: true,
                              proofDormitory: true, now: now.addingTimeInterval(Double(i)), newID: identifier(10 + i))
        }
        check(many.count == 8, "at most eight networks are remembered")
        check(many.match(desc("aa:00:00:00:00:00", "10.0.0.1", nil), now: now.addingTimeInterval(10)).level == .none, "oldest record was dropped")
        check(many.forget(id: identifier(19)) && many.count == 7 && !many.forget(id: identifier(19)), "forget removes exactly one record")
        many.forgetAll()
        check(many.count == 0, "forgetAll clears everything")

        let okSalt = "00112233445566778899aabbccddeeff"
        check(NetworkMemory.decode(Data("{\"schema\":1,\"salt\":\"\(okSalt)\",\"records\":[]}".utf8), now: now) != nil, "empty valid file loads")
        let record = "{\"id\":\"\(identifier(1))\",\"created\":1,\"verified\":1,\"proofTeaching\":true,\"proofDormitory\":true"
        for bad in ["not json",
                    "{\"schema\":2,\"salt\":\"\(okSalt)\",\"records\":[]}",
                    "{\"schema\":1,\"salt\":\"abc\",\"records\":[]}",
                    "{\"schema\":1,\"records\":[]}",
                    "{\"schema\":1,\"salt\":\"\(okSalt)\",\"records\":[{\"id\":\"zz\"}]}",
                    "{\"schema\":1,\"salt\":\"\(okSalt)\",\"records\":[\(record),\"features\":{\"gw\":\"short\"}}]}",
                    "{\"schema\":1,\"salt\":\"\(okSalt)\",\"records\":[\(record),\"area\":\"moon\",\"features\":{}}]}"] {
            check(NetworkMemory.decode(Data(bad.utf8), now: now) == nil, "corrupt memory refused: \(bad.prefix(40))")
        }

        // ---- descriptor helpers
        check(NetworkContinuitySnapshot.normalizedHardwareAddress("A4:5E:60:1:2:33") == "a4:5e:60:01:02:33", "hardware address is zero-padded and lower-cased")
        check(NetworkContinuitySnapshot.normalizedHardwareAddress("00:00:00:00:00:00") == nil, "all-zero hardware address is unknown")
        check(NetworkContinuitySnapshot.normalizedHardwareAddress("a4:5e:60:11:22") == nil, "short hardware address is refused")
        check(NetworkContinuitySnapshot.networkText(address: "172.30.5.9", mask: "255.255.240.0") == "172.30.0.0/20", "subnet is derived from address and mask")
        check(NetworkContinuitySnapshot.networkText(address: "169.254.3.4", mask: "255.255.0.0") == nil, "link-local addresses give no subnet")
        check(NetworkContinuitySnapshot.networkText(address: "10.0.0.5", mask: "255.0.255.0") == nil, "non-contiguous mask is refused")
        let item: [String: Any] = ["Router": "172.30.0.1", "Addresses": ["172.30.5.9"], "SubnetMasks": ["255.255.240.0"],
                                   "ARPResolvedHardwareAddress": gateway]
        let built = NetworkContinuitySnapshot.makeDescriptor(kind: "wifi", ipv4: item, dhcpServer: "172.30.0.2",
                                                             dnsServers: ["8.8.8.8", "1.1.1.1"], searchDomain: "SZU.edu.cn")
        check(built.raw["gw"] == gateway && built.raw["gwip"] == "172.30.0.1" && built.raw["net"] == "172.30.0.0/20"
              && built.raw["dhcp"] == "172.30.0.2" && built.raw["dns"] == "1.1.1.1,8.8.8.8" && built.raw["sfx"] == "szu.edu.cn",
              "descriptor carries gateway, subnet, DHCP, DNS and suffix")
        check(!built.raw.values.contains { $0.contains("172.30.5.9") }, "the client address is not part of the descriptor")
        let signature: [String: Any] = ["Router": "192.168.1.1", "Addresses": ["192.168.1.20"], "SubnetMasks": ["255.255.255.0"],
                                        "NetworkSignature": "IPv4.Router=192.168.1.1;IPv4.RouterHardwareAddress=aa:bb:cc:dd:ee:ff"]
        check(NetworkContinuitySnapshot.makeDescriptor(kind: "ethernet", ipv4: signature).raw["gw"] == "aa:bb:cc:dd:ee:ff", "NetworkSignature supplies the gateway hardware address")
        check(!NetworkContinuitySnapshot.makeDescriptor(kind: "ethernet", ipv4: ["Router": "10.0.0.1"]).isReliable, "a router address alone is not reliable")
        print("network memory tests: \(passed) checks passed")
    }
}
