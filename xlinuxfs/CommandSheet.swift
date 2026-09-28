//
//  CommandSheet.swift
//  Sheets that present copyable Terminal commands (attach / detach an image). The app
//  never runs them — the sandbox blocks hdiutil/mount. macOS can't attach a Linux disk
//  image through Disk Utility, so attaching is a guided `hdiutil attach` command; once the
//  image is attached the extension auto-mounts any ext/XFS/Btrfs volume on it.
//

import SwiftUI

/// A picked image file waiting to be attached (wrapper for `.sheet(item:)`).
struct PendingAttach: Identifiable {
    let id = UUID()
    let url: URL
}

/// Attach a disk image: choose read-only (default) or writable, then copy the
/// `hdiutil attach` command. Read-only by default so an image isn't written by accident;
/// a physical drive is unaffected (it auto-mounts from its own writable media).
struct AttachImageSheet: View {
    @Environment(\.dismiss) private var dismiss
    let url: URL
    @State private var readOnly = true   // on == read-only; images default to read-only

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Attach Disk Image").font(.title3).bold()
            Text(url.lastPathComponent)
                .font(.callout).foregroundStyle(.secondary)
                .lineLimit(1).truncationMode(.middle)

            Toggle("Mount read-only", isOn: $readOnly)
            Text(readOnly ? "The image will mount read-only (recommended)."
                          : "The image is attached writable; whether it mounts read-only then follows your Disk Images setting.")
                .font(.caption).foregroundStyle(.secondary)

            Divider()
            Text("Run this in Terminal — a sandboxed app can't do it for you:")
                .font(.callout).foregroundStyle(.secondary)
            CopyableCommand(command: MountService.attachCommand(url.path, readOnly: readOnly))
            Text("Once attached, the volume appears in the list and mounts automatically.")
                .font(.caption).foregroundStyle(.secondary)

            Spacer(minLength: 0)
            HStack { Spacer(); Button("Close") { dismiss() }.keyboardShortcut(.defaultAction) }
        }
        .padding(20)
        .frame(width: 580, height: 340)
    }
}

/// A small sheet presenting one fixed copyable command (e.g. detach).
struct CommandSheet: View {
    @Environment(\.dismiss) private var dismiss
    let title: LocalizedStringKey
    let command: String

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(title).font(.title3).bold()
            Text("Run this in Terminal — a sandboxed app can't do it for you:")
                .font(.callout).foregroundStyle(.secondary)
            CopyableCommand(command: command)
            Spacer(minLength: 0)
            HStack { Spacer(); Button("Close") { dismiss() }.keyboardShortcut(.defaultAction) }
        }
        .padding(20)
        .frame(width: 580, height: 230)
    }
}
