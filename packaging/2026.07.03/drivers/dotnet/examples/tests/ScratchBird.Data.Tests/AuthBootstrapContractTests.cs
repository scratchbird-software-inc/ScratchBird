// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Buffers.Binary;
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading.Tasks;
using ScratchBird.Data;
using Xunit;

namespace ScratchBird.Data.Tests;

public sealed class AuthBootstrapContractTests
{
    [Fact]
    public void ProbeAuthSurfaceDirectReportsScramSha512()
    {
        using var harness = LoopbackHarness.Start((stream, capture) =>
        {
            ReceiveProtocolMessage(stream);
            SendProtocolMessage(stream, BuildAuthRequestMessage(1, AuthMethod.SCRAM_SHA_512));
        });

        var result = ScratchBirdConnection.ProbeAuthSurface(
            $"Host=127.0.0.1;Port={harness.Port};Database=main;SSLMode=disable;AllowInsecure=true");

        Assert.True(result.Reachable);
        Assert.Equal("direct", result.FrontDoorMode);
        Assert.Equal((byte)AuthMethod.SCRAM_SHA_512, result.RequiredMethodCode);
        Assert.Equal("SCRAM_SHA_512", result.RequiredMethodName);
        Assert.Equal("scratchbird.auth.scram", result.RequiredAuthPluginId);
        Assert.Single(result.AdmittedMethods);
        Assert.True(result.AdmittedMethods[0].ExecutableLocally);
    }

    [Fact]
    public void ProbeAuthSurfaceManagerProxyReportsToken()
    {
        using var harness = LoopbackHarness.Start((stream, capture) =>
        {
            var hello = ReceiveManagerFrame(stream);
            Assert.Equal(0x65, hello.Type);
            SendManagerFrame(stream, 0x64, Array.Empty<byte>());

            var authStart = ReceiveManagerFrame(stream);
            Assert.Equal(0x66, authStart.Type);
            AssertManagerAuthStartPayload(authStart.Payload, expectInlineToken: false);

            SendManagerFrame(stream, 0x12, Array.Empty<byte>());
        });

        var result = ScratchBirdConnection.ProbeAuthSurface(
            $"Host=127.0.0.1;Port={harness.Port};Database=main;Username=admin;SSLMode=disable;AllowInsecure=true;Front_Door_Mode=manager_proxy");

        Assert.True(result.Reachable);
        Assert.Equal("manager_proxy", result.FrontDoorMode);
        Assert.Equal((byte)AuthMethod.TOKEN, result.RequiredMethodCode);
        Assert.Equal("TOKEN", result.RequiredMethodName);
        Assert.Equal("scratchbird.auth.token_authkey", result.RequiredAuthPluginId);
        Assert.Single(result.AdmittedMethods);
        Assert.True(result.AdmittedMethods[0].ExecutableLocally);
    }

    [Fact]
    public void HandshakeSupportsScramSha512AndResolvedContext()
    {
        using var harness = LoopbackHarness.Start((stream, capture) =>
        {
            ReceiveProtocolMessage(stream);
            SendProtocolMessage(stream, BuildAuthRequestMessage(1, AuthMethod.SCRAM_SHA_512));

            var clientFirst = Encoding.UTF8.GetString(ReceiveProtocolMessage(stream).Payload);
            var nonce = ExtractScramNonce(clientFirst);
            var serverFirst = $"r={nonce}server,s=QSXCR+Q6sek8bf92,i=4096";
            SendProtocolMessage(stream, BuildAuthContinueMessage(2, AuthMethod.SCRAM_SHA_512, 0, serverFirst));

            var clientFinal = Encoding.UTF8.GetString(ReceiveProtocolMessage(stream).Payload);
            Assert.Contains("c=biws", clientFinal, StringComparison.Ordinal);
            Assert.Contains("p=", clientFinal, StringComparison.Ordinal);

            SendProtocolMessage(stream, BuildAuthOkMessage(3));
            SendProtocolMessage(stream, BuildReadyMessage(4, 0));
        });

        using var connection = new ScratchBirdConnection(
            $"Host=127.0.0.1;Port={harness.Port};Database=main;Username=sb_admin;Password=SbAdmin_Compat1!;SSLMode=disable;AllowInsecure=true;Pooling=false");
        connection.Open();

        var resolved = connection.GetResolvedAuthContext();
        Assert.True(resolved.Attached);
        Assert.False(resolved.ManagerAuthenticated);
        Assert.Equal("direct", resolved.FrontDoorMode);
        Assert.Equal((byte)AuthMethod.SCRAM_SHA_512, resolved.ResolvedMethodCode);
        Assert.Equal("SCRAM_SHA_512", resolved.ResolvedMethodName);
        Assert.Equal("scratchbird.auth.scram", resolved.ResolvedAuthPluginId);
    }

