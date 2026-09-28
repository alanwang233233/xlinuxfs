//
//  DiskArbitrationMonitor.swift
//  Detects ext3/4 volumes via DiskArbitration so the app can list them and offer
//  manual mount / unmount. Detection only — auto-mounting is handled by the
//  system + the file-system extension (FSMediaTypes).
//

import Foundation
import DiskArbitration
import IOKit

final class DiskArbitrationMonitor {

    /// Called (on the main queue) whenever the known device set changes.
    var onDevicesChanged: (([LinuxDevice]) -> Void)?

    private let session: DASession
    private let queue = DispatchQueue(label: "com.huanchuan.xlinuxfs.diskarb")
    private let callbackContext = MonitorCallbackContext()
    private var devices: [String: LinuxDevice] = [:]
    private let lock = NSLock()

    init?() {
        guard let s = DASessionCreate(kCFAllocatorDefault) else { return nil }
        session = s
        DASessionSetDispatchQueue(session, queue)
        callbackContext.monitor = self
    }

    func start() {
        queue.async { [weak self] in
            guard let self else { return }
            let ctx = Unmanaged.passUnretained(self.callbackContext).toOpaque()
            DARegisterDiskAppearedCallback(self.session, nil, daAppeared, ctx)
            DARegisterDiskDisappearedCallback(self.session, nil, daDisappeared, ctx)
            DARegisterDiskDescriptionChangedCallback(self.session, nil, nil, daChanged, ctx)
        }
    }

    deinit {
        // Drain earlier callbacks before releasing their context or session.
        let session = session, context = callbackContext
        queue.async {
            withExtendedLifetime(context) {
                let ctx = Unmanaged.passUnretained(context).toOpaque()
                DAUnregisterCallback(session, unsafeBitCast(daAppeared as DADiskAppearedCallback, to: UnsafeMutableRawPointer.self), ctx)
                DAUnregisterCallback(session, unsafeBitCast(daDisappeared as DADiskDisappearedCallback, to: UnsafeMutableRawPointer.self), ctx)
                DAUnregisterCallback(session, unsafeBitCast(daChanged as DADiskDescriptionChangedCallback, to: UnsafeMutableRawPointer.self), ctx)
                DASessionSetDispatchQueue(session, nil)
            }
        }
    }

    func sessionRef() -> DASession { session }

