// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/sha512"
	"encoding/base64"
	"errors"
	"fmt"
	"hash"
	"strings"
)

type scramClient struct {
	username        string
	digest          string
	clientNonce     string
	clientFirstBare string
	serverSignature []byte
}

func newScramClient(username string, digest string) (*scramClient, error) {
	nonce := make([]byte, 18)
	if _, err := rand.Read(nonce); err != nil {
		return nil, err
	}
	if digest == "" {
		digest = "sha256"
	}
	return &scramClient{
		username:    username,
		digest:      digest,
		clientNonce: base64.StdEncoding.EncodeToString(nonce),
	}, nil
}

func (s *scramClient) clientFirstMessage() string {
	s.clientFirstBare = fmt.Sprintf("n=%s,r=%s", escapeScram(s.username), s.clientNonce)
	return "n,," + s.clientFirstBare
}

func (s *scramClient) handleServerFirst(password, serverFirst string) (string, error) {
	attrs := parseScramAttrs(serverFirst)
	nonce := attrs["r"]
	saltB64 := attrs["s"]
	iterStr := attrs["i"]
	if nonce == "" || !strings.HasPrefix(nonce, s.clientNonce) {
		return "", errors.New("SCRAM server nonce mismatch")
	}
	if saltB64 == "" || iterStr == "" {
		return "", errors.New("SCRAM server-first missing fields")
	}
	iterations, err := parseInt(iterStr)
	if err != nil {
		return "", err
	}
	salt, err := base64.StdEncoding.DecodeString(saltB64)
	if err != nil {
		return "", err
	}
	salted := pbkdf2Digest([]byte(password), salt, iterations, digestSize(s.digest), s.digest)
	clientKey := hmacDigest(salted, []byte("Client Key"), s.digest)
	storedKey := digestSum(clientKey, s.digest)
	clientFinalWithoutProof := "c=biws,r=" + nonce
	authMessage := s.clientFirstBare + "," + serverFirst + "," + clientFinalWithoutProof
	clientSignature := hmacDigest(storedKey, []byte(authMessage), s.digest)
	clientProof := xorBytes(clientKey, clientSignature)
	serverKey := hmacDigest(salted, []byte("Server Key"), s.digest)
	s.serverSignature = hmacDigest(serverKey, []byte(authMessage), s.digest)
	return clientFinalWithoutProof + ",p=" + base64.StdEncoding.EncodeToString(clientProof), nil
}

func (s *scramClient) verifyServerFinal(serverFinal string) error {
	attrs := parseScramAttrs(serverFinal)
	verifier := attrs["v"]
	if verifier == "" || len(s.serverSignature) == 0 {
		return errors.New("SCRAM server-final missing verifier")
	}
	expected := base64.StdEncoding.EncodeToString(s.serverSignature)
	if verifier != expected {
		return errors.New("SCRAM server signature mismatch")
	}
	return nil
}

func escapeScram(input string) string {
	replacer := strings.NewReplacer("=", "=3D", ",", "=2C")
	return replacer.Replace(input)
}

func parseScramAttrs(message string) map[string]string {
	attrs := map[string]string{}
	if message == "" {
		return attrs
	}
	parts := strings.Split(message, ",")
	for _, part := range parts {
		if len(part) < 3 {
			continue
		}
		if idx := strings.Index(part, "="); idx > 0 {
			attrs[part[:idx]] = part[idx+1:]
		}
	}
	return attrs
}

func digestFactory(name string) func() hash.Hash {
	switch strings.ToLower(strings.TrimSpace(name)) {
	case "sha512":
		return sha512.New
	default:
		return sha256.New
	}
}

func digestSize(name string) int {
	switch strings.ToLower(strings.TrimSpace(name)) {
	case "sha512":
		return sha512.Size
	default:
		return sha256.Size
	}
}

func hmacDigest(key, data []byte, digest string) []byte {
	mac := hmac.New(digestFactory(digest), key)
	mac.Write(data)
	return mac.Sum(nil)
}

func digestSum(data []byte, digest string) []byte {
	switch strings.ToLower(strings.TrimSpace(digest)) {
	case "sha512":
		sum := sha512.Sum512(data)
		return sum[:]
	default:
		sum := sha256.Sum256(data)
		return sum[:]
	}
}

func xorBytes(left, right []byte) []byte {
	out := make([]byte, len(left))
	for i := range left {
		out[i] = left[i] ^ right[i]
	}
	return out
}

func parseInt(text string) (int, error) {
	var value int
	for _, ch := range text {
		if ch < '0' || ch > '9' {
			return 0, errors.New("invalid SCRAM iteration count")
		}
		value = value*10 + int(ch-'0')
	}
	return value, nil
}

func pbkdf2Digest(password, salt []byte, iterations, keyLen int, digest string) []byte {
	size := digestSize(digest)
	blockCount := (keyLen + size - 1) / size
	var out []byte
	for i := 1; i <= blockCount; i++ {
		t := pbkdf2F(password, salt, iterations, i, digest)
		out = append(out, t...)
	}
	return out[:keyLen]
}

func pbkdf2F(password, salt []byte, iterations, blockIndex int, digest string) []byte {
	block := make([]byte, len(salt)+4)
	copy(block, salt)
	block[len(block)-4] = byte(blockIndex >> 24)
	block[len(block)-3] = byte(blockIndex >> 16)
	block[len(block)-2] = byte(blockIndex >> 8)
	block[len(block)-1] = byte(blockIndex)
	u := hmacDigest(password, block, digest)
	out := make([]byte, len(u))
	copy(out, u)
	for i := 1; i < iterations; i++ {
		u = hmacDigest(password, u, digest)
		for j := range out {
			out[j] ^= u[j]
		}
	}
	return out
}
