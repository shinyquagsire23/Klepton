import Foundation

// What can be installed again stays out of the device's backup.
//
// A game's files are in the app's Documents folder, and the system backs that
// folder up — to iCloud where the owner has that on — unless an item is marked
// "excluded from backup". Half-Life: Alyx is 72 GB there, Deadpool 50. A marked
// folder leaves out everything in it.
//
// Marked are the places the installer fills and can fill again: the APK, the
// unpacked assets, the expansion folders, the Linux trees, the shader lists.
// Everything else is what the game wrote — saves, settings, the Steam Cloud
// records — and stays in the backup. Where a save lives inside a tree the
// installer filled (Alyx: lx/game/hlvr/save), only the folders on the way to it
// are opened and what lies beside it is marked.
//
// At every start, off the main thread: a file the installer replaces is a new
// file and has lost the mark. KL_BACKUP_EXCLUDE=0 marks nothing (what is marked
// stays marked).
//
// After Glovebox's BackupExclusion (github.com/glovebox-vr/glovebox,
// launcher/Sources/SteamLauncherKit/Util/BackupExclusion.swift, MIT), where a
// tester's device backup had grown by the game's 68.7 GB.
enum KLBackup {
    /// Inside a game's own folder (Documents/<tree>): what the installer puts there.
    private static let staged = ["assets", "qtplugins", "android-files/obb", "android-files/Android/obb"]
    /// Inside Documents/lx, the Linux trees: marked whole, or all but the named children.
    private static let linuxWhole = ["sysroot", "xrizer", "klfz"]
    private static let linuxKeep = ["game/hlvr/save", "game/hlvr/cfg"]

    static func keepGameFilesOut() {
        if let e = getenv("KL_BACKUP_EXCLUDE"), atoi(e) == 0 { return }
        Thread.detachNewThread {
            Thread.current.qualityOfService = .utility
            guard let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else { return }
            let n = apply(under: docs)
            NSLog("[app] backup: \(n) item(s) of the game's files are marked as left out of the device backup")
        }
    }

    /// Marks what the rules above name under `docs`; how many items carry the mark now.
    static func apply(under docs: URL) -> Int {
        let fm = FileManager.default
        var n = 0
        for item in (try? fm.contentsOfDirectory(at: docs, includingPropertiesForKeys: [.isDirectoryKey])) ?? [] {
            let name = item.lastPathComponent
            let isDir = (try? item.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory ?? false
            if !isDir {
                if name.lowercased().hasSuffix(".apk"), exclude(item) { n += 1 }
            } else if name == "klfz" {
                if exclude(item) { n += 1 }
            } else if name == "lx" {
                for whole in linuxWhole {
                    let u = item.appendingPathComponent(whole)
                    if fm.fileExists(atPath: u.path), exclude(u) { n += 1 }
                }
                let game = item.appendingPathComponent("game")
                if fm.fileExists(atPath: game.path) {
                    n += allBut(game, linuxKeep.map { $0.split(separator: "/").dropFirst().map(String.init) })
                }
            } else {
                for rel in staged {
                    let u = item.appendingPathComponent(rel)
                    if fm.fileExists(atPath: u.path), exclude(u) { n += 1 }
                }
            }
        }
        return n
    }

    /// Everything in `folder` but the `keep` paths: the folders on the way to
    /// one are opened, a kept one is left alone with all that is in it.
    private static func allBut(_ folder: URL, _ keep: [[String]]) -> Int {
        var n = 0
        for item in (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? [] {
            let inside = keep.filter { $0.first == item.lastPathComponent }.map { Array($0.dropFirst()) }
            if inside.contains(where: \.isEmpty) { continue }
            if !inside.isEmpty { n += allBut(item, inside) } else if exclude(item) { n += 1 }
        }
        return n
    }

    private static func exclude(_ item: URL) -> Bool {
        var item = item
        var v = URLResourceValues()
        v.isExcludedFromBackup = true
        return (try? item.setResourceValues(v)) != nil
    }
}
