// swift-tools-version: 5.10.1
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import PackageDescription

let package = Package(
    name: "ScratchBird",
    platforms: [
        .macOS(.v13),
        .iOS(.v16)
    ],
    products: [
        .library(name: "ScratchBird", targets: ["ScratchBird"]),
        .executable(name: "SBIsqlSwift", targets: ["SBIsqlSwift"])
    ],
    dependencies: [
        .package(url: "https://github.com/apple/swift-crypto.git", from: "3.1.0"),
        .package(url: "https://github.com/apple/swift-nio.git", from: "2.74.0"),
        .package(url: "https://github.com/apple/swift-nio-ssl.git", from: "2.30.0")
    ],
    targets: [
        .target(
            name: "ScratchBird",
            dependencies: [
                .product(name: "Crypto", package: "swift-crypto"),
                .product(name: "NIOPosix", package: "swift-nio"),
                .product(name: "NIOSSL", package: "swift-nio-ssl")
            ]
        ),
        .testTarget(
            name: "ScratchBirdTests",
            dependencies: ["ScratchBird"]
        ),
        .executableTarget(
            name: "SBIsqlSwift",
            dependencies: ["ScratchBird", .product(name: "Crypto", package: "swift-crypto")]
        )
    ]
)
