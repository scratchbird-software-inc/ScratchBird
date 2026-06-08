<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird\PDO;

final class Scram
{
    public const ALGORITHM_SHA256 = 'sha256';
    public const ALGORITHM_SHA512 = 'sha512';

    private string $username;
    private string $clientNonce;
    private string $clientFirstBare = '';
    private string $serverSignature = '';
    private string $algorithm;

    public function __construct(string $username, string $algorithm = self::ALGORITHM_SHA256)
    {
        $this->username = $username;
        $this->clientNonce = base64_encode(random_bytes(18));
        $normalized = strtolower(trim($algorithm));
        if ($normalized !== self::ALGORITHM_SHA256 && $normalized !== self::ALGORITHM_SHA512) {
            throw new \InvalidArgumentException('unsupported SCRAM algorithm');
        }
        $this->algorithm = $normalized;
    }

    public function clientFirstMessage(): string
    {
        $this->clientFirstBare = 'n=' . $this->escape($this->username) . ',r=' . $this->clientNonce;
        return 'n,,' . $this->clientFirstBare;
    }

    public function handleServerFirst(string $password, string $serverFirst): string
    {
        $attrs = $this->parseAttributes($serverFirst);
        $nonce = $attrs['r'] ?? '';
        $saltB64 = $attrs['s'] ?? '';
        $iterations = isset($attrs['i']) ? (int)$attrs['i'] : 0;
        if ($nonce === '' || !str_starts_with($nonce, $this->clientNonce)) {
            throw new \RuntimeException('SCRAM server nonce mismatch');
        }
        if ($saltB64 === '' || $iterations <= 0) {
            throw new \RuntimeException('SCRAM server-first missing fields');
        }
        $salt = base64_decode($saltB64, true);
        if ($salt === false) {
            throw new \RuntimeException('Invalid SCRAM salt');
        }
        $digestLength = $this->algorithm === self::ALGORITHM_SHA512 ? 64 : 32;
        $salted = hash_pbkdf2($this->algorithm, $password, $salt, $iterations, $digestLength, true);
        $clientKey = $this->hmac($salted, 'Client Key');
        $storedKey = hash($this->algorithm, $clientKey, true);
        $clientFinalWithoutProof = 'c=biws,r=' . $nonce;
        $authMessage = $this->clientFirstBare . ',' . $serverFirst . ',' . $clientFinalWithoutProof;
        $clientSignature = $this->hmac($storedKey, $authMessage);
        $clientProof = $this->xorBytes($clientKey, $clientSignature);
        $serverKey = $this->hmac($salted, 'Server Key');
        $this->serverSignature = $this->hmac($serverKey, $authMessage);
        return $clientFinalWithoutProof . ',p=' . base64_encode($clientProof);
    }

    public function verifyServerFinal(string $serverFinal): void
    {
        $attrs = $this->parseAttributes($serverFinal);
        $verifier = $attrs['v'] ?? '';
        if ($verifier === '' || $this->serverSignature === '') {
            throw new \RuntimeException('SCRAM server-final missing verifier');
        }
        if ($verifier !== base64_encode($this->serverSignature)) {
            throw new \RuntimeException('SCRAM server signature mismatch');
        }
    }

    private function escape(string $value): string
    {
        return str_replace(['=', ','], ['=3D', '=2C'], $value);
    }

    private function parseAttributes(string $message): array
    {
        $attrs = [];
        foreach (explode(',', $message) as $part) {
            $pos = strpos($part, '=');
            if ($pos === false) {
                continue;
            }
            $attrs[substr($part, 0, $pos)] = substr($part, $pos + 1);
        }
        return $attrs;
    }

    private function hmac(string $key, string $data): string
    {
        return hash_hmac($this->algorithm, $data, $key, true);
    }

    private function xorBytes(string $left, string $right): string
    {
        $out = '';
        for ($i = 0; $i < strlen($left); $i++) {
            $out .= $left[$i] ^ $right[$i];
        }
        return $out;
    }
}
