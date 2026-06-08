// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Data;
using System.Data.Common;

namespace ScratchBird.Data;

public sealed class ScratchBirdParameter : DbParameter
{
    private DbType _dbType = DbType.Object;
    private ParameterDirection _direction = ParameterDirection.Input;
    private bool _isNullable = true;
    private string _parameterName = string.Empty;
    private string _sourceColumn = string.Empty;
    private bool _sourceColumnNullMapping;
    private DataRowVersion _sourceVersion = DataRowVersion.Current;
    private object? _value;
    private byte _precision;
    private byte _scale;
    private int _size;

    public ScratchBirdParameter() { }

    public ScratchBirdParameter(string name, object? value)
    {
        ParameterName = name;
        Value = value;
    }

    public override DbType DbType
    {
        get => _dbType;
        set => _dbType = value;
    }

    public override ParameterDirection Direction
    {
        get => _direction;
        set
        {
            if (value != ParameterDirection.Input
                && value != ParameterDirection.Output
                && value != ParameterDirection.InputOutput
                && value != ParameterDirection.ReturnValue)
            {
                throw new NotSupportedException("Supported parameter directions: Input, Output, InputOutput, ReturnValue");
            }
            _direction = value;
        }
    }

    public override bool IsNullable
    {
        get => _isNullable;
        set => _isNullable = value;
    }

    public override string ParameterName
    {
        get => _parameterName;
        set => _parameterName = value ?? string.Empty;
    }

    public override string SourceColumn
    {
        get => _sourceColumn;
        set => _sourceColumn = value ?? string.Empty;
    }

    public override bool SourceColumnNullMapping
    {
        get => _sourceColumnNullMapping;
        set => _sourceColumnNullMapping = value;
    }

    public override DataRowVersion SourceVersion
    {
        get => _sourceVersion;
        set => _sourceVersion = value;
    }

    public override object? Value
    {
        get => _value;
        set => _value = value;
    }

    public override byte Precision
    {
        get => _precision;
        set => _precision = value;
    }

    public override byte Scale
    {
        get => _scale;
        set => _scale = value;
    }

    public override int Size
    {
        get => _size;
        set => _size = value;
    }

    public override void ResetDbType()
    {
        _dbType = DbType.Object;
    }
}
