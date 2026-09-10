// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <string_view>
namespace delete_io_fixture {
enum class Fault { none, native_create_sync, native_release_sync, publication_rename, publication_directory_sync };
void Arm(Fault, std::string_view database_path);
bool Disarm();
}
