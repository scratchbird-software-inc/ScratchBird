// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Data.Common;

namespace ScratchBird.Data;

public sealed class ScratchBirdFactory : DbProviderFactory
{
    public static readonly ScratchBirdFactory Instance = new();

    public override DbConnection CreateConnection() => new ScratchBirdConnection();
    public override DbCommand CreateCommand() => new ScratchBirdCommand();
    public override DbParameter CreateParameter() => new ScratchBirdParameter();
}
