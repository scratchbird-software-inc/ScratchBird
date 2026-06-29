// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Buffers.Binary;
using System.IO;
using System.Linq;
using System.Text;

namespace ScratchBird.Data;

internal enum MessageType : byte
{
    STARTUP = 0x01,
    AUTH_RESPONSE = 0x02,
    QUERY = 0x03,
    PARSE = 0x04,
    BIND = 0x05,
    DESCRIBE = 0x06,
    EXECUTE = 0x07,
    CLOSE = 0x08,
    SYNC = 0x09,
    FLUSH = 0x0A,
    CANCEL = 0x0B,
    TERMINATE = 0x0C,
    COPY_DATA = 0x0D,
    COPY_DONE = 0x0E,
    COPY_FAIL = 0x0F,
    SBLR_EXECUTE = 0x10,
    SUBSCRIBE = 0x11,
    UNSUBSCRIBE = 0x12,
    FEDERATED_QUERY = 0x13,
    STREAM_CONTROL = 0x14,
    TXN_BEGIN = 0x15,
    TXN_COMMIT = 0x16,
    TXN_ROLLBACK = 0x17,
    TXN_SAVEPOINT = 0x18,
    TXN_RELEASE = 0x19,
    TXN_ROLLBACK_TO = 0x1A,
    PING = 0x1B,
    SET_OPTION = 0x1C,
    CLUSTER_AUTH = 0x1D,
    ATTACH_CREATE = 0x1E,
    ATTACH_DETACH = 0x1F,
    ATTACH_LIST = 0x20,

    AUTH_REQUEST = 0x40,
    AUTH_OK = 0x41,
    AUTH_CONTINUE = 0x42,
    READY = 0x43,
    ROW_DESCRIPTION = 0x44,
    DATA_ROW = 0x45,
    COMMAND_COMPLETE = 0x46,
    EMPTY_QUERY = 0x47,
    ERROR = 0x48,
    NOTICE = 0x49,
    PARSE_COMPLETE = 0x4A,
    BIND_COMPLETE = 0x4B,
    CLOSE_COMPLETE = 0x4C,
    PORTAL_SUSPENDED = 0x4D,
    NO_DATA = 0x4E,
    PARAMETER_STATUS = 0x4F,
    PARAMETER_DESCRIPTION = 0x50,
    COPY_IN_RESPONSE = 0x51,
    COPY_OUT_RESPONSE = 0x52,
    COPY_BOTH_RESPONSE = 0x53,
    NOTIFICATION = 0x54,
    FUNCTION_RESULT = 0x55,
    NEGOTIATE_VERSION = 0x56,
    SBLR_COMPILED = 0x57,
    QUERY_PLAN = 0x58,
    STREAM_READY = 0x59,
    STREAM_DATA = 0x5A,
    STREAM_END = 0x5B,
    TXN_STATUS = 0x5C,
    PONG = 0x5D,
    CLUSTER_AUTH_OK = 0x5E,
    FEDERATED_RESULT = 0x5F,
    HEARTBEAT = 0x80,
    EXTENSION = 0x81
}

internal enum AuthMethod : byte
{
    OK = 0,
    PASSWORD = 1,
    MD5 = 2,
    SCRAM_SHA_256 = 3,
    SCRAM_SHA_512 = 4,
    TOKEN = 5,
    PEER = 6,
    REATTACH = 7,
    CERTIFICATE = 8,
    GSSAPI = 9,
    SSPI = 10,
    LDAP = 11,
    SAML = 12,
    OIDC = 13,
    MFA_TOTP = 14,
    CLUSTER_PKI = 15
}

internal static class ProtocolConstants
{
    public const uint Magic = 0x50574253; // "SBWP" (little-endian bytes)
    public const byte VersionMajor = 1;
    public const byte VersionMinor = 1;
    public const ushort Version = (ushort)((VersionMajor << 8) | VersionMinor);
    public const int HeaderSize = 40;
    public const int MaxMessageSize = 1024 * 1024 * 1024;

    public const byte MsgFlagCompressed = 0x01;
    public const byte MsgFlagContinued = 0x02;
    public const byte MsgFlagFinal = 0x04;
    public const byte MsgFlagUrgent = 0x08;
    public const byte MsgFlagEncrypted = 0x10;
    public const byte MsgFlagChecksum = 0x20;

    public const ulong FeatureCompression = 1UL << 0;
    public const ulong FeatureStreaming = 1UL << 1;
    public const ulong FeatureSblr = 1UL << 2;
    public const ulong FeatureFederation = 1UL << 3;
    public const ulong FeatureNotifications = 1UL << 4;
    public const ulong FeatureQueryPlan = 1UL << 5;
    public const ulong FeatureBatch = 1UL << 6;
    public const ulong FeaturePipeline = 1UL << 7;
    public const ulong FeatureBinaryCopy = 1UL << 8;
    public const ulong FeatureSavepoints = 1UL << 9;
    public const ulong Feature2Pc = 1UL << 10;
    public const ulong FeatureChecksums = 1UL << 11;

