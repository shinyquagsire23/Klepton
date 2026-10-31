import SwiftUI

// The mods a build with mods carries, named for its player: the authors asked
// to be named where players see it (2026-10-30). The list comes with the build
// (Scotland2Metadata/credits.json, from ports/scotland2/credits.json); a build
// without mods has none, and Settings shows nothing.

struct KleptonModCredits: Decodable {
    struct Entry: Decodable, Identifiable {
        let name: String
        var version: String?
        let authors: String
        let url: String
        let license: String
        var role: String?
        var id: String { name }
    }
    let port: Entry
    let mods: [Entry]

    static let shipped: KleptonModCredits? = {
        guard let url = Bundle.main.url(forResource: "credits", withExtension: "json", subdirectory: "Scotland2Metadata"),
              let data = try? Data(contentsOf: url) else { return nil }
        return try? JSONDecoder().decode(KleptonModCredits.self, from: data)
    }()
}

struct ModCreditsView: View {
    var body: some View {
        if let credits = KleptonModCredits.shipped {
            DisclosureGroup(klT("Mods", "Mods")) {
                VStack(alignment: .leading, spacing: 10) {
                    Text(klT("This game runs with mods made by the Beat Saber modding community, unchanged in what they do and brought to the Vision Pro. Thank you to their authors.",
                             "Dieses Spiel läuft mit Mods aus der Beat-Saber-Modding-Community, in ihrer Funktion unverändert und auf die Vision Pro gebracht. Danke an ihre Autoren."))
                        .font(.caption).foregroundStyle(.secondary)
                    row(credits.port)
                    Divider()
                    ForEach(credits.mods) { row($0) }
                }
                .padding(.top, 6)
            }
        }
    }

    private func row(_ e: KleptonModCredits.Entry) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(e.name + (e.version.map { " " + $0 } ?? "")).font(.callout)
            Text(e.authors + " · " + (e.license.isEmpty ? klT("no licence stated", "keine Lizenzangabe") : e.license))
                .font(.caption).foregroundStyle(.secondary)
            Text(e.url).font(.caption2).foregroundStyle(.tertiary).textSelection(.enabled)
        }
    }
}
