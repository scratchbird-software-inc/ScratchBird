// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver
 * Copyright (c) 2025 ScratchBird Project
 */
package com.scratchbird.jdbc;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.SecureRandom;
import java.util.Base64;
import java.util.LinkedHashMap;
import java.util.Map;
import javax.crypto.Mac;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.PBEKeySpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * SCRAM client helper for SASL authentication.
 */
public class SBScramClient {

    public enum Algorithm {
        SHA_256("HmacSHA256", "PBKDF2WithHmacSHA256", "SHA-256", 256),
        SHA_512("HmacSHA512", "PBKDF2WithHmacSHA512", "SHA-512", 512);

        private final String hmacAlgorithm;
        private final String pbkdf2Algorithm;
        private final String digestAlgorithm;
        private final int derivedKeyBits;

        Algorithm(String hmacAlgorithm, String pbkdf2Algorithm, String digestAlgorithm, int derivedKeyBits) {
            this.hmacAlgorithm = hmacAlgorithm;
            this.pbkdf2Algorithm = pbkdf2Algorithm;
            this.digestAlgorithm = digestAlgorithm;
            this.derivedKeyBits = derivedKeyBits;
        }
    }

    private static final SecureRandom RNG = new SecureRandom();

    private final String username;
    private final String clientNonce;
    private final Algorithm algorithm;
    private String clientFirstBare;
    private String serverFirstMessage;
    private String authMessage;
    private byte[] serverSignature;

    public SBScramClient(String username) {
        this(username, Algorithm.SHA_256, generateNonce());
    }

    public SBScramClient(String username, Algorithm algorithm) {
        this(username, algorithm, generateNonce());
    }

    public SBScramClient(String username, String clientNonce) {
        this(username, Algorithm.SHA_256, clientNonce);
    }

    public SBScramClient(String username, Algorithm algorithm, String clientNonce) {
        this.username = username != null ? username : "";
        this.algorithm = algorithm != null ? algorithm : Algorithm.SHA_256;
        this.clientNonce = clientNonce != null ? clientNonce : generateNonce();
    }

    public String getClientFirstMessage() {
        clientFirstBare = "n=" + escape(username) + ",r=" + clientNonce;
        return "n,," + clientFirstBare;
    }

    public String handleServerFirst(String serverFirst, String password) {
        serverFirstMessage = serverFirst;
        Map<String, String> attrs = parseAttributes(serverFirst);
        String nonce = attrs.get("r");
        String saltB64 = attrs.get("s");
        String iterStr = attrs.get("i");

        if (nonce == null || !nonce.startsWith(clientNonce)) {
            throw new IllegalStateException("SCRAM server nonce mismatch");
        }
        if (saltB64 == null || iterStr == null) {
            throw new IllegalStateException("SCRAM server-first missing fields");
        }

        int iterations = Integer.parseInt(iterStr);
        byte[] salt = Base64.getDecoder().decode(saltB64);
        byte[] saltedPassword = hi(password != null ? password : "", salt, iterations);

        byte[] clientKey = hmac(saltedPassword, "Client Key");
        byte[] storedKey = digest(clientKey);

        String clientFinalWithoutProof = "c=biws,r=" + nonce;
        authMessage = clientFirstBare + "," + serverFirstMessage + "," + clientFinalWithoutProof;

        byte[] clientSignature = hmac(storedKey, authMessage);
        byte[] clientProof = xor(clientKey, clientSignature);

        byte[] serverKey = hmac(saltedPassword, "Server Key");
        serverSignature = hmac(serverKey, authMessage);

        return clientFinalWithoutProof + ",p=" + Base64.getEncoder().encodeToString(clientProof);
    }

    public void verifyServerFinal(String serverFinal) {
        Map<String, String> attrs = parseAttributes(serverFinal);
        String verifier = attrs.get("v");
        if (verifier == null) {
            throw new IllegalStateException("SCRAM server-final missing verifier");
        }
        byte[] received = Base64.getDecoder().decode(verifier);
        if (!MessageDigest.isEqual(received, serverSignature)) {
            throw new IllegalStateException("SCRAM server signature mismatch");
        }
    }

    String getServerSignatureBase64() {
        return serverSignature == null
            ? null
            : Base64.getEncoder().encodeToString(serverSignature);
    }

    private static String escape(String value) {
        return value.replace("=", "=3D").replace(",", "=2C");
    }

    private static Map<String, String> parseAttributes(String message) {
        Map<String, String> attrs = new LinkedHashMap<>();
        if (message == null || message.isEmpty()) {
            return attrs;
        }
        String[] parts = message.split(",");
        for (String part : parts) {
            int eq = part.indexOf('=');
            if (eq > 0) {
                attrs.put(part.substring(0, eq), part.substring(eq + 1));
            }
        }
        return attrs;
    }

    private byte[] hi(String password, byte[] salt, int iterations) {
        try {
            PBEKeySpec spec = new PBEKeySpec(password.toCharArray(), salt, iterations, algorithm.derivedKeyBits);
            SecretKeyFactory factory = SecretKeyFactory.getInstance(algorithm.pbkdf2Algorithm);
            return factory.generateSecret(spec).getEncoded();
        } catch (Exception e) {
            throw new IllegalStateException("SCRAM PBKDF2 failed", e);
        }
    }

    private byte[] hmac(byte[] key, String data) {
        try {
            Mac mac = Mac.getInstance(algorithm.hmacAlgorithm);
            mac.init(new SecretKeySpec(key, algorithm.hmacAlgorithm));
            return mac.doFinal(data.getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            throw new IllegalStateException("SCRAM HMAC failed", e);
        }
    }

    private byte[] digest(byte[] data) {
        try {
            MessageDigest digest = MessageDigest.getInstance(algorithm.digestAlgorithm);
            return digest.digest(data);
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SCRAM digest not available", e);
        }
    }

    private static byte[] xor(byte[] left, byte[] right) {
        byte[] out = new byte[left.length];
        for (int i = 0; i < left.length; i++) {
            out[i] = (byte) (left[i] ^ right[i]);
        }
        return out;
    }

    private static String generateNonce() {
        byte[] bytes = new byte[18];
        RNG.nextBytes(bytes);
        return Base64.getEncoder().encodeToString(bytes);
    }
}
