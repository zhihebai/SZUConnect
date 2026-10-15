import SwiftUI

// Uncached reference parser used as an independent geometry oracle.
struct LegacyIconShape: Shape {
    let name: String
    func path(in rect: CGRect) -> Path {
        var path = Path()
        guard let data = SharedIcons.paths[name] else { return path }
        let scale = min(rect.width, rect.height) / 24
        func point(_ x: Double, _ y: Double) -> CGPoint { CGPoint(x: rect.minX + x * scale, y: rect.minY + y * scale) }
        let tokens = data.replacingOccurrences(of: ",", with: " ").split(separator: " ")
        var index = 0
        var command: Character = "M"
        func next() -> Double {
            defer { index += 1 }
            return index < tokens.count ? Double(tokens[index]) ?? 0 : 0
        }
        while index < tokens.count {
            let token = tokens[index]
            if let first = token.first, first.isLetter {
                command = first
                if token.count > 1 {
                    // "M12" style: the number is glued to the command letter.
                    let number = Double(token.dropFirst()) ?? 0
                    index += 1
                    switch command {
                    case "M": path.move(to: point(number, next()))
                    case "L": path.addLine(to: point(number, next()))
                    case "C":
                        let c1 = point(number, next()), c2 = point(next(), next()), end = point(next(), next())
                        path.addCurve(to: end, control1: c1, control2: c2)
                    default: break
                    }
                    continue
                }
                index += 1
                if command == "Z" { path.closeSubpath() }
                continue
            }
            switch command {
            case "M": path.move(to: point(next(), next()))
            case "L": path.addLine(to: point(next(), next()))
            case "C":
                let c1 = point(next(), next()), c2 = point(next(), next()), end = point(next(), next())
                path.addCurve(to: end, control1: c1, control2: c2)
            default: index += 1
            }
        }
        return path
    }
}


private struct GeometryElement {
    let kind: String
    let points: [CGPoint]
}

private func elements(_ path: Path) -> [GeometryElement] {
    var result: [GeometryElement] = []
    path.forEach { element in
        switch element {
        case .move(let point): result.append(.init(kind: "move", points: [point]))
        case .line(let point): result.append(.init(kind: "line", points: [point]))
        case .quadCurve(let end, let control): result.append(.init(kind: "quad", points: [end, control]))
        case .curve(let end, let c1, let c2): result.append(.init(kind: "curve", points: [end, c1, c2]))
        case .closeSubpath: result.append(.init(kind: "close", points: []))
        }
    }
    return result
}

// Read every resulting point in both implementations so Release cannot discard path work.
@inline(never) private func consume(_ path: Path) -> Double {
    var total = 0.0
    path.forEach { element in
        switch element {
        case .move(let p): total += p.x * 0.31 + p.y * 0.73 + 1
        case .line(let p): total += p.x * 0.37 + p.y * 0.79 + 2
        case .quadCurve(let p, let c): total += p.x + p.y + c.x + c.y + 3
        case .curve(let p, let c1, let c2): total += p.x + p.y + c1.x + c1.y + c2.x + c2.y + 4
        case .closeSubpath: total += 5
        }
    }
    return total
}

@inline(never) private func legacy(_ name: String, _ rect: CGRect) -> Path {
    LegacyIconShape(name: name).path(in: rect)
}
@inline(never) private func cached(_ name: String, _ rect: CGRect) -> Path {
    IconShape(name: name).path(in: rect)
}

