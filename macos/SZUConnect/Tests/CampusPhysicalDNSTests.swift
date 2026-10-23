import Foundation

/// Virtual clock and packets only. No interface lookup, DNS socket or real host request.
@main
struct CampusPhysicalDNSTests {
    typealias DNS = CampusPhysicalDNS
    static var checks = 0
    static func check(_ condition: @autoclosure () -> Bool, _ label: String) {
        checks += 1
        if !condition() { fatalError("FAIL: \(label)") }
    }
    static func name(_ host: String) -> [UInt8] {
        host.split(separator: ".").flatMap { [UInt8($0.utf8.count)] + Array($0.utf8) } + [0]
    }
    static func reply(code: UInt8 = 0, truncated: Bool = false, transaction: UInt16 = 0x1234,
                      host: String = "net.szu.edu.cn", type: UInt8 = 1, klass: UInt8 = 1,
                      address: [UInt8]? = [203, 0, 113, 20]) -> Data {
        let answer = address.map { [UInt8](arrayLiteral: 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4) + $0 } ?? []
        return Data([UInt8(transaction >> 8), UInt8(transaction & 255), truncated ? 0x83 : 0x81,
                     0x80 | code, 0, 1, 0, address == nil ? 0 : 1, 0, 0, 0, 0]
            + name(host) + [0, type, 0, klass] + answer)
    }
    final class Fixture {
        struct Sent { let server: Int; let at: TimeInterval; let data: Data }
        var time: TimeInterval = 0
        var sent: [Sent] = []
        var queued: [(TimeInterval, DNS.ExchangeEvent)] = []
        var sends: [Int: Int] = [:]
        var receiveCalls = 0
        var onSend: ((Fixture, Int, Int) -> Bool)?
        var isCancelled: ((Fixture) -> Bool)?
        var failReceive = false

        func add(_ at: TimeInterval, server: Int = 0, data: Data = CampusPhysicalDNSTests.reply()) {
            queued.append((at, .packet(server: server, data: data)))
        }
        func run(servers: Int = 1, deadline: TimeInterval = 3, host: String = "net.szu.edu.cn") throws -> [String] {
            try DNS.exchange(host: host, transaction: 0x1234, servers: servers, deadline: deadline,
                now: { self.time }, send: { server, data in
                    self.sent.append(Sent(server: server, at: self.time, data: data))
                    self.sends[server, default: 0] += 1
                    return self.onSend?(self, server, self.sends[server]!) ?? true
                }, receive: { active, wait in
                    self.receiveCalls += 1
                    precondition(self.receiveCalls < 10_000, "virtual exchange did not advance")
                    precondition(wait > 0 && wait <= 0.05 + 0.000_001, "poll wait is bounded")
                    if self.failReceive { throw CampusPhysicalDNSError.unavailable }
                    let next = self.queued.map(\.0).filter { $0 >= self.time }.min()
                    self.time = min(self.time + wait, next ?? .infinity)
                    let ready = self.queued.filter { $0.0 <= self.time }
                    self.queued.removeAll { $0.0 <= self.time }
                    return ready.map(\.1)
                }, cancelled: { self.isCancelled?(self) ?? false })
        }
    }
    static func fails(_ expected: CampusPhysicalDNSError, _ fixture: Fixture, servers: Int = 1,
                      deadline: TimeInterval = 3, host: String = "net.szu.edu.cn", _ label: String) {
        do { _ = try fixture.run(servers: servers, deadline: deadline, host: host); check(false, label) }
        catch { check(error as? CampusPhysicalDNSError == expected, label) }
    }
    static func near(_ a: TimeInterval, _ b: TimeInterval) -> Bool { abs(a - b) < 0.000_001 }

