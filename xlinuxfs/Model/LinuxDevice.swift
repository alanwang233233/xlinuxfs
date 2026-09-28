//
//  LinuxDevice.swift
//  A removable / image-backed ext4 volume the app knows about.
//

import Foundation

enum DeviceKind: Equatable {
    case removable        // physical disk (USB, SD, etc.)
    case diskImage        // attached raw ext4 image file
    case fixed            // internal / non-removable
}

enum MountState: Equatable {
    case unmounted
    case mounting
    case mounted(URL)              // current mount point
    case unmounting
    case failed(String)            // last error message

    var isMounted: Bool { if case .mounted = self { return true }; return false }
    var mountPoint: URL? { if case .mounted(let u) = self { return u }; return nil }
}

/// One ext4 volume/partition. Identified by its BSD name (e.g. `disk4s1`).
struct LinuxDevice: Identifiable, Equatable {
    let id: String                 // BSD name, e.g. "disk4s1"
    var volumeName: String         // on-disk label or fallback
    var sizeBytes: UInt64
    var kind: DeviceKind
    var contentHint: String        // DiskArbitration media content (e.g. "Linux_Filesystem")
    /// DiskArbitration VolumeKind, lowercased ("ext4"/"xfs"/"btrfs"); "" if unknown
    /// or (when mounted through our module) the module fs-type.
    var fsKind: String = ""
    var isRemovable: Bool
    var devicePath: String         // "/dev/disk4s1"
    var state: MountState = .unmounted
    /// True when the volume is currently mounted through our FSKit module (its
    /// mount fs-type matches the extension's FSShortName), regardless of whether
    /// this app or the system performed the mount.
    var mountedByModule: Bool = false
    /// True when the mounted volume is read-only (statfs MNT_RDONLY).
    var readOnly: Bool = false
    /// Whether the underlying media is writable (DAMediaWritable). When false the volume
    /// can only be mounted read-only — e.g. an image attached with `hdiutil attach -readonly`.
    var mediaWritable: Bool = true
    /// For image-backed devices (kind == .diskImage): the whole-disk node (e.g. "disk6"),
    /// used to detach the image via `hdiutil detach`.
    var wholeDiskBSD: String = ""
    /// Changes on reconnect, even when the BSD name is reused.
    var registryEntryID: UInt64?

    var displayName: String { volumeName.isEmpty ? id : volumeName }

    /// Human format label for display: the concrete Linux fs when known, else "Linux".
    var formatDisplay: String {
        switch fsKind {
        case "ext2", "ext3", "ext4", "xfs", "btrfs": return fsKind
        default: return "Linux"
        }
    }

    static func == (lhs: LinuxDevice, rhs: LinuxDevice) -> Bool {
        lhs.id == rhs.id && lhs.volumeName == rhs.volumeName &&
        lhs.state == rhs.state && lhs.sizeBytes == rhs.sizeBytes &&
        lhs.mountedByModule == rhs.mountedByModule && lhs.readOnly == rhs.readOnly &&
        lhs.mediaWritable == rhs.mediaWritable && lhs.registryEntryID == rhs.registryEntryID
    }
}

extension UInt64 {
    var humanSize: String {
        ByteCountFormatter.string(fromByteCount: Int64(self), countStyle: .file)
    }
}
