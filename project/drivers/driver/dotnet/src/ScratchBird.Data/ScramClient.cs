// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Security.Cryptography;
using System.Text;

namespace ScratchBird.Data;

internal sealed class ScramClient
{
    private readonly string _username;
    private readonly string _nonce;
    private readonly HashAlgorithmName _hashAlgorithm;
    private string _clientFirstBare = string.Empty;
    private byte[]? _serverSignature;

    public ScramClient(string username, HashAlgorithmName? hashAlgorithm = null, string? nonce = null)
    {
        _username = username ?? string.Empty;
        _hashAlgorithm = hashAlgorithm ?? HashAlgorithmName.SHA256;
        _nonce = nonce ?? Convert.ToBase64String(RandomNumberGenerator.GetBytes(18));
    }

    public string ClientFirstMessage()
    {
        _clientFirstBare = $"n={Escape(_username)},r={_nonce}";
        return $"n,,{_clientFirstBare}";
    }

    public string HandleServerFirst(string password, string serverFirst)
    {
        var attrs = ParseAttributes(serverFirst);
        if (!attrs.TryGetValue("r", out var nonce) || !nonce.StartsWith(_nonce, StringComparison.Ordinal))
        {
            throw new InvalidOperationException("SCRAM server nonce mismatch");
        }
        if (!attrs.TryGetValue("s", out var saltB64) || !attrs.TryGetValue("i", out var iterStr))
        {
            throw new InvalidOperationException("SCRAM server-first missing fields");
        }

        var iterations = int.Parse(iterStr);
        var salt = Convert.FromBase64String(saltB64);
        var saltedPassword = Hi(password ?? string.Empty, salt, iterations);
        var clientKey = Hmac(saltedPassword, "Client Key");
        var storedKey = Hash(clientKey);
        var clientFinalWithoutProof = $"c=biws,r={nonce}";
        var authMessage = $"{_clientFirstBare},{serverFirst},{clientFinalWithoutProof}";
        var clientSignature = Hmac(storedKey, authMessage);
        var clientProof = Xor(clientKey, clientSignature);
        var serverKey = Hmac(saltedPassword, "Server Key");
        _serverSignature = Hmac(serverKey, authMessage);
        return $"{clientFinalWithoutProof},p={Convert.ToBase64String(clientProof)}";
    }

    public void VerifyServerFinal(string serverFinal)
    {
        var attrs = ParseAttributes(serverFinal);
        if (!attrs.TryGetValue("v", out var verifier) || _serverSignature == null)
        {
            throw new InvalidOperationException("SCRAM server-final missing verifier");
        }
        var expected = Convert.ToBase64String(_serverSignature);
        if (!string.Equals(verifier, expected, StringComparison.Ordinal))
        {
            throw new InvalidOperationException("SCRAM server signature mismatch");
        }
    }

    private static string Escape(string value)
    {
        return value.Replace("=", "=3D", StringComparison.Ordinal).Replace(",", "=2C", StringComparison.Ordinal);
    }

    private static Dictionary<string, string> ParseAttributes(string message)
    {
        var dict = new Dictionary<string, string>(StringComparer.Ordinal);
        if (string.IsNullOrEmpty(message)) return dict;
        var parts = message.Split(',');
        foreach (var part in parts)
        {
            var idx = part.IndexOf('=');
            if (idx > 0)
            {
                dict[part[..idx]] = part[(idx + 1)..];
            }
        }
        return dict;
    }

    private byte[] Hi(string password, byte[] salt, int iterations)
    {
        using var derive = new Rfc2898DeriveBytes(password, salt, iterations, _hashAlgorithm);
        return derive.GetBytes(GetDigestLength(_hashAlgorithm));
    }

    private byte[] Hmac(byte[] key, string data)
    {
        using var hmac = CreateHmac(key, _hashAlgorithm);
        return hmac.ComputeHash(Encoding.UTF8.GetBytes(data));
    }

    private byte[] Hash(byte[] value)
    {
        return _hashAlgorithm.Name switch
        {
            nameof(HashAlgorithmName.SHA512) => SHA512.HashData(value),
            _ => SHA256.HashData(value)
        };
    }

    private static byte[] Xor(byte[] left, byte[] right)
    {
        var output = new byte[left.Length];
        for (var i = 0; i < left.Length; i++)
        {
            output[i] = (byte)(left[i] ^ right[i]);
        }
        return output;
    }

    private static int GetDigestLength(HashAlgorithmName algorithm)
    {
        return algorithm.Name switch
        {
            nameof(HashAlgorithmName.SHA512) => 64,
            _ => 32
        };
    }

    private static HMAC CreateHmac(byte[] key, HashAlgorithmName algorithm)
    {
        return algorithm.Name switch
        {
            nameof(HashAlgorithmName.SHA512) => new HMACSHA512(key),
            _ => new HMACSHA256(key)
        };
    }
}
