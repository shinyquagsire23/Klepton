import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

/// A binary message pipe to a Steam connection manager (CM). On Apple platforms this is the
/// same WebSocket (`wss://<cm>/cmsocket/`) the Steam client and SteamKit2 use.
public protocol CMTransport: AnyObject {
    func connect(url: URL) async throws
    func send(_ data: Data) async throws
    func receive() async throws -> Data
    func close()
}

public enum TransportError: Error, CustomStringConvertible {
    case closed
    case unexpectedMessage
    case bridge(String)
    public var description: String {
        switch self {
        case .closed: return "the connection to Steam closed"
        case .unexpectedMessage: return "Steam sent a text frame on a binary socket"
        case .bridge(let s): return "WebSocket bridge: \(s)"
        }
    }
}

#if !os(Linux)
public final class WebSocketTransport: CMTransport {
    private var task: URLSessionWebSocketTask?
    private let session: URLSession

    public init(session: URLSession = .shared) { self.session = session }

    public func connect(url: URL) async throws {
        let t = session.webSocketTask(with: url)
        t.maximumMessageSize = 64 << 20
        task = t
        t.resume()
    }

    public func send(_ data: Data) async throws {
        guard let task else { throw TransportError.closed }
        try await task.send(.data(data))
    }

    public func receive() async throws -> Data {
        guard let task else { throw TransportError.closed }
        switch try await task.receive() {
        case .data(let d): return d
        default: throw TransportError.unexpectedMessage
        }
    }

    public func close() {
        task?.cancel(with: .normalClosure, reason: nil)
        task = nil
    }
}
#endif

#if os(Linux)
import Glibc

/// Linux has no WebSocket support in Foundation, so tests on a Linux box go through a tiny local
/// bridge (`tools/ws_bridge.py`) that speaks length-prefixed frames on 127.0.0.1 and real
/// WebSockets to Steam. The protocol code above the transport is identical.
public final class BridgeTransport: CMTransport {
    private var fd: Int32 = -1
    private let port: UInt16
    private let queue = DispatchQueue(label: "steam.bridge")

    public init(port: UInt16) { self.port = port }

    public func connect(url: URL) async throws {
        fd = socket(AF_INET, Int32(SOCK_STREAM.rawValue), 0)
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        let rc = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Glibc.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        guard rc == 0 else { throw TransportError.bridge("can't reach 127.0.0.1:\(port)") }
        try await send(Data(url.absoluteString.utf8))
    }

    private func writeAll(_ d: Data) throws {
        try d.withUnsafeBytes { (p: UnsafeRawBufferPointer) in
            var off = 0
            while off < p.count {
                let n = Glibc.write(fd, p.baseAddress! + off, p.count - off)
                if n <= 0 { throw TransportError.closed }
                off += n
            }
        }
    }

    private func readExactly(_ n: Int) throws -> Data {
        var out = Data(count: n)
        var off = 0
        try out.withUnsafeMutableBytes { (p: UnsafeMutableRawBufferPointer) in
            while off < n {
                let r = Glibc.read(fd, p.baseAddress! + off, n - off)
                if r <= 0 { throw TransportError.closed }
                off += r
            }
        }
        return out
    }

    public func send(_ data: Data) async throws {
        var len = UInt32(data.count).bigEndian
        var frame = Data(bytes: &len, count: 4)
        frame.append(data)
        try queue.sync { try writeAll(frame) }
    }

    public func receive() async throws -> Data {
        try await withCheckedThrowingContinuation { cont in
            DispatchQueue.global().async {
                do {
                    let h = try self.readExactly(4)
                    let n = Int(UInt32(h[0]) << 24 | UInt32(h[1]) << 16 | UInt32(h[2]) << 8 | UInt32(h[3]))
                    cont.resume(returning: try self.readExactly(n))
                } catch { cont.resume(throwing: error) }
            }
        }
    }

    public func close() {
        if fd >= 0 { Glibc.close(fd); fd = -1 }
    }
}
#endif

public enum Transports {
    /// The default transport for this platform.
    public static func make() -> CMTransport {
        #if os(Linux)
        let port = UInt16(ProcessInfo.processInfo.environment["STEAM_WS_BRIDGE_PORT"] ?? "") ?? 27099
        return BridgeTransport(port: port)
        #else
        return WebSocketTransport()
        #endif
    }
}
