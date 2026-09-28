import Foundation

let data = try Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]))
let plist = try PropertyListSerialization.propertyList(from: data, format: nil) as! [String: Any]
let entries = plist["system-entities"] as! [[String: Any]]
let nodes = entries.compactMap { $0["dev-entry"] as? String }
guard let whole = nodes.first(where: { $0.range(of: #"^/dev/disk[0-9]+$"#, options: .regularExpression) != nil }),
      let partition = nodes.first(where: { $0.hasPrefix(whole + "s") }) else {
    fatalError("Expected a disposable image with one partition")
}
print(whole)
print(partition)
