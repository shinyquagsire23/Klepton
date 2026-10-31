import Foundation

/// What Steam Cloud needs to know about one game beyond what Steam says itself (the Auto-Cloud rules come from
/// Steam's product info): where its root tokens point in the staged tree, and which files never go up.
///
/// Not from Glovebox's kit, which has this in its depot downloader's game profile; the values for Alyx are its.
public struct CloudGame: Sendable {
    public let appID: UInt32
    /// Root token (lowercased, e.g. `gameinstall`) → folder relative to the game's staged tree.
    public let roots: [String: String]
    /// Save-folder file names (globs) that never go to Steam Cloud.
    public let neverUpload: [String]

    public init(appID: UInt32, roots: [String: String], neverUpload: [String]) {
        self.appID = appID; self.roots = roots; self.neverUpload = neverUpload
    }

    /// Half-Life: Alyx. Its one rule is `gameinstall` / `game/hlvr/save`, and the staged tree (Documents/lx) holds
    /// the depot's `game/` under that name. personal.cfg is this device's settings (a PC would get the headset's);
    /// the .hl1/.hl2/.hl3 files hold the level the game was in, left behind because the app is ended rather than
    /// the game quit (a PC's cloud saves have none). The .bad files are empty markers beside a save and say
    /// nothing a PC wants (ours, not Glovebox's: seen on a headset beside the first uploads that Steam refused).
    public static let halfLifeAlyx = CloudGame(appID: 546_560, roots: ["gameinstall": ""],
                                               neverUpload: ["personal.cfg", "*.hl1", "*.hl2", "*.hl3", "*.bad"])

    public static func known(appID: UInt32) -> CloudGame? { appID == halfLifeAlyx.appID ? halfLifeAlyx : nil }
}