    func refresh() {
        queue.async { [weak self] in
            guard let self else { return }
            for device in self.currentDevices {
                if let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, self.session, device.id) {
                    self.handleAppearedOrChanged(disk)
                } else {
                    self.lock.lock()
                    self.devices.removeValue(forKey: device.id)
                    self.lock.unlock()
                    self.publish()
                }
            }
        }
    }

    var currentDevices: [LinuxDevice] {
        lock.lock(); defer { lock.unlock() }
        return Array(devices.values).sorted { $0.id < $1.id }
    }

    // MARK: callback handlers

    fileprivate func handleAppearedOrChanged(_ disk: DADisk) {
        guard let bsd = bsdName(disk) else { return }
        guard let dev = makeDevice(disk, bsd: bsd) else {
            // A DA "changed" event left this device no longer matching (e.g. reformatted to a
            // non-Linux fs, or our module unmounted it) — drop any tracked entry so it leaves
            // the list instead of lingering stale.
            lock.lock(); let removed = devices.removeValue(forKey: bsd) != nil; lock.unlock()
            if removed { publish() }
            return
        }
        lock.lock(); devices[bsd] = dev; lock.unlock()
        publish()
    }

    fileprivate func handleDisappeared(_ disk: DADisk) {
        guard let bsd = bsdName(disk) else { return }
        lock.lock(); devices[bsd] = nil; lock.unlock()
        publish()
    }

    private func publish() {
        let list = currentDevices
        DispatchQueue.main.async { [weak self] in self?.onDevicesChanged?(list) }
    }

    // MARK: description parsing

    private func bsdName(_ disk: DADisk) -> String? {
        guard let c = DADiskGetBSDName(disk) else { return nil }
        return String(cString: c)
    }

    private func description(_ disk: DADisk) -> [String: Any]? {
        DADiskCopyDescription(disk) as? [String: Any]
    }

    /// The mount fs-type our extension reports — must match the extension's
    /// FSShortName (lklfuse/Info.plist). Used to recognize volumes we're serving.
    static let moduleFSType = "xlinuxfs"

    /// DADeviceModel value reported by disk-image-backed devices.
    static let imageDeviceModel = "Disk Image"

    /// GPT "Linux filesystem data" partition GUID — ext3/4 live under this type.
    /// (TODO: verify the exact DA content hints for ext volumes on a real machine;
    /// MBR disks may report "Linux" or a 0x83 hint instead.)
    static let linuxFilesystemGUID = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"

    private func makeDevice(_ disk: DADisk, bsd: String) -> LinuxDevice? {
        guard let d = description(disk) else { return nil }
        let leaf = d[kDADiskDescriptionMediaLeafKey as String] as? Bool ?? false
        let content = d[kDADiskDescriptionMediaContentKey as String] as? String ?? ""
        let kind = (d[kDADiskDescriptionVolumeKindKey as String] as? String ?? "").lowercased()
        let mountURL = d[kDADiskDescriptionVolumePathKey as String] as? URL

        // Authoritative: is the volume actually mounted through our module? The media
        // content hint can't be trusted for mounted volumes, so check the real mount fs-type.
        let info = mountURL.flatMap { Self.mountInfo($0) }
        let byModule = info?.fsType == Self.moduleFSType
        let isLinuxMedia = content == Self.linuxFilesystemGUID
            || content == "Linux"
            || kind == "ext2" || kind == "ext3" || kind == "ext4"
            || kind == "xfs" || kind == "btrfs"
            || kind == Self.moduleFSType
        // Image-backed devices report DADeviceModel == "Disk Image".
        let isImage = (d[kDADiskDescriptionDeviceModelKey as String] as? String) == Self.imageDeviceModel
        // A just-attached partitionless raw Linux image is a whole leaf disk macOS can't
        // identify, so it carries no content hint / VolumeKind and wouldn't match isLinuxMedia.
        // Surface it anyway so it can be mounted / detached from the UI — otherwise an
        // attached-but-unmounted raw image is invisible and strands the user at the CLI.
        let isUnknownRawImage = isImage && leaf && content.isEmpty
        guard byModule || (isLinuxMedia && leaf) || isUnknownRawImage else { return nil }

        let name = d[kDADiskDescriptionVolumeNameKey as String] as? String
            ?? (d[kDADiskDescriptionMediaNameKey as String] as? String) ?? bsd
        let size = (d[kDADiskDescriptionMediaSizeKey as String] as? NSNumber)?.uint64Value ?? 0
        let removable = (d[kDADiskDescriptionMediaRemovableKey as String] as? Bool) ?? false
        let ejectable = (d[kDADiskDescriptionMediaEjectableKey as String] as? Bool) ?? false

        // Image-backed devices' volumes go in the "Disk Images" section and detach via the
        // whole-disk node (disk<unit>); isImage was determined above.
        let deviceKind: DeviceKind = isImage ? .diskImage : ((removable || ejectable) ? .removable : .fixed)

        var dev = LinuxDevice(
            id: bsd,
            volumeName: name,
            sizeBytes: size,
            kind: deviceKind,
            contentHint: content,
            isRemovable: removable || ejectable,
            devicePath: "/dev/\(bsd)")
        dev.mountedByModule = byModule
        dev.fsKind = kind
        dev.readOnly = info?.readOnly ?? false
        dev.mediaWritable = (d[kDADiskDescriptionMediaWritableKey as String] as? Bool) ?? true
        let media = DADiskCopyIOMedia(disk)
        if media != IO_OBJECT_NULL {
            var entryID: UInt64 = 0
            if IORegistryEntryGetRegistryEntryID(media, &entryID) == KERN_SUCCESS, entryID != 0 {
                dev.registryEntryID = entryID
            }
            IOObjectRelease(media)
        }
        // The whole-disk node for detach — straight from DiskArbitration, not inferred from
        // the BSD unit number.
        if isImage {
            if let whole = DADiskCopyWholeDisk(disk), let c = DADiskGetBSDName(whole) {
                dev.wholeDiskBSD = String(cString: c)
            } else {
                // Fallback so detach always has a valid node: strip the partition suffix
                // (disk6s1 -> disk6); a whole-disk name is left unchanged.
                dev.wholeDiskBSD = bsd.replacingOccurrences(of: #"s\d+$"#, with: "", options: .regularExpression)
            }
        }
        if let url = mountURL { dev.state = .mounted(url) }
        return dev
    }

    /// `(f_fstypename, read-only)` of the filesystem mounted at `url`, or nil if not mounted.
    static func mountInfo(_ url: URL) -> (fsType: String, readOnly: Bool)? {
        guard url.isFileURL else { return nil }
        var s = statfs()
        guard statfs(url.path, &s) == 0 else { return nil }
        let fsType = withUnsafeBytes(of: &s.f_fstypename) { raw in
            String(cString: raw.bindMemory(to: CChar.self).baseAddress!)
        }
        return (fsType, (s.f_flags & UInt32(MNT_RDONLY)) != 0)
    }
}

// MARK: - C trampolines

private final class MonitorCallbackContext {
    weak var monitor: DiskArbitrationMonitor?
}

private func monitor(_ ctx: UnsafeMutableRawPointer?) -> DiskArbitrationMonitor? {
    guard let ctx else { return nil }
    return Unmanaged<MonitorCallbackContext>.fromOpaque(ctx).takeUnretainedValue().monitor
}

private func daAppeared(_ disk: DADisk, _ ctx: UnsafeMutableRawPointer?) {
    monitor(ctx)?.handleAppearedOrChanged(disk)
}
private func daDisappeared(_ disk: DADisk, _ ctx: UnsafeMutableRawPointer?) {
    monitor(ctx)?.handleDisappeared(disk)
}
private func daChanged(_ disk: DADisk, _ keys: CFArray, _ ctx: UnsafeMutableRawPointer?) {
    monitor(ctx)?.handleAppearedOrChanged(disk)
}