    [Fact]
    public void HandshakeSupportsTokenAuth()
    {
        const string expectedToken = "oidc-bearer-token";

        using var harness = LoopbackHarness.Start((stream, captured) =>
        {
            ReceiveProtocolMessage(stream);
            SendProtocolMessage(stream, BuildAuthRequestMessage(1, AuthMethod.TOKEN));

            captured.TokenPayload = Encoding.UTF8.GetString(ReceiveProtocolMessage(stream).Payload);
            SendProtocolMessage(stream, BuildAuthOkMessage(2));
            SendProtocolMessage(stream, BuildReadyMessage(3, 0));
        });

        using var connection = new ScratchBirdConnection(
            $"Host=127.0.0.1;Port={harness.Port};Database=main;Username=sb_admin;Password=unused;SSLMode=disable;AllowInsecure=true;Pooling=false;Auth_Token={expectedToken}");
        connection.Open();

        Assert.Equal(expectedToken, harness.Captured.TokenPayload);
        var resolved = connection.GetResolvedAuthContext();
        Assert.Equal((byte)AuthMethod.TOKEN, resolved.ResolvedMethodCode);
        Assert.Equal("TOKEN", resolved.ResolvedMethodName);
        Assert.Equal("scratchbird.auth.token_authkey", resolved.ResolvedAuthPluginId);
    }

    [Fact]
    public void HandshakePeerFailsClosed()
    {
        using var harness = LoopbackHarness.Start((stream, capture) =>
        {
            ReceiveProtocolMessage(stream);
            SendProtocolMessage(stream, BuildAuthRequestMessage(1, AuthMethod.PEER));
        });

        var client = new ProtocolClient();
        var config = ScratchBirdConfig.FromConnectionString(
            $"Host=127.0.0.1;Port={harness.Port};Database=main;Username=sb_admin;Password=unused;SSLMode=disable;AllowInsecure=true;Pooling=false");

        var ex = Assert.Throws<ScratchBirdNotSupportedException>(() => client.Connect(config));
        Assert.Equal("0A000", ex.SqlState);
        Assert.Contains("PEER", ex.Message, StringComparison.Ordinal);
    }

    private static ProtocolMessage ReceiveProtocolMessage(NetworkStream stream)
    {
        var headerBytes = ReadExact(stream, ProtocolConstants.HeaderSize);
        var header = ProtocolMessage.ParseHeader(headerBytes);
        var payload = header.Length > 0 ? ReadExact(stream, (int)header.Length) : Array.Empty<byte>();
        return new ProtocolMessage(header, payload);
    }

    private static void SendProtocolMessage(NetworkStream stream, ProtocolMessage message)
    {
        var bytes = message.ToBytes();
        stream.Write(bytes, 0, bytes.Length);
        stream.Flush();
    }

    private static ProtocolMessage BuildAuthRequestMessage(uint sequence, AuthMethod method)
    {
        var payload = new byte[4];
        payload[0] = (byte)method;
        return new ProtocolMessage(
            new MessageHeader((byte)MessageType.AUTH_REQUEST, 0, (uint)payload.Length, sequence, new byte[16], 0),
            payload);
    }