    static func main() throws {
        let lostFirst = Fixture()
        lostFirst.onSend = { fixture, server, attempt in
            if attempt == 2 { fixture.add(fixture.time + 0.02, server: server) }
            return true
        }
        check((try? lostFirst.run()) == ["203.0.113.20"], "one lost first UDP packet recovers within the original request")
        check(lostFirst.sent.count == 2 && near(lostFirst.sent[1].at, 0.25), "first retransmission occurs at 250 ms")
        check(near(lostFirst.time, 0.27), "packet loss does not consume the complete timeout")
        check(Set(lostFirst.sent.map(\.data)).count == 1, "retransmission preserves transaction and exact A/IN question")

        let lostTwo = Fixture()
        lostTwo.onSend = { fixture, server, attempt in
            if attempt == 3 { fixture.add(fixture.time + 0.02, server: server) }
            return true
        }
        check((try? lostTwo.run()) == ["203.0.113.20"], "two lost packets recover on the final bounded attempt")
        check(lostTwo.sent.count == 3 && near(lostTwo.sent[2].at, 0.75) && near(lostTwo.time, 0.77), "last retry at 750 ms")

        for (budget, sends) in [(0.1, 1), (0.25, 1), (0.5, 2), (0.75, 2), (1.0, 3), (6.0, 3), (12.0, 3)] {
            let none = Fixture()
            fails(.timeout, none, deadline: budget, "lost replies exhaust one shared budget \(budget)")
            check(none.sent.count == sends && near(none.time, budget), "retry count and deadline bounded \(budget)")
        }
        let setup = Fixture(); setup.time = 0.2
        fails(.timeout, setup, deadline: 0.3, "socket setup time is already deducted from deadline")
        check(setup.sent.count == 1 && near(setup.time, 0.3), "setup never restarts the timeout or squeezes extra retries")

        for code: UInt8 in [1, 2, 3, 4, 5] {
            let negative = Fixture(); negative.add(0.01, data: reply(code: code, address: nil))
            fails(.invalidResponse, negative, "matched DNS error \(code) finishes promptly")
            check(negative.sent.count == 1 && near(negative.time, 0.01), "matched negative answer is not retried \(code)")
        }
        for data in [reply(truncated: true, address: nil), reply(truncated: true), reply(address: nil)] {
            let terminal = Fixture(); terminal.add(0.01, data: data)
            fails(.invalidResponse, terminal, "matched truncation or empty answer fails closed without waiting")
            check(terminal.sent.count == 1 && near(terminal.time, 0.01), "no TCP/system/public DNS fallback")
        }
        let mixed = Fixture()
        mixed.add(0.01, server: 0, data: reply(code: 2, address: nil)); mixed.add(0.02, server: 1)
        check((try? mixed.run(servers: 2)) == ["203.0.113.20"], "one resolver failure leaves another resolver eligible to succeed")
        check(mixed.sent.count == 2 && near(mixed.time, 0.02), "mixed outcome does not wait for retries")
        let both = Fixture()
        both.add(0.01, server: 0, data: reply(code: 3, address: nil))
        both.add(0.02, server: 1, data: reply(code: 5, address: nil))
        fails(.invalidResponse, both, servers: 2, "all matched terminal answers finish the round")
        check(both.sent.count == 2 && near(both.time, 0.02), "all terminal answers finish before timeout")
        let sameBatch = Fixture()
        sameBatch.add(0.01, server: 0, data: reply(code: 3, address: nil)); sameBatch.add(0.01, server: 1)
        check((try? sameBatch.run(servers: 2)) == ["203.0.113.20"], "success in same receive batch survives another server's terminal failure")

        let onlyPending = Fixture()
        onlyPending.add(0.01, server: 0, data: reply(code: 2, address: nil))
        onlyPending.onSend = { fixture, server, attempt in
            if server == 1 && attempt == 2 { fixture.add(fixture.time + 0.01, server: server) }
            return true
        }
        check((try? onlyPending.run(servers: 2)) == ["203.0.113.20"], "remaining resolver retries after its peer terminates")
        check(onlyPending.sends[0] == 1 && onlyPending.sends[1] == 2, "terminated resolver receives no retransmission")
        let partlySilent = Fixture(); partlySilent.add(0.01, server: 0, data: reply(code: 3, address: nil))
        fails(.invalidResponse, partlySilent, servers: 2, deadline: 1, "one terminal error cannot prematurely end a silent alternate")
        check(near(partlySilent.time, 1) && partlySilent.sends[0] == 1 && partlySilent.sends[1] == 3,
              "silent alternate retains its complete bounded opportunity")

        var reserved = reply(code: 3, address: nil); reserved[3] |= 0x40
        var query = reply(code: 3, address: nil); query[2] &= 0x7f
        var opcode = reply(code: 3, address: nil); opcode[2] |= 0x08
        let foreign = [reply(code: 3, transaction: 0x4321, address: nil),
            reply(code: 3, host: "www.apple.com", address: nil), reply(code: 3, type: 28, address: nil),
            reply(code: 3, klass: 3, address: nil), reserved, query, opcode,
            Data(reply(truncated: true).prefix(15)), reply(address: [198, 18, 0, 1])]
        for (index, data) in foreign.enumerated() {
            let noise = Fixture(); noise.add(0.01, data: data)
            noise.onSend = { fixture, server, attempt in
                if attempt == 2 { fixture.add(fixture.time + 0.01, server: server) }
                return true
            }
            check((try? noise.run()) == ["203.0.113.20"], "unmatched or malformed packet does not terminate resolver \(index)")
            check(noise.sent.count == 2 && near(noise.time, 0.26), "noise neither accepts identity nor delays retransmission \(index)")
        }
        let inactive = Fixture(); inactive.add(0.01, server: 7)
        inactive.onSend = { fixture, server, attempt in
            if attempt == 2 { fixture.add(fixture.time + 0.01, server: server) }
            return true
        }
        check((try? inactive.run()) == ["203.0.113.20"], "packets from outside active resolver set cannot authorize a host")
        let closedPeer = Fixture()
        closedPeer.add(0.01, server: 0, data: reply(code: 3, address: nil)); closedPeer.add(0.02, server: 0)
        closedPeer.add(0.03, server: 1, data: reply(address: [192, 0, 2, 8]))
        check((try? closedPeer.run(servers: 2)) == ["192.0.2.8"], "late packet from a terminated resolver cannot supply identity")

        let cancelled = Fixture(); cancelled.isCancelled = { _ in true }
        fails(.cancelled, cancelled, "early cancellation precedes all sends")
        check(cancelled.sent.isEmpty && cancelled.receiveCalls == 0, "early cancellation performs no I/O")
        let onRetry = Fixture(); onRetry.isCancelled = { $0.time >= 0.25 }
        fails(.cancelled, onRetry, "cancellation at retry boundary wins")
        check(onRetry.sent.count == 1 && near(onRetry.time, 0.25), "cancelled query is never retransmitted")
        let betweenSends = Fixture(); betweenSends.isCancelled = { !$0.sent.isEmpty }
        fails(.cancelled, betweenSends, servers: 2, "cancellation is checked before each resolver send")
        check(betweenSends.sent.count == 1, "cancellation stops remaining initial sends")
        let onResult = Fixture(); onResult.add(0.01); onResult.isCancelled = { $0.time >= 0.01 }
        fails(.cancelled, onResult, "cancellation after receive wins over a ready valid answer")
        let late = Fixture(); late.add(0.5)
        fails(.timeout, late, deadline: 0.5, "answer at expired deadline is not accepted")

        let slowSend = Fixture(); slowSend.onSend = { fixture, _, _ in fixture.time += 0.2; return true }
        fails(.timeout, slowSend, servers: 2, deadline: 0.1, "deadline is checked between resolver sends")
        check(slowSend.sent.count == 1, "socket work cannot extend budget for another initial send")
        let sendsFail = Fixture(); sendsFail.onSend = { _, _, _ in false }
        fails(.unavailable, sendsFail, servers: 2, "all send failures end promptly")
        check(near(sendsFail.time, 0) && sendsFail.receiveCalls == 0, "unavailable socket never polls or falls back")
        let retryFail = Fixture(); retryFail.onSend = { _, _, attempt in attempt == 1 }
        fails(.unavailable, retryFail, "retry send failure retires the last resolver")
        check(retryFail.sent.count == 2 && near(retryFail.time, 0.25), "send failure never resets timeout")
        let ioFail = Fixture(); ioFail.failReceive = true
        fails(.unavailable, ioFail, "poll failure is retained")
        for count in [0, 9] {
            let invalid = Fixture(); fails(.unavailable, invalid, servers: count, "resolver count bounded \(count)")
            check(invalid.sent.isEmpty, "invalid resolver count sends nothing \(count)")
        }
        let unknown = Fixture(); fails(.unavailable, unknown, host: "example.org", "exchange host allowlist retained")
        check(unknown.sent.isEmpty, "disallowed host never reaches injected transport")
        let invalidDeadline = Fixture(); fails(.unavailable, invalidDeadline, deadline: .infinity, "deadline must be finite")
        print("CampusPhysicalDNS: \(checks) assertions passed (virtual packets; no DNS or network I/O).")
    }
}