@main struct IconPathVerification {
    static func main() {
        let names = SharedIcons.paths.keys.sorted()
        let rects: [CGRect] = [
            CGRect(x: 0, y: 0, width: 24, height: 24),
            CGRect(x: 3.25, y: -7.75, width: 12, height: 12),
            CGRect(x: -14, y: 8.5, width: 18, height: 32),
            CGRect(x: 19.5, y: 41, width: 64, height: 14),
            CGRect(x: 0.125, y: 0.375, width: 1, height: 1.5),
            CGRect(x: -1024, y: 512, width: 240, height: 480),
            CGRect(x: 81.375, y: -64.125, width: 37.5, height: 37.5),
            CGRect(x: 7, y: 9, width: 13.75, height: 16.125),
            CGRect(x: 9.5, y: 12.75, width: 0, height: 24),
            CGRect(x: -4.75, y: 2.125, width: 24, height: 0),
            CGRect(x: 3, y: 7, width: 0, height: 0)
        ]
        var comparisons = 0
        var points = 0
        var maxDifference = 0.0
        for name in names {
            for rect in rects {
                let a = elements(legacy(name, rect))
                let b = elements(cached(name, rect))
                precondition(a.count == b.count, "Element count differs: \(name), \(rect)")
                for (before, after) in zip(a, b) {
                    precondition(before.kind == after.kind && before.points.count == after.points.count,
                                 "Element kind differs: \(name), \(rect)")
                    for (p, q) in zip(before.points, after.points) {
                        let difference = max(abs(p.x-q.x), abs(p.y-q.y))
                        maxDifference = max(maxDifference, difference)
                        let tolerance = 1e-10 * max(1, abs(p.x), abs(p.y))
                        precondition(difference <= tolerance, "Coordinates differ: \(name), \(rect), \(p), \(q)")
                        points += 1
                    }
                }
                comparisons += 1
            }
        }
        for rect in rects {
            precondition(elements(cached("not-a-shared-icon", rect)).isEmpty, "Unknown icon must stay empty")
        }
        print("GEOMETRY PASS: \(names.count) keys, \(Set(SharedIcons.paths.values).count) unique paths, \(rects.count) rects, \(comparisons) path comparisons, \(points) points, maxDifference=\(maxDifference)")
        guard CommandLine.arguments.contains("--benchmark") else { return }

        // Both consume the exact same nonzero-size input sequence. Warm the static cache first.
        let inputs = names.flatMap { name in rects.prefix(8).map { (name, $0) } }
        let cycles = 80
        let warmupCycles = 5
        let rounds = 9
        @inline(never) func run(_ function: (String, CGRect) -> Path, cycles: Int) -> (Double, Double) {
            let start = ContinuousClock.now
            var checksum = 0.0
            for _ in 0..<cycles {
                for (name, rect) in inputs { checksum += consume(function(name, rect)) }
            }
            let duration = start.duration(to: .now)
            let seconds = Double(duration.components.seconds) + Double(duration.components.attoseconds) / 1e18
            return (seconds, checksum)
        }
        let aWarm = run(legacy, cycles: warmupCycles)
        let bWarm = run(cached, cycles: warmupCycles)
        precondition(abs(aWarm.1-bWarm.1) <= abs(aWarm.1)*1e-12, "Warmup checksum differs")
        var before: [Double] = [], after: [Double] = []
        var lastChecksum = 0.0
        for round in 0..<rounds {
            // Alternate order to avoid giving one implementation all later/cooler samples.
            let a: (Double, Double), b: (Double, Double)
            if round.isMultiple(of: 2) { a = run(legacy, cycles: cycles); b = run(cached, cycles: cycles) }
            else { b = run(cached, cycles: cycles); a = run(legacy, cycles: cycles) }
            precondition(abs(a.1-b.1) <= abs(a.1)*1e-12, "Benchmark checksum differs")
            before.append(a.0); after.append(b.0); lastChecksum = a.1
            print(String(format: "ROUND %d legacy_ms=%.3f cached_ms=%.3f ratio=%.3f checksum=%.9f", round+1, a.0*1000, b.0*1000, a.0/b.0, a.1))
        }
        let medianBefore = before.sorted()[rounds/2]
        let medianAfter = after.sorted()[rounds/2]
        print("BENCHMARK: Release -O -whole-module-optimization, \(inputs.count) inputs/cycle, \(cycles) cycles/round, \(rounds) alternating rounds, \(warmupCycles) warmup cycles/implementation, checksum=\(lastChecksum)")
        print(String(format: "MEDIAN legacy_ms=%.3f cached_ms=%.3f ratio=%.3f legacy_range_ms=[%.3f,%.3f] cached_range_ms=[%.3f,%.3f]", medianBefore*1000, medianAfter*1000, medianBefore/medianAfter, before.min()!*1000, before.max()!*1000, after.min()!*1000, after.max()!*1000))
        print("This is an icon-path construction microbenchmark; it does not measure whole-app CPU, RSS, energy, or user latency.")
    }
}
