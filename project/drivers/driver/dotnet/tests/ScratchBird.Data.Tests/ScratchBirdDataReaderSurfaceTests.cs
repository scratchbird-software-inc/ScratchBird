// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System;
using System.Buffers.Binary;
using System.Data;
using System.IO;
using System.Reflection;
using System.Text;
using Xunit;

namespace ScratchBird.Data.Tests;

public class ScratchBirdDataReaderSurfaceTests
{
    [Fact]
    public void NextResultTraversesMultipleResultSetsWithoutLiveConnection()
    {
        using var reader = CreateReader(
            BuildRowDescriptionMessage(1, "first_value"),
            BuildDataRowMessage(2, "1"),
            BuildCommandCompleteMessage(3, 1, 0, "SELECT"),
            BuildRowDescriptionMessage(4, "second_value"),
            BuildDataRowMessage(5, "2"),
            BuildCommandCompleteMessage(6, 1, 0, "SELECT"),
            BuildReadyMessage(7, 17));

        Assert.True(reader.Read());
        Assert.Equal(1, Convert.ToInt32(reader.GetValue(0)));
        Assert.False(reader.Read());

        Assert.True(reader.NextResult());
        Assert.True(reader.Read());
        Assert.Equal(2, Convert.ToInt32(reader.GetValue(0)));
        Assert.False(reader.Read());

        Assert.False(reader.NextResult());
    }

    [Fact]
    public void NextResultSkipsUnreadRowsAndAdvancesWithoutLiveConnection()
    {
        using var reader = CreateReader(
            BuildRowDescriptionMessage(1, "value"),
            BuildDataRowMessage(2, "1"),
            BuildDataRowMessage(3, "2"),
            BuildCommandCompleteMessage(4, 2, 0, "SELECT"),
            BuildRowDescriptionMessage(5, "value"),
            BuildDataRowMessage(6, "3"),
            BuildCommandCompleteMessage(7, 1, 0, "SELECT"),
            BuildReadyMessage(8, 23));

        Assert.True(reader.Read());
        Assert.Equal(1, Convert.ToInt32(reader.GetValue(0)));

        Assert.True(reader.NextResult());
        Assert.True(reader.Read());
        Assert.Equal(3, Convert.ToInt32(reader.GetValue(0)));
        Assert.False(reader.Read());
        Assert.False(reader.NextResult());
    }

    private static ScratchBirdDataReader CreateReader(params ProtocolMessage[] messages)
    {
        var client = new ProtocolClient();
        var buffer = new MemoryStream();
        foreach (var message in messages)
        {
            var bytes = message.ToBytes();
            buffer.Write(bytes, 0, bytes.Length);
        }
        buffer.Position = 0;

        SetPrivateField(client, "_stream", buffer);
        SetPrivateField(client, "_connected", true);

        var stream = new ProtocolClient.QueryStream(client, timeoutMs: 0, pageSize: 0);
        return new ScratchBirdDataReader(stream, CommandBehavior.Default, connection: null);
    }

    private static ProtocolMessage BuildRowDescriptionMessage(uint sequence, string columnName)
    {
        var nameBytes = Encoding.UTF8.GetBytes(columnName);
        var payload = new byte[4 + 4 + nameBytes.Length + 4 + 2 + 4 + 2 + 4 + 1 + 1 + 2];
        var offset = 0;
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(offset, 2), 1);
        offset += 4;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(offset, 4), (uint)nameBytes.Length);
        offset += 4;
        Buffer.BlockCopy(nameBytes, 0, payload, offset, nameBytes.Length);
        offset += nameBytes.Length;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(offset, 4), 0);
        offset += 4;
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(offset, 2), 1);
        offset += 2;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(offset, 4), 23);
        offset += 4;
        BinaryPrimitives.WriteInt16LittleEndian(payload.AsSpan(offset, 2), 4);
        offset += 2;
        BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(offset, 4), -1);
        offset += 4;
        payload[offset++] = 0;
        payload[offset++] = 1;
        return BuildMessage(MessageType.ROW_DESCRIPTION, sequence, payload);
    }

    private static ProtocolMessage BuildDataRowMessage(uint sequence, string value)
    {
        var valueBytes = Encoding.UTF8.GetBytes(value);
        var payload = new byte[4 + 4 + valueBytes.Length];
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(0, 2), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(2, 2), 0);
        BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(4, 4), valueBytes.Length);
        Buffer.BlockCopy(valueBytes, 0, payload, 8, valueBytes.Length);
        return BuildMessage(MessageType.DATA_ROW, sequence, payload);
    }

    private static ProtocolMessage BuildCommandCompleteMessage(uint sequence, ulong rows, ulong lastId, string tag)
    {
        var tagBytes = Encoding.UTF8.GetBytes(tag + "\0");
        var payload = new byte[20 + tagBytes.Length];
        payload[0] = 0;
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(4, 8), rows);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(12, 8), lastId);
        Buffer.BlockCopy(tagBytes, 0, payload, 20, tagBytes.Length);
        return BuildMessage(MessageType.COMMAND_COMPLETE, sequence, payload);
    }

    private static ProtocolMessage BuildReadyMessage(uint sequence, ulong txnId)
    {
        var payload = new byte[20];
        payload[0] = 0;
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(4, 8), txnId);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(12, 8), txnId);
        return BuildMessage(MessageType.READY, sequence, payload, txnId);
    }

    private static ProtocolMessage BuildMessage(MessageType type, uint sequence, byte[] payload, ulong txnId = 0)
    {
        return new ProtocolMessage(
            new MessageHeader((byte)type, 0, (uint)payload.Length, sequence, new byte[16], txnId),
            payload);
    }

    private static void SetPrivateField(object target, string fieldName, object? value)
    {
        var field = target.GetType().GetField(fieldName, BindingFlags.Instance | BindingFlags.NonPublic);
        Assert.NotNull(field);
        field!.SetValue(target, value);
    }
}
