import SwiftUI
import Combine

private struct ThermalFrame: Decodable {
    let width: Int
    let height: Int
    let min_c: Double
    let max_c: Double
    let mean_c: Double
    let pixels_c: [Double]
}
private struct ThermalFrameResponse: Decodable {
    let success: Bool
    let stale: Bool
    let frame: ThermalFrame?
}

@MainActor
private final class ThermalCameraModel: ObservableObject {
    @Published var frame: ThermalFrame?
    @Published var status = "Live view is off."
    private var etag: String?

    func pause() { frame = nil; etag = nil; status = "Live view is off." }

    func run(nodeId: String) async {
        while !Task.isCancelled {
            do {
                let customerToken = CustomerAccessTokenStore.load()
                let prefix = customerToken == nil ? "thermal" : "customer/thermal"
                guard let encoded = nodeId.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed),
                      let url = URL(string: "https://good-shepherd-server-j06f.onrender.com/\(prefix)/\(encoded)/frame") else { return }
                var request = URLRequest(url: url)
                if let etag { request.setValue(etag, forHTTPHeaderField: "If-None-Match") }
                request.timeoutInterval = 10
                request.cachePolicy = .reloadIgnoringLocalCacheData
                if let customerToken {
                    request.setValue("Bearer \(customerToken)", forHTTPHeaderField: "Authorization")
                } else { StaffAPIAuth.apply(to: &request) }
                let (data, response) = try await URLSession.shared.data(for: request)
                try Task.checkCancellation()
                if (response as? HTTPURLResponse)?.statusCode == 304 {
                    try await Task.sleep(nanoseconds: 1_000_000_000)
                    continue
                }
                guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
                    frame = nil; status = "Thermal image unavailable. Check your connection and sensor assignment."
                    try await Task.sleep(nanoseconds: 2_000_000_000)
                    continue
                }
                let result = try JSONDecoder().decode(ThermalFrameResponse.self, from: data)
                if result.success, !result.stale, let image = result.frame,
                   image.width == 32, image.height == 24, image.pixels_c.count == 768,
                   image.pixels_c.allSatisfy({ $0.isFinite && (-40...300).contains($0) }) {
                    etag = http.value(forHTTPHeaderField: "ETag")
                    frame = image; status = "Live thermal image"
                } else {
                    frame = nil; status = "Waiting for a fresh image. Keep your thermal sensor connected."
                }
            } catch is CancellationError { return }
            catch { frame = nil; status = "Reconnecting to your thermal camera…" }
            do { try await Task.sleep(nanoseconds: 1_000_000_000) }
            catch { return }
        }
    }
}

struct ThermalCameraCard: View {
    let nodeId: String
    let title: String
    @Environment(\.scenePhase) private var scenePhase
    @StateObject private var model = ThermalCameraModel()
    @State private var wantsLive = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Label(title, systemImage: "thermometer.medium").font(.headline)
                Spacer()
                Button(wantsLive ? "Stop live" : "View live") {
                    wantsLive.toggle()
                    if !wantsLive { model.pause() }
                }.buttonStyle(.bordered)
            }
            ThermalLearningStatusView(nodeId: nodeId)
            Divider()
            Text(model.status).font(.caption).foregroundStyle(.secondary)
            if let frame = model.frame {
                Canvas { context, size in
                    let midpoint = (frame.min_c + frame.max_c) / 2
                    let span = max(8, frame.max_c - frame.min_c)
                    let low = midpoint - span / 2
                    for row in 0..<24 {
                        for column in 0..<32 {
                            let value = min(1, max(0, (frame.pixels_c[row * 32 + column] - low) / span))
                            let color = Self.thermalColor(value)
                            let rect = CGRect(x: Double(column) * size.width / 32, y: Double(row) * size.height / 24,
                                              width: size.width / 32 + 0.25, height: size.height / 24 + 0.25)
                            context.fill(Path(rect), with: .color(color))
                        }
                    }
                }
                .aspectRatio(4 / 3, contentMode: .fit)
                .clipShape(RoundedRectangle(cornerRadius: 12))
                .accessibilityLabel("Thermal image. Coolest \(frame.min_c, specifier: "%.1f") degrees Celsius, warmest \(frame.max_c, specifier: "%.1f") degrees Celsius.")
                HStack {
                    Text("Cool \(frame.min_c, specifier: "%.1f") °C")
                    Spacer()
                    Text("Warm \(frame.max_c, specifier: "%.1f") °C")
                }.font(.caption.monospacedDigit())
                Text("Colors adjust to the current temperature range.").font(.caption2).foregroundStyle(.secondary)
            } else {
                RoundedRectangle(cornerRadius: 12).fill(Color(.secondarySystemBackground))
                    .aspectRatio(4 / 3, contentMode: .fit)
                    .overlay(Image(systemName: "thermometer.medium").font(.largeTitle).foregroundStyle(.secondary))
            }
        }
        .padding()
        .background(Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 18))
        .task(id: scenePhase == .active && wantsLive ? nodeId : "") {
            guard scenePhase == .active && wantsLive else { model.pause(); return }
            await model.run(nodeId: nodeId)
        }
        .onChange(of: scenePhase) { _, phase in
            if phase != .active { wantsLive = false; model.pause() }
        }
        .onDisappear { wantsLive = false; model.pause() }
    }

    private static func thermalColor(_ value: Double) -> Color {
        let stops: [(Double, Double, Double)] = [(8,11,37),(84,33,129),(211,47,84),(245,128,40),(251,233,116),(255,255,255)]
        let scaled = value * 5, index = min(4, Int(scaled)), fraction = scaled - Double(index)
        let a = stops[index], b = stops[index + 1]
        return Color(red: (a.0 + (b.0-a.0)*fraction)/255, green: (a.1 + (b.1-a.1)*fraction)/255, blue: (a.2 + (b.2-a.2)*fraction)/255)
    }
}

