//
//  lklfuse.swift
//  lklfuse
//
//  Created by kkHAIKE on 2026/6/19.
//

import ExtensionFoundation
import Foundation
import FSKit

@main
struct lklfuse : UnaryFileSystemExtension {
    let fileSystem = lklfuseFileSystem()
}