    public const uint QueryFlagDescribeOnly = 0x01;
    public const uint QueryFlagNoPortal = 0x02;
    public const uint QueryFlagBinaryResult = 0x04;
    public const uint QueryFlagIncludePlan = 0x08;
    public const uint QueryFlagReturnSblr = 0x10;
    public const uint QueryFlagNoCache = 0x20;

    public const byte IsolationReadUncommitted = 0;
    public const byte IsolationReadCommitted = 1;
    public const byte IsolationRepeatableRead = 2;
    public const byte IsolationSerializable = 3;

    public const byte ReadCommittedModeDefault = 0;
    public const byte ReadCommittedModeReadConsistency = 1;
    public const byte ReadCommittedModeRecordVersion = 2;
    public const byte ReadCommittedModeNoRecordVersion = 3;

    public const ushort TxnFlagHasIsolation = 0x0001;
    public const ushort TxnFlagHasAccess = 0x0002;
    public const ushort TxnFlagHasDeferrable = 0x0004;
    public const ushort TxnFlagHasWait = 0x0008;
    public const ushort TxnFlagHasTimeout = 0x0010;
    public const ushort TxnFlagHasAutocommit = 0x0020;
    public const ushort TxnFlagHasReadCommittedMode = 0x0100;

    public const byte StreamStart = 0;
    public const byte StreamPause = 1;
    public const byte StreamResume = 2;
    public const byte StreamCancel = 3;
    public const byte StreamAck = 4;

    public const byte SubscribeTypeChannel = 0;
    public const byte SubscribeTypeTable = 1;
    public const byte SubscribeTypeQuery = 2;
    public const byte SubscribeTypeEvent = 3;
}

internal sealed class MessageHeader
{
    public byte Type { get; }
    public byte Flags { get; }
    public uint Length { get; }
    public uint Sequence { get; }
    public byte[] AttachmentId { get; }
    public ulong TxnId { get; }

    public MessageHeader(byte type, byte flags, uint length, uint sequence, byte[] attachmentId, ulong txnId)
    {
        Type = type;
        Flags = flags;
        Length = length;
        Sequence = sequence;
        AttachmentId = attachmentId.Length == 16 ? attachmentId : throw new ArgumentException("AttachmentId must be 16 bytes");
        TxnId = txnId;
    }
}

internal sealed class ProtocolMessage
{
    public MessageHeader Header { get; }
    public byte[] Payload { get; }

    public ProtocolMessage(MessageHeader header, byte[] payload)
    {
        Header = header;
        Payload = payload;
    }

    public byte[] ToBytes()
    {
        var buffer = new byte[ProtocolConstants.HeaderSize + Payload.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(0, 4), ProtocolConstants.Magic);
        buffer[4] = ProtocolConstants.VersionMajor;
        buffer[5] = ProtocolConstants.VersionMinor;
        buffer[6] = Header.Type;
        buffer[7] = Header.Flags;
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(8, 4), (uint)Payload.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(12, 4), Header.Sequence);
        Buffer.BlockCopy(Header.AttachmentId, 0, buffer, 16, 16);
        BinaryPrimitives.WriteUInt64LittleEndian(buffer.AsSpan(32, 8), Header.TxnId);
        if (Payload.Length > 0)
        {
            Buffer.BlockCopy(Payload, 0, buffer, ProtocolConstants.HeaderSize, Payload.Length);
        }
        return buffer;
    }

    public static MessageHeader ParseHeader(ReadOnlySpan<byte> header)
    {
        if (header.Length != ProtocolConstants.HeaderSize)
        {
            throw new InvalidOperationException("Invalid header length");
        }
        var magic = BinaryPrimitives.ReadUInt32LittleEndian(header.Slice(0, 4));
        if (magic != ProtocolConstants.Magic)
        {
            throw new InvalidOperationException("Invalid protocol magic");
        }
        var major = header[4];
        var minor = header[5];
        if (major != ProtocolConstants.VersionMajor || minor != ProtocolConstants.VersionMinor)
        {
            throw new InvalidOperationException("Unsupported protocol version");
        }
        var type = header[6];
        var flags = header[7];
        var length = BinaryPrimitives.ReadUInt32LittleEndian(header.Slice(8, 4));
        if (length > ProtocolConstants.MaxMessageSize)
        {
            throw new InvalidOperationException("Payload too large");
        }
        var sequence = BinaryPrimitives.ReadUInt32LittleEndian(header.Slice(12, 4));
        var attachmentId = header.Slice(16, 16).ToArray();
        var txnId = BinaryPrimitives.ReadUInt64LittleEndian(header.Slice(32, 8));
        return new MessageHeader(type, flags, length, sequence, attachmentId, txnId);
    }
}

internal sealed class ColumnInfo
{
    public string Name { get; set; } = string.Empty;
    public uint TableOid { get; set; }
    public ushort ColumnIndex { get; set; }
    public uint TypeOid { get; set; }
    public short TypeSize { get; set; }
    public int TypeModifier { get; set; }
    public byte Format { get; set; }
    public bool Nullable { get; set; }
}

internal sealed class ColumnValue
{
    public byte[]? Data { get; set; }
}

internal sealed class ParamValue
{
    public ushort Format { get; set; }
    public byte[]? Data { get; set; }
    public bool IsNull { get; set; }
}