// Learning status uses the lightweight endpoint and never requests a live image.
private struct ThermalLearningResponse: Decodable {
    let success: Bool
    let status: ThermalLearningStatus
}
private struct ThermalLearningStatus: Decodable {
    let state: String
    let stale: Bool
    let receivedAt: String?
    let learning: ThermalLearningDetails?
    var lastUpdate: Date? {
        guard let receivedAt else { return nil }
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        if let date = formatter.date(from: receivedAt) { return date }
        formatter.formatOptions = [.withInternetDateTime]
        return formatter.date(from: receivedAt)
    }
    var title: String {
        guard !stale else { return "Waiting for sensor" }
        switch state {
        case "ready": return "Ready"
        case "learning": return "Learning the room"
        case "waiting_for_background": return "Waiting for stable background"
        case "reviewing_change": return "Reviewing a room change"
        case "waiting_for_sensor": return "Waiting for sensor"
        default: return "Learning status update pending"
        }
    }
}
private struct ThermalLearningDetails: Decodable {
    let readErrors: UInt32
    let samplingGaps: UInt32
}
@MainActor
private final class ThermalLearningModel: ObservableObject {
    @Published var status: ThermalLearningStatus?
    @Published var message = "Checking room learning…"
    func pause() { status = nil; message = "Checking room learning…" }
    func run(nodeId: String) async {
        while !Task.isCancelled {
            do {
                let token = CustomerAccessTokenStore.load()
                let prefix = token == nil ? "thermal" : "customer/thermal"
                guard let encoded = nodeId.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed),
                      let url = URL(string: "https://good-shepherd-server-j06f.onrender.com/\(prefix)/\(encoded)/status") else { return }
                var request = URLRequest(url: url)
                request.timeoutInterval = 10
                request.cachePolicy = .reloadIgnoringLocalCacheData
                if let token { request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization") }
                else { StaffAPIAuth.apply(to: &request) }
                let (data, response) = try await URLSession.shared.data(for: request)
                try Task.checkCancellation()
                guard (response as? HTTPURLResponse)?.statusCode == 200 else {
                    status = nil; message = "Learning status unavailable"
                    try await Task.sleep(nanoseconds: 15_000_000_000)
                    continue
                }
                let result = try JSONDecoder().decode(ThermalLearningResponse.self, from: data)
                if result.success { status = result.status; message = result.status.title }
                else { status = nil; message = "Learning status unavailable" }
            } catch is CancellationError { return }
            catch { status = nil; message = "Learning status unavailable" }
            do { try await Task.sleep(nanoseconds: 15_000_000_000) }
            catch { return }
        }
    }
}
private struct ThermalLearningStatusView: View {
    let nodeId: String
    @Environment(\.scenePhase) private var scenePhase
    @StateObject private var model = ThermalLearningModel()
    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack {
                Label("Room learning", systemImage: "sparkles")
                Spacer()
                Text(model.message)
                    .foregroundStyle(model.status?.state == "ready" && model.status?.stale == false ? Color.green : Color.secondary)
            }.font(.subheadline)
            if let updated = model.status?.lastUpdate {
                HStack(spacing: 4) {
                    Text("Last update")
                    Text(updated, style: .relative)
                    Text("ago")
                }.font(.caption).foregroundStyle(.secondary)
            }
            Text("Learns room background and sensor noise. Person and fall detection are not enabled.")
                .font(.caption2).foregroundStyle(.secondary)
            if let details = model.status?.learning, details.readErrors > 0 || details.samplingGaps > 0 {
                Text("Since restart: \(details.readErrors) camera read errors · \(details.samplingGaps) sampling interruptions")
                    .font(.caption2).foregroundStyle(.secondary)
            }
        }
        .task(id: scenePhase == .active ? nodeId : "") {
            guard scenePhase == .active else { model.pause(); return }
            await model.run(nodeId: nodeId)
        }
        .onDisappear { model.pause() }
    }
}
