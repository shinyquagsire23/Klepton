// Host check of KleptonWalkaboutSave.swift against a copy of a real save:
//   build/t_walkabout_unlock <android-files/files> <global-metadata.dat> [backup dir]
// Prints what it changed and reads every changed record back.
import Foundation

@main
struct TWalkaboutUnlock {
    static func main() throws {
        let args = CommandLine.arguments
        guard args.count >= 3 else { print("usage: t_walkabout_unlock <files dir> <global-metadata.dat> [backup]"); exit(2) }
        let files = URL(fileURLWithPath: args[1]), meta = URL(fileURLWithPath: args[2])
        let backup = args.count > 3 ? URL(fileURLWithPath: args[3]) : nil
        print("profiles:", KLWalkaboutUnlock.profiles(filesDir: files).map { "\($0.dir.lastPathComponent)#\($0.index)" })
        let t0 = Date()
        do {
            let k = try KLWalkaboutUnlock.key(metadata: meta)
            print(String(format: "key found in %.2f s", Date().timeIntervalSince(t0)))
            let r = try KLWalkaboutUnlock.unlockAll(filesDir: files, metadata: meta, backup: backup)
            print("changed: profiles \(r.profiles), balls +\(r.balls), putters +\(r.putters), courses \(r.courses), cosmetics \(r.cosmetics)")
            for p in KLWalkaboutUnlock.profiles(filesDir: files) {
                let a = try KLWalkaboutUnlock.decrypt(p.avatar, k) as! NSDictionary
                let g = (try KLWalkaboutUnlock.decrypt(p.golf, k) as! NSDictionary)["golfData"] as! NSDictionary
                let full = g.allValues.filter { (($0 as? NSDictionary)?["ballsFound"] as? Int) == 18 }.count
                if let c = (try? KLWalkaboutUnlock.decrypt(p.cosmetics, k) as? NSDictionary)?["cosmeticData"] as? NSDictionary {
                print("  cosmetics:", KLWalkaboutUnlock.earned.map { "\($0)=\((c[$0] as? NSDictionary)?["isUnlocked"] ?? "none")" })
            }
            print("\(p.dir.lastPathComponent)#\(p.index): balls \((a["ballsFound"] as! [Any]).count), putters \((a["puttersFound"] as! [Any]).count), courses at 18: \(full), avatar version \(a["version"]!)")
            }
            let again = try KLWalkaboutUnlock.unlockAll(filesDir: files, metadata: meta, backup: nil)
            print("second run changed: profiles \(again.profiles), balls +\(again.balls), putters +\(again.putters), cosmetics +\(again.cosmetics)")
        } catch {
            print("FAIL:", error); exit(1)
        }
    }
}