internal static class ProtocolCodec
{
    private const int ConnectValueText = 1;
    private const int P1RowDescriptionHeaderBytes = 72;
    private const int P1CanonicalTypeRefBytes = 144;

    public static byte[] BuildStartupPayload(ulong features, IReadOnlyDictionary<string, string> parameters)
    {
        using var paramStream = new MemoryStream();
        foreach (var kvp in parameters.OrderBy(kvp => kvp.Key, StringComparer.Ordinal))
        {
            WriteLengthPrefixedString(paramStream, kvp.Key);
            Span<byte> format = stackalloc byte[2];
            BinaryPrimitives.WriteUInt16LittleEndian(format, ConnectValueText);
            paramStream.Write(format);
            var value = Encoding.UTF8.GetBytes(kvp.Value);
            Span<byte> valueLength = stackalloc byte[4];
            BinaryPrimitives.WriteUInt32LittleEndian(valueLength, (uint)value.Length);
            paramStream.Write(valueLength);
            paramStream.Write(value);
        }

        var paramBytes = paramStream.ToArray();
        var payload = new byte[88 + paramBytes.Length];
        var offset = 0;
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(offset, 2), ProtocolConstants.Version);
        offset += 2;
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(offset, 2), ProtocolConstants.Version);
        offset += 2;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(offset, 4), 0);
        offset += 4;
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(offset, 8), features);
        offset += 8;
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(offset, 8), 0);
        offset += 8;
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(offset, 8), 0);
        offset += 8;
        offset += 16 * 3;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(offset, 4), (uint)parameters.Count);
        offset += 4;
        Buffer.BlockCopy(paramBytes, 0, payload, offset, paramBytes.Length);
        offset += paramBytes.Length;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(offset, 4), 0);
        return payload;
    }

    private static void WriteLengthPrefixedString(Stream stream, string value)
    {
        var encoded = Encoding.UTF8.GetBytes(value);
        Span<byte> length = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(length, (uint)encoded.Length);
        stream.Write(length);
        stream.Write(encoded);
    }

    public static (AuthMethod Method, byte[] Data) ParseAuthRequest(byte[] payload)
    {
        if (payload.Length < 4)
        {
            throw new InvalidOperationException("Auth request truncated");
        }
        var method = (AuthMethod)payload[0];
        var data = payload.AsSpan(4).ToArray();
        return (method, data);
    }

    public static (AuthMethod Method, byte Stage, byte[] Data) ParseAuthContinue(byte[] payload)
    {
        if (payload.Length < 8)
        {
            throw new InvalidOperationException("Auth continue truncated");
        }
        var method = (AuthMethod)payload[0];
        var stage = payload[1];
        var dataLen = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(4, 4));
        if (8 + dataLen > payload.Length)
        {
            throw new InvalidOperationException("Auth continue truncated");
        }
        var data = payload.AsSpan(8, (int)dataLen).ToArray();
        return (method, stage, data);
    }

    public static (byte[] SessionId, byte[] ServerInfo) ParseAuthOk(byte[] payload)
    {
        if (payload.Length < 20)
        {
            throw new InvalidOperationException("Auth ok truncated");
        }
        var sessionId = payload.AsSpan(0, 16).ToArray();
        var infoLen = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(16, 4));
        if (20 + infoLen > payload.Length)
        {
            throw new InvalidOperationException("Auth ok truncated");
        }
        var serverInfo = payload.AsSpan(20, (int)infoLen).ToArray();
        return (sessionId, serverInfo);
    }

    public static byte[] BuildQueryPayload(string sql, uint flags, uint maxRows, uint timeoutMs)
    {
        var sqlBytes = Encoding.UTF8.GetBytes(sql + "\0");
        var payload = new byte[12 + sqlBytes.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), flags);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), maxRows);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(8, 4), timeoutMs);
        Buffer.BlockCopy(sqlBytes, 0, payload, 12, sqlBytes.Length);
        return payload;
    }

    public static byte[] BuildParsePayload(string statementName, string sql, IReadOnlyList<uint> paramTypes)
    {
        var nameBytes = Encoding.UTF8.GetBytes(statementName);
        var sqlBytes = Encoding.UTF8.GetBytes(sql);
        var payloadLen = 4 + nameBytes.Length + 4 + sqlBytes.Length + 2 + 2 + paramTypes.Count * 4;
        var payload = new byte[payloadLen];
        var offset = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(offset, 4), (uint)nameBytes.Length);
        offset += 4;
        Buffer.BlockCopy(nameBytes, 0, payload, offset, nameBytes.Length);
        offset += nameBytes.Length;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(offset, 4), (uint)sqlBytes.Length);
        offset += 4;
        Buffer.BlockCopy(sqlBytes, 0, payload, offset, sqlBytes.Length);
        offset += sqlBytes.Length;
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(offset, 2), (ushort)paramTypes.Count);
        offset += 2;
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(offset, 2), 0);
        offset += 2;
        foreach (var oid in paramTypes)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(offset, 4), oid);
            offset += 4;
        }
        return payload;
    }

    public static byte[] BuildBindPayload(string portalName, string statementName, IReadOnlyList<ParamValue> parameters, IReadOnlyList<ushort> resultFormats)
    {
        var portalBytes = Encoding.UTF8.GetBytes(portalName);
        var stmtBytes = Encoding.UTF8.GetBytes(statementName);
        var paramFormats = parameters.Select(param => param.Format).ToArray();

        var payloadLen = 4 + portalBytes.Length + 4 + stmtBytes.Length;
        payloadLen += 2 + paramFormats.Length * 2;
        payloadLen += 2 + 2;
        foreach (var param in parameters)
        {
            payloadLen += 4;
            if (!param.IsNull && param.Data != null)
            {
                payloadLen += param.Data.Length;
            }
        }
        payloadLen += 2 + resultFormats.Count * 2;

        var payload = new byte[payloadLen];
        var offset = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(offset, 4), (uint)portalBytes.Length);
        offset += 4;
        Buffer.BlockCopy(portalBytes, 0, payload, offset, portalBytes.Length);
        offset += portalBytes.Length;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(offset, 4), (uint)stmtBytes.Length);
        offset += 4;
        Buffer.BlockCopy(stmtBytes, 0, payload, offset, stmtBytes.Length);
        offset += stmtBytes.Length;
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(offset, 2), (ushort)paramFormats.Length);
        offset += 2;
        foreach (var fmt in paramFormats)
        {
            BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(offset, 2), fmt);
            offset += 2;
        }
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(offset, 2), (ushort)parameters.Count);
        offset += 2;
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(offset, 2), 0);
        offset += 2;
        foreach (var param in parameters)
        {
            if (param.IsNull)
            {
                BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(offset, 4), -1);
                offset += 4;
                continue;
            }
            var data = param.Data ?? Array.Empty<byte>();
            BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(offset, 4), data.Length);
            offset += 4;
            Buffer.BlockCopy(data, 0, payload, offset, data.Length);
            offset += data.Length;
        }
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(offset, 2), (ushort)resultFormats.Count);
        offset += 2;
        foreach (var fmt in resultFormats)
        {
            BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(offset, 2), fmt);
            offset += 2;
        }
        return payload;
    }

    public static byte[] BuildDescribePayload(byte describeType, string name)
    {
        var nameBytes = Encoding.UTF8.GetBytes(name);
        var payload = new byte[8 + nameBytes.Length];
        payload[0] = describeType;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), (uint)nameBytes.Length);
        Buffer.BlockCopy(nameBytes, 0, payload, 8, nameBytes.Length);
        return payload;
    }

    public static byte[] BuildExecutePayload(string portalName, uint maxRows)
    {
        var portalBytes = Encoding.UTF8.GetBytes(portalName);
        var payload = new byte[4 + portalBytes.Length + 4];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), (uint)portalBytes.Length);
        Buffer.BlockCopy(portalBytes, 0, payload, 4, portalBytes.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4 + portalBytes.Length, 4), maxRows);
        return payload;
    }

    public static byte[] BuildClosePayload(byte closeType, string name)
    {
        var nameBytes = Encoding.UTF8.GetBytes(name);
        var payload = new byte[8 + nameBytes.Length];
        payload[0] = closeType;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), (uint)nameBytes.Length);
        Buffer.BlockCopy(nameBytes, 0, payload, 8, nameBytes.Length);
        return payload;
    }

    public static byte[] BuildCancelPayload(uint cancelType, uint targetSequence)
    {
        var payload = new byte[8];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), cancelType);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), targetSequence);
        return payload;
    }

    public static byte[] BuildSblrExecutePayload(ulong sblrHash, byte[]? sblrBytecode, IReadOnlyList<ParamValue> parameters)
    {
        var bytecode = sblrBytecode ?? Array.Empty<byte>();
        var paramBytes = BuildParamValues(parameters);
        var payload = new byte[8 + 4 + 2 + 2 + bytecode.Length + paramBytes.Length];
        var offset = 0;
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(offset, 8), sblrHash);
        offset += 8;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(offset, 4), (uint)bytecode.Length);
        offset += 4;
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(offset, 2), (ushort)parameters.Count);
        offset += 2;
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(offset, 2), 0);
        offset += 2;
        if (bytecode.Length > 0)
        {
            Buffer.BlockCopy(bytecode, 0, payload, offset, bytecode.Length);
            offset += bytecode.Length;
        }
        Buffer.BlockCopy(paramBytes, 0, payload, offset, paramBytes.Length);
        return payload;
    }

    public static byte[] BuildSubscribePayload(byte subscribeType, string channel, string filterExpr)
    {
        var channelBytes = Encoding.UTF8.GetBytes(channel);
        var filterBytes = Encoding.UTF8.GetBytes(filterExpr ?? string.Empty);
        var payload = new byte[4 + 4 + channelBytes.Length + 4 + filterBytes.Length];
        payload[0] = subscribeType;
        var offset = 4;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(offset, 4), (uint)channelBytes.Length);
        offset += 4;
        Buffer.BlockCopy(channelBytes, 0, payload, offset, channelBytes.Length);
        offset += channelBytes.Length;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(offset, 4), (uint)filterBytes.Length);
        offset += 4;
        Buffer.BlockCopy(filterBytes, 0, payload, offset, filterBytes.Length);
        return payload;
    }

    public static byte[] BuildUnsubscribePayload(string channel)
    {
        var channelBytes = Encoding.UTF8.GetBytes(channel);
        var payload = new byte[4 + channelBytes.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), (uint)channelBytes.Length);
        Buffer.BlockCopy(channelBytes, 0, payload, 4, channelBytes.Length);
        return payload;
    }

    public static byte[] BuildTxnBeginPayload(
        ushort flags,
        byte conflictAction,
        byte autocommitMode,
        byte isolationLevel,
        byte accessMode,
        byte deferrable,
        byte waitMode,
        uint timeoutMs,
        byte readCommittedMode = ProtocolConstants.ReadCommittedModeDefault
    )
    {
        var payload = new byte[(flags & ProtocolConstants.TxnFlagHasReadCommittedMode) != 0 ? 16 : 12];
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(0, 2), flags);
        payload[2] = conflictAction;
        payload[3] = autocommitMode;
        payload[4] = isolationLevel;
        payload[5] = accessMode;
        payload[6] = deferrable;
        payload[7] = waitMode;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(8, 4), timeoutMs);
        if ((flags & ProtocolConstants.TxnFlagHasReadCommittedMode) != 0)
        {
            payload[12] = readCommittedMode;
        }
        return payload;
    }

    public static byte[] BuildTxnCommitPayload(byte flags)
    {
        return new[] { flags, (byte)0, (byte)0, (byte)0 };
    }

    public static byte[] BuildTxnRollbackPayload(byte flags)
    {
        return new[] { flags, (byte)0, (byte)0, (byte)0 };
    }

    public static byte[] BuildTxnSavepointPayload(string name)
    {
        var nameBytes = Encoding.UTF8.GetBytes(name);
        var payload = new byte[4 + nameBytes.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), (uint)nameBytes.Length);
        Buffer.BlockCopy(nameBytes, 0, payload, 4, nameBytes.Length);
        return payload;
    }

    public static byte[] BuildTxnReleasePayload(string name) => BuildTxnSavepointPayload(name);

    public static byte[] BuildTxnRollbackToPayload(string name) => BuildTxnSavepointPayload(name);

    public static byte[] BuildSetOptionPayload(string name, string value)
    {
        var nameBytes = Encoding.UTF8.GetBytes(name);
        var valueBytes = Encoding.UTF8.GetBytes(value);
        var payload = new byte[8 + nameBytes.Length + valueBytes.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), (uint)nameBytes.Length);
        Buffer.BlockCopy(nameBytes, 0, payload, 4, nameBytes.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4 + nameBytes.Length, 4), (uint)valueBytes.Length);
        Buffer.BlockCopy(valueBytes, 0, payload, 8 + nameBytes.Length, valueBytes.Length);
        return payload;
    }

    public static byte[] BuildStreamControlPayload(byte controlType, uint windowSize, uint timeoutMs)
    {
        var payload = new byte[12];
        payload[0] = controlType;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), windowSize);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(8, 4), timeoutMs);
        return payload;
    }

    public static byte[] BuildAttachCreatePayload(string emulationMode, string dbName)
    {
        var modeBytes = Encoding.UTF8.GetBytes(emulationMode);
        var dbBytes = Encoding.UTF8.GetBytes(dbName);
        var payload = new byte[8 + modeBytes.Length + dbBytes.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), (uint)modeBytes.Length);
        Buffer.BlockCopy(modeBytes, 0, payload, 4, modeBytes.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4 + modeBytes.Length, 4), (uint)dbBytes.Length);
        Buffer.BlockCopy(dbBytes, 0, payload, 8 + modeBytes.Length, dbBytes.Length);
        return payload;
    }

    private static byte[] BuildParamValues(IReadOnlyList<ParamValue> parameters)
    {
        var len = 0;
        foreach (var param in parameters)
        {
            len += 4;
            if (!param.IsNull && param.Data.Length > 0)
            {
                len += param.Data.Length;
            }
        }
        var payload = new byte[len];
        var offset = 0;
        foreach (var param in parameters)
        {
            if (param.IsNull)
            {
                BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(offset, 4), -1);
                offset += 4;
                continue;
            }
            BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(offset, 4), param.Data.Length);
            offset += 4;
            if (param.Data.Length > 0)
            {
                Buffer.BlockCopy(param.Data, 0, payload, offset, param.Data.Length);
                offset += param.Data.Length;
            }
        }
        return payload;
    }

    public static (byte Status, ulong TxnId, ulong Visibility) ParseReady(byte[] payload)
    {
        if (payload.Length >= 76
            && (payload[56] == (byte)'I'
                || payload[56] == (byte)'T'
                || payload[56] == (byte)'E'
                || payload[56] == (byte)'R'
                || payload[56] == (byte)'A'))
        {
            var p1TxnId = BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(48, 8));
            var p1Status = payload[56] == (byte)'T' || payload[56] == (byte)'E'
                ? (byte)1
                : (byte)0;
            return (p1Status, p1TxnId, p1TxnId);
        }
        if (payload.Length < 20)
        {
            throw new InvalidOperationException("Ready truncated");
        }
        var status = payload[0];
        var txnId = BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(4, 8));
        var visibility = BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(12, 8));
        return (status, txnId, visibility);
    }

    public static (char Status, ulong TxnId) ParseTxnStatus(byte[] payload)
    {
        if (payload.Length < 12)
        {
            throw new InvalidOperationException("Txn status truncated");
        }

        var status = (char)payload[0];
        var txnId = BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(4, 8));
        return (status, txnId);
    }

    public static (string Name, string Value) ParseParameterStatus(byte[] payload)
    {
        var statuses = ParseParameterStatuses(payload);
        if (statuses.Count == 0)
        {
            throw new InvalidOperationException("Parameter status truncated");
        }
        return statuses[0];
    }

    public static List<(string Name, string Value)> ParseParameterStatuses(byte[] payload)
    {
        if (payload.Length < 8)
        {
            throw new InvalidOperationException("Parameter status truncated");
        }

        var count = BinaryPrimitives.ReadInt32LittleEndian(payload.AsSpan(0, 4));
        if (count > 0 && count <= 256)
        {
            try
            {
                var p1Offset = 4;
                var statuses = new List<(string Name, string Value)>(count);
                for (var index = 0; index < count; index++)
                {
                    if (p1Offset + 4 > payload.Length)
                    {
                        throw new InvalidOperationException("Parameter status truncated");
                    }
                    var p1NameLen = BinaryPrimitives.ReadInt32LittleEndian(payload.AsSpan(p1Offset, 4));
                    p1Offset += 4;
                    if (p1NameLen < 0 || p1Offset + p1NameLen + 7 > payload.Length)
                    {
                        throw new InvalidOperationException("Parameter status truncated");
                    }
                    var p1Name = Encoding.UTF8.GetString(payload, p1Offset, p1NameLen);
                    p1Offset += p1NameLen;
                    p1Offset += 3;
                    var p1ValueLen = BinaryPrimitives.ReadInt32LittleEndian(payload.AsSpan(p1Offset, 4));
                    p1Offset += 4;
                    if (p1ValueLen < 0 || p1Offset + p1ValueLen > payload.Length)
                    {
                        throw new InvalidOperationException("Parameter status truncated");
                    }
                    var p1Value = Encoding.UTF8.GetString(payload, p1Offset, p1ValueLen);
                    p1Offset += p1ValueLen;
                    statuses.Add((p1Name, p1Value));
                }
                if (p1Offset == payload.Length)
                {
                    return statuses;
                }
            }
            catch (InvalidOperationException)
            {
                // Fall through to the legacy single key/value payload shape.
            }
        }

        var offset = 0;
        var nameLen = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(offset, 4));
        offset += 4;
        if (nameLen > payload.Length - offset - 4)
        {
            throw new InvalidOperationException("Parameter status truncated");
        }
        var name = Encoding.UTF8.GetString(payload, offset, (int)nameLen);
        offset += (int)nameLen;
        var valueLen = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(offset, 4));
        offset += 4;
        if (valueLen > payload.Length - offset)
        {
            throw new InvalidOperationException("Parameter status truncated");
        }
        var value = Encoding.UTF8.GetString(payload, offset, (int)valueLen);
        return new List<(string Name, string Value)> { (name, value) };
    }

    public static List<uint> ParseParameterDescription(byte[] payload)
    {
        if (payload.Length >= P1RowDescriptionHeaderBytes
            && BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(0, 2)) == 1
            && payload[3] == 1)
        {
            var p1Count = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(68, 4));
            var p1Types = new List<uint>((int)p1Count);
            var p1Offset = P1RowDescriptionHeaderBytes;
            for (var i = 0; i < p1Count; i++)
            {
                if (p1Offset + 4 + 4 + 8 + 8 + P1CanonicalTypeRefBytes + 4 + 5 > payload.Length)
                {
                    throw new InvalidOperationException("P1 parameter description truncated");
                }
                var typeOffset = p1Offset + 4 + 4 + 8 + 8;
                p1Types.Add(OidFromCanonicalTypeRef(payload, typeOffset));
                p1Offset = typeOffset + P1CanonicalTypeRefBytes + 4;
                _ = ReadNullableText(payload, ref p1Offset);
            }
            return p1Types;
        }
        if (payload.Length < 4)
        {
            throw new InvalidOperationException("Parameter description truncated");
        }
        var count = BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(0, 2));
        var offset = 4;
        var types = new List<uint>(count);
        for (var i = 0; i < count; i++)
        {
            if (offset + 4 > payload.Length)
            {
                throw new InvalidOperationException("Parameter description truncated");
            }
            types.Add(BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(offset, 4)));
            offset += 4;
        }
        return types;
    }

    public static List<ColumnInfo> ParseRowDescription(byte[] payload)
    {
        if (IsP1RowDescription(payload))
        {
            return ParseP1RowDescription(payload);
        }
        if (payload.Length < 4)
        {
            throw new InvalidOperationException("Row description truncated");
        }
        var offset = 0;
        var count = BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(offset, 2));
        offset += 4;
        var cols = new List<ColumnInfo>(count);
        for (var i = 0; i < count; i++)
        {
            var nameLen = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(offset, 4));
            offset += 4;
            var name = Encoding.UTF8.GetString(payload, offset, (int)nameLen);
            offset += (int)nameLen;
            var tableOid = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(offset, 4));
            offset += 4;
            var columnIndex = BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(offset, 2));
            offset += 2;
            var typeOid = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(offset, 4));
            offset += 4;
            var typeSize = BinaryPrimitives.ReadInt16LittleEndian(payload.AsSpan(offset, 2));
            offset += 2;
            var typeModifier = BinaryPrimitives.ReadInt32LittleEndian(payload.AsSpan(offset, 4));
            offset += 4;
            var format = payload[offset];
            offset += 1;
            var nullable = payload[offset] == 1;
            offset += 1;
            offset += 2;
            cols.Add(new ColumnInfo
            {
                Name = name,
                TableOid = tableOid,
                ColumnIndex = columnIndex,
                TypeOid = typeOid,
                TypeSize = typeSize,
                TypeModifier = typeModifier,
                Format = format,
                Nullable = nullable
            });
        }
        return cols;
    }

    private static bool IsP1RowDescription(byte[] payload)
    {
        return payload.Length >= P1RowDescriptionHeaderBytes
            && BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(0, 2)) == 1
            && payload[3] == 1;
    }

    private static List<ColumnInfo> ParseP1RowDescription(byte[] payload)
    {
        var count = BinaryPrimitives.ReadInt32LittleEndian(payload.AsSpan(4, 4));
        if (count < 0)
        {
            throw new InvalidOperationException("P1 row description column count invalid");
        }

        var offset = P1RowDescriptionHeaderBytes;
        var cols = new List<ColumnInfo>(count);
        for (var i = 0; i < count; i++)
        {
            var fixedColumnBytes = 4 + 4 + 8 + P1CanonicalTypeRefBytes + 56;
            if (offset + fixedColumnBytes > payload.Length)
            {
                throw new InvalidOperationException("P1 row description truncated");
            }

            var ordinal = BinaryPrimitives.ReadInt32LittleEndian(payload.AsSpan(offset, 4));
            offset += 4;
            offset += 1;
            var format = payload[offset++] == 1
                ? (byte)TypeDecoder.FormatText
                : (byte)TypeDecoder.FormatBinary;
            var nullable = payload[offset++] == 1;
            offset += 1;
            offset += 8;
            var typeOid = OidFromCanonicalTypeRef(payload, offset);
            offset += P1CanonicalTypeRefBytes;
            offset += 16 * 3;
            offset += 4;
            offset += 2;
            offset += 2;

            var name = ReadNullableText(payload, ref offset);
            cols.Add(new ColumnInfo
            {
                Name = string.IsNullOrEmpty(name) ? $"column{i + 1}" : name,
                TableOid = 0,
                ColumnIndex = (ushort)(ordinal == 0 ? i : ordinal - 1),
                TypeOid = typeOid,
                TypeSize = TypeSizeForOid(typeOid),
                TypeModifier = -1,
                Format = format,
                Nullable = nullable
            });
        }
        return cols;
    }

    private static uint OidFromCanonicalTypeRef(byte[] payload, int offset)
    {
        if (offset + 4 > payload.Length)
        {
            return TypeDecoder.OidText;
        }
        var family = BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(offset, 2));
        var code = BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(offset + 2, 2));
        return (family, code) switch
        {
            (1, 1) => TypeDecoder.OidBool,
            (2, 3) => TypeDecoder.OidInt4,
            (2, 4) => TypeDecoder.OidInt8,
            (4, 1) => TypeDecoder.OidNumeric,
            (6, 2) => TypeDecoder.OidFloat8,
            (8, 1) => TypeDecoder.OidText,
            _ => TypeDecoder.OidText
        };
    }

    private static short TypeSizeForOid(uint oid)
    {
        return oid switch
        {
            TypeDecoder.OidBool => 1,
            TypeDecoder.OidInt4 => 4,
            TypeDecoder.OidInt8 => 8,
            TypeDecoder.OidFloat8 => 8,
            _ => -1
        };
    }

    private static string ReadNullableText(byte[] payload, ref int offset)
    {
        if (offset + 5 > payload.Length)
        {
            throw new InvalidOperationException("Nullable text truncated");
        }
        var tag = payload[offset++];
        var length = BinaryPrimitives.ReadInt32LittleEndian(payload.AsSpan(offset, 4));
        offset += 4;
        if (length < 0)
        {
            throw new InvalidOperationException("Nullable text length invalid");
        }
        if (tag == 0)
        {
            return string.Empty;
        }
        if (offset + length > payload.Length)
        {
            throw new InvalidOperationException("Nullable text truncated");
        }
        var value = Encoding.UTF8.GetString(payload, offset, length);
        offset += length;
        return value;
    }

    public static List<ColumnValue> ParseDataRow(byte[] payload)
    {
        if (payload.Length < 4)
        {
            throw new InvalidOperationException("Row data truncated");
        }
        var offset = 0;
        var count = BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(offset, 2));
        offset += 2;
        var nullBytes = BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(offset, 2));
        offset += 2;
        var nullBitmap = payload.AsSpan(offset, nullBytes).ToArray();
        offset += nullBytes;
        var values = new List<ColumnValue>(count);
        for (var i = 0; i < count; i++)
        {
            var byteIndex = i / 8;
            var bitIndex = i % 8;
            var isNull = byteIndex < nullBitmap.Length && (nullBitmap[byteIndex] & (1 << bitIndex)) != 0;
            if (isNull)
            {
                values.Add(new ColumnValue { Data = null });
                continue;
            }
            var length = BinaryPrimitives.ReadInt32LittleEndian(payload.AsSpan(offset, 4));
            offset += 4;
            if (length < 0)
            {
                values.Add(new ColumnValue { Data = null });
                continue;
            }
            var data = payload.AsSpan(offset, length).ToArray();
            offset += length;
            values.Add(new ColumnValue { Data = data });
        }
        return values;
    }

    public static (byte CommandType, ulong Rows, ulong LastId, string Tag) ParseCommandComplete(byte[] payload)
    {
        if (payload.Length < 20)
        {
            throw new InvalidOperationException("Command complete truncated");
        }
        var commandType = payload[0];
        var rows = BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(4, 8));
        var lastId = BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(12, 8));
        var tagBytes = payload.AsSpan(20);
        var nullIdx = tagBytes.IndexOf((byte)0);
        var tag = Encoding.UTF8.GetString(nullIdx >= 0 ? tagBytes.Slice(0, nullIdx) : tagBytes);
        return (commandType, rows, lastId, tag);
    }

    public static (uint ProcessId, string Channel, byte[] Payload, char? ChangeType, ulong? RowId) ParseNotification(byte[] payload)
    {
        if (payload.Length < 12)
        {
            throw new InvalidOperationException("Notification truncated");
        }
        var offset = 0;
        var processId = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(offset, 4));
        offset += 4;
        var channelLen = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(offset, 4));
        offset += 4;
        if (offset + channelLen + 4 > payload.Length)
        {
            throw new InvalidOperationException("Notification truncated");
        }
        var channel = Encoding.UTF8.GetString(payload, offset, (int)channelLen);
        offset += (int)channelLen;
        var payloadLen = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(offset, 4));
        offset += 4;
        if (offset + payloadLen > payload.Length)
        {
            throw new InvalidOperationException("Notification truncated");
        }
        var data = payload.AsSpan(offset, (int)payloadLen).ToArray();
        offset += (int)payloadLen;
        char? changeType = null;
        ulong? rowId = null;
        if (offset < payload.Length)
        {
            changeType = (char)payload[offset];
            offset += 1;
            if (offset + 8 <= payload.Length)
            {
                rowId = BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(offset, 8));
            }
        }
        return (processId, channel, data, changeType, rowId);
    }

    public static (uint Format, ulong PlanningTimeUs, ulong EstimatedRows, ulong EstimatedCost, byte[] Plan) ParseQueryPlan(byte[] payload)
    {
        if (payload.Length < 32)
        {
            throw new InvalidOperationException("Query plan truncated");
        }
        var format = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(0, 4));
        var planLength = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(4, 4));
        var planning = BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(8, 8));
        var estimatedRows = BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(16, 8));
        var estimatedCost = BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(24, 8));
        if (32 + planLength > payload.Length)
        {
            throw new InvalidOperationException("Query plan truncated");
        }
        var plan = payload.AsSpan(32, (int)planLength).ToArray();
        return (format, planning, estimatedRows, estimatedCost, plan);
    }

    public static (ulong Hash, uint Version, byte[] Bytecode) ParseSblrCompiled(byte[] payload)
    {
        if (payload.Length < 16)
        {
            throw new InvalidOperationException("SBLR compiled truncated");
        }
        var hash = BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(0, 8));
        var version = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(8, 4));
        var length = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(12, 4));
        if (16 + length > payload.Length)
        {
            throw new InvalidOperationException("SBLR compiled truncated");
        }
        var bytecode = payload.AsSpan(16, (int)length).ToArray();
        return (hash, version, bytecode);
    }

    public static (string Severity, string SqlState, string Message, string Detail, string Hint) ParseErrorMessage(byte[] payload)
    {
        var offset = 0;
        var severity = string.Empty;
        var sqlState = string.Empty;
        var message = string.Empty;
        var detail = string.Empty;
        var hint = string.Empty;

        while (offset < payload.Length)
        {
            var field = payload[offset];
            offset += 1;
            if (field == 0)
            {
                break;
            }
            var start = offset;
            while (offset < payload.Length && payload[offset] != 0)
            {
                offset += 1;
            }
            if (offset >= payload.Length)
            {
                break;
            }
            var value = Encoding.UTF8.GetString(payload, start, offset - start);
            offset += 1;
            switch ((char)field)
            {
                case 'S':
                    severity = value;
                    break;
                case 'C':
                    sqlState = value;
                    break;
                case 'M':
                    message = value;
                    break;
                case 'D':
                    detail = value;
                    break;
                case 'H':
                    hint = value;
                    break;
            }
        }

        return (severity, sqlState, message, detail, hint);
    }
}
