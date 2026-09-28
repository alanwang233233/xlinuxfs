//
//  lklfuseItem.swift
//  An FSItem backed by an ext3/4 inode number.
//

import FSKit
import Foundation

@available(macOS 15.4, *)
final class lklfuseItem: FSItem {
    /// ext3/4 inode number. The root directory is always inode 2 (EXT4_ROOT_INO).
    static let rootIno: UInt64 = 2

    let ino: UInt64
    var parentIno: UInt64
    var name: FSFileName

    init(ino: UInt64, parentIno: UInt64, name: FSFileName) {
        self.ino = ino
        self.parentIno = parentIno
        self.name = name
        super.init()
    }
}
