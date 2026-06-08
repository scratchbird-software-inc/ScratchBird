// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Collections;
using System.Data.Common;

namespace ScratchBird.Data;

public sealed class ScratchBirdParameterCollection : DbParameterCollection
{
    private readonly List<ScratchBirdParameter> _items = new();

    public override int Count => _items.Count;
    public override object SyncRoot => ((ICollection)_items).SyncRoot;

    public ScratchBirdParameter this[int index]
    {
        get => _items[index];
        set => _items[index] = value;
    }

    public ScratchBirdParameter? this[string name]
    {
        get => Find(name);
        set
        {
            var index = IndexOf(name);
            if (index >= 0)
            {
                _items[index] = value ?? new ScratchBirdParameter();
            }
        }
    }

    public override int Add(object value)
    {
        if (value is not ScratchBirdParameter param)
        {
            throw new ArgumentException("Value must be ScratchBirdParameter");
        }
        _items.Add(param);
        return _items.Count - 1;
    }

    public ScratchBirdParameter Add(string name, object? value)
    {
        var param = new ScratchBirdParameter(name, value);
        _items.Add(param);
        return param;
    }

    public override void AddRange(Array values)
    {
        foreach (var value in values)
        {
            Add(value ?? new ScratchBirdParameter());
        }
    }

    public override void Clear()
    {
        _items.Clear();
    }

    public override bool Contains(object value)
    {
        return value is ScratchBirdParameter param && _items.Contains(param);
    }

    public override bool Contains(string value)
    {
        return IndexOf(value) >= 0;
    }

    public override void CopyTo(Array array, int index)
    {
        ((ICollection)_items).CopyTo(array, index);
    }

    public override IEnumerator GetEnumerator() => _items.GetEnumerator();

    public override int IndexOf(object value)
    {
        return value is ScratchBirdParameter param ? _items.IndexOf(param) : -1;
    }

    public override int IndexOf(string parameterName)
    {
        if (string.IsNullOrEmpty(parameterName))
        {
            return -1;
        }
        for (var i = 0; i < _items.Count; i++)
        {
            if (string.Equals(NormalizeName(_items[i].ParameterName), NormalizeName(parameterName), StringComparison.OrdinalIgnoreCase))
            {
                return i;
            }
        }
        return -1;
    }

    public override void Insert(int index, object value)
    {
        if (value is not ScratchBirdParameter param)
        {
            throw new ArgumentException("Value must be ScratchBirdParameter");
        }
        _items.Insert(index, param);
    }

    public override void Remove(object value)
    {
        if (value is ScratchBirdParameter param)
        {
            _items.Remove(param);
        }
    }

    public override void RemoveAt(int index)
    {
        _items.RemoveAt(index);
    }

    public override void RemoveAt(string parameterName)
    {
        var index = IndexOf(parameterName);
        if (index >= 0)
        {
            _items.RemoveAt(index);
        }
    }

    protected override DbParameter GetParameter(int index) => _items[index];

    protected override DbParameter GetParameter(string parameterName)
    {
        return Find(parameterName) ?? throw new IndexOutOfRangeException($"Parameter '{parameterName}' not found");
    }

    protected override void SetParameter(int index, DbParameter value)
    {
        if (value is not ScratchBirdParameter param)
        {
            throw new ArgumentException("Value must be ScratchBirdParameter");
        }
        _items[index] = param;
    }

    protected override void SetParameter(string parameterName, DbParameter value)
    {
        if (value is not ScratchBirdParameter param)
        {
            throw new ArgumentException("Value must be ScratchBirdParameter");
        }
        var index = IndexOf(parameterName);
        if (index < 0)
        {
            _items.Add(param);
        }
        else
        {
            _items[index] = param;
        }
    }

    private ScratchBirdParameter? Find(string name)
    {
        var index = IndexOf(name);
        return index >= 0 ? _items[index] : null;
    }

    private static string NormalizeName(string name)
    {
        return name.TrimStart('@', ':');
    }
}
