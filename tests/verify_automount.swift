import Foundation

let data = try Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]))
let plist = try PropertyListSerialization.propertyList(from: data, format: nil) as! [String: Any]
let entries = plist["system-entities"] as! [[String: Any]]
let mounts = entries.compactMap { $0["mount-point"] as? String }
precondition(mounts.count == 1, "Expected one auto-mounted Linux volume")
for mount in mounts {
    var info = statfs()
    precondition(statfs(mount, &info) == 0)
    let type = withUnsafeBytes(of: info.f_fstypename) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
    precondition(type == "xlinuxfs" && info.f_flags & UInt32(MNT_RDONLY) != 0)
    let entries = try FileManager.default.contentsOfDirectory(atPath: mount)
    precondition(!entries.isEmpty)
    print("PASS system auto-mount: \(mount), xlinuxfs, read-only, readable directory")
}