    private static ProtocolMessage BuildAuthContinueMessage(uint sequence, AuthMethod method, byte stage, string data)
    {
        var dataBytes = Encoding.UTF8.GetBytes(data);
        var payload = new byte[8 + dataBytes.Length];
        payload[0] = (byte)method;
        payload[1] = stage;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), (uint)dataBytes.Length);
        Buffer.BlockCopy(dataBytes, 0, payload, 8, dataBytes.Length);
        return new ProtocolMessage(
            new MessageHeader((byte)MessageType.AUTH_CONTINUE, 0, (uint)payload.Length, sequence, new byte[16], 0),
            payload);
    }

    private static ProtocolMessage BuildAuthOkMessage(uint sequence)
    {
        var payload = new byte[20];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(16, 4), 0);
        return new ProtocolMessage(
            new MessageHeader((byte)MessageType.AUTH_OK, 0, (uint)payload.Length, sequence, new byte[16], 0),
            payload);
    }

    private static ProtocolMessage BuildReadyMessage(uint sequence, ulong txnId)
    {
        var payload = new byte[20];
        payload[0] = 0;
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(4, 8), txnId);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(12, 8), txnId);
        return new ProtocolMessage(
            new MessageHeader((byte)MessageType.READY, 0, (uint)payload.Length, sequence, new byte[16], txnId),
            payload);
    }

    private static byte[] ReadExact(NetworkStream stream, int length)
    {
        var buffer = new byte[length];
        var offset = 0;
        while (offset < length)
        {
            var read = stream.Read(buffer, offset, length - offset);
            if (read <= 0)
            {
                throw new IOException("Connection closed while reading test payload");
            }
            offset += read;
        }
        return buffer;
    }

    private static string ExtractScramNonce(string clientFirst)
    {
        const string marker = "r=";
        var index = clientFirst.IndexOf(marker, StringComparison.Ordinal);
        Assert.True(index >= 0, $"SCRAM client-first message missing nonce: {clientFirst}");
        return clientFirst[(index + marker.Length)..];
    }

    private static void AssertManagerAuthStartPayload(byte[] payload, bool expectInlineToken)
    {
        var offset = 0;
        var userLength = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(offset, 4));
        offset += 4 + (int)userLength;
        Assert.True(offset < payload.Length);
        Assert.Equal(4, payload[offset]);
        offset += 1;
        var tokenLength = BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(offset, 4));
        Assert.Equal(expectInlineToken ? 1U : 0U, tokenLength > 0 ? 1U : 0U);
    }

    private static ManagerFrame ReceiveManagerFrame(NetworkStream stream)
    {
        var header = ReadExact(stream, 12);
        var magic = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(0, 4));
        Assert.Equal(0x42444253u, magic);
        var version = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(4, 2));
        Assert.Equal((ushort)0x0101, version);
        var type = header[6];
        var payloadLength = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(8, 4));
        var payload = payloadLength > 0 ? ReadExact(stream, (int)payloadLength) : Array.Empty<byte>();
        return new ManagerFrame(type, payload);
    }

    private static void SendManagerFrame(NetworkStream stream, byte type, byte[] payload)
    {
        var frame = new byte[12 + payload.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(0, 4), 0x42444253u);
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(4, 2), 0x0101);
        frame[6] = type;
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(8, 4), (uint)payload.Length);
        if (payload.Length > 0)
        {
            Buffer.BlockCopy(payload, 0, frame, 12, payload.Length);
        }
        stream.Write(frame, 0, frame.Length);
        stream.Flush();
    }

    private sealed record ManagerFrame(byte Type, byte[] Payload);

    private sealed class HarnessCapture
    {
        public string? TokenPayload { get; set; }
    }

    private sealed class LoopbackHarness : IDisposable
    {
        private readonly TcpListener _listener;
        private readonly Task _task;
        public int Port { get; }
        public HarnessCapture Captured { get; }

        private LoopbackHarness(TcpListener listener, Task task, int port, HarnessCapture captured)
        {
            _listener = listener;
            _task = task;
            Port = port;
            Captured = captured;
        }

        public static LoopbackHarness Start(Action<NetworkStream, HarnessCapture> handler)
        {
            var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            var port = ((IPEndPoint)listener.LocalEndpoint).Port;
            var capture = new HarnessCapture();
            var task = Task.Run(() =>
            {
                using var client = listener.AcceptTcpClient();
                using var stream = client.GetStream();
                handler(stream, capture);
            });

            return new LoopbackHarness(listener, task, port, capture);
        }

        public void Dispose()
        {
            _listener.Stop();
            _task.GetAwaiter().GetResult();
        }
    }
}
