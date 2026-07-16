// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/core/error_context.h"
#include "scratchbird/core/status.h"
#include "scratchbird/security/tls_config.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>

namespace {

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using PkeyContextPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using X509ExtensionPtr = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>;
using SslPtr = std::unique_ptr<SSL, decltype(&SSL_free)>;

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

PkeyPtr GenerateRsaKey() {
    PkeyContextPtr context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
                           EVP_PKEY_CTX_free);
    EVP_PKEY* raw_key = nullptr;
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) != 1 ||
        EVP_PKEY_keygen(context.get(), &raw_key) != 1) {
        return PkeyPtr(nullptr, EVP_PKEY_free);
    }
    return PkeyPtr(raw_key, EVP_PKEY_free);
}

bool AddExtension(X509* certificate,
                  X509* issuer,
                  int extension_nid,
                  const char* value) {
    X509V3_CTX context{};
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
    X509ExtensionPtr extension(
        X509V3_EXT_conf_nid(nullptr,
                            &context,
                            extension_nid,
                            const_cast<char*>(value)),
        X509_EXTENSION_free);
    return extension && X509_add_ext(certificate, extension.get(), -1) == 1;
}

X509Ptr MakeCertificate(EVP_PKEY* key,
                        const char* common_name,
                        std::int64_t serial,
                        X509* issuer,
                        EVP_PKEY* issuer_key,
                        bool certificate_authority) {
    X509Ptr certificate(X509_new(), X509_free);
    if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serial) != 1 ||
        X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) == nullptr ||
        X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400) == nullptr ||
        X509_set_pubkey(certificate.get(), key) != 1) {
        return X509Ptr(nullptr, X509_free);
    }

    X509_NAME* subject = X509_get_subject_name(certificate.get());
    if (subject == nullptr ||
        X509_NAME_add_entry_by_txt(subject,
                                   "CN",
                                   MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(common_name),
                                   -1,
                                   -1,
                                   0) != 1 ||
        X509_set_issuer_name(certificate.get(),
                             issuer == nullptr ? subject : X509_get_subject_name(issuer)) != 1) {
        return X509Ptr(nullptr, X509_free);
    }

    if (certificate_authority) {
        if (!AddExtension(certificate.get(), certificate.get(), NID_basic_constraints,
                          "critical,CA:TRUE") ||
            !AddExtension(certificate.get(), certificate.get(), NID_key_usage,
                          "critical,keyCertSign,cRLSign")) {
            return X509Ptr(nullptr, X509_free);
        }
    } else {
        if (!AddExtension(certificate.get(), issuer, NID_basic_constraints,
                          "critical,CA:FALSE") ||
            !AddExtension(certificate.get(), issuer, NID_key_usage,
                          "critical,digitalSignature,keyEncipherment") ||
            !AddExtension(certificate.get(), issuer, NID_ext_key_usage,
                          "serverAuth") ||
            !AddExtension(certificate.get(), issuer, NID_subject_alt_name,
                          "DNS:db.example.test,IP:127.0.0.1,IP:::1")) {
            return X509Ptr(nullptr, X509_free);
        }
    }

    EVP_PKEY* signing_key = issuer_key == nullptr ? key : issuer_key;
    if (X509_sign(certificate.get(), signing_key, EVP_sha256()) <= 0) {
        return X509Ptr(nullptr, X509_free);
    }
    return certificate;
}

bool WriteCertificate(const std::filesystem::path& path, X509* certificate) {
    std::unique_ptr<BIO, decltype(&BIO_free_all)> output(
        BIO_new_file(path.string().c_str(), "wb"), BIO_free_all);
    return output && PEM_write_bio_X509(output.get(), certificate) == 1;
}

bool WritePrivateKey(const std::filesystem::path& path, EVP_PKEY* key) {
    std::unique_ptr<BIO, decltype(&BIO_free_all)> output(
        BIO_new_file(path.string().c_str(), "wb"), BIO_free_all);
    return output &&
           PEM_write_bio_PrivateKey(output.get(), key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
}

struct TlsFixture {
    std::filesystem::path root;
    std::filesystem::path ca_certificate;
    std::filesystem::path server_certificate;
    std::filesystem::path server_key;

    ~TlsFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

bool PrepareFixture(TlsFixture* fixture) {
    if (fixture == nullptr) return false;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count() ^
                       static_cast<std::int64_t>(std::random_device{}());
    fixture->root = std::filesystem::temp_directory_path() /
                    ("scratchbird_tls_identity_" + std::to_string(nonce));
    std::error_code error;
    std::filesystem::create_directories(fixture->root, error);
    if (error) return false;

    fixture->ca_certificate = fixture->root / "ca.pem";
    fixture->server_certificate = fixture->root / "server.pem";
    fixture->server_key = fixture->root / "server-key.pem";

    auto ca_key = GenerateRsaKey();
    auto server_key = GenerateRsaKey();
    auto ca = MakeCertificate(ca_key.get(), "ScratchBird TLS identity test CA", 1,
                              nullptr, nullptr, true);
    auto server = MakeCertificate(server_key.get(), "ignored-cn.example.test", 2,
                                  ca.get(), ca_key.get(), false);
    return ca_key && server_key && ca && server &&
           WriteCertificate(fixture->ca_certificate, ca.get()) &&
           WriteCertificate(fixture->server_certificate, server.get()) &&
           WritePrivateKey(fixture->server_key, server_key.get());
}

struct HandshakeResult {
    bool complete = false;
    bool client_failed = false;
    long verify_result = X509_V_OK;
};

HandshakeResult RunHandshake(scratchbird::security::TLSContext& client_context,
                             scratchbird::security::TLSContext& server_context) {
    HandshakeResult result;
    ERR_clear_error();
    SslPtr client(SSL_new(client_context.get()), SSL_free);
    SslPtr server(SSL_new(server_context.get()), SSL_free);
    if (!client || !server) {
        result.client_failed = true;
        return result;
    }

    BIO* client_bio = nullptr;
    BIO* server_bio = nullptr;
    if (BIO_new_bio_pair(&client_bio, 0, &server_bio, 0) != 1) {
        result.client_failed = true;
        return result;
    }
    SSL_set_bio(client.get(), client_bio, client_bio);
    SSL_set_bio(server.get(), server_bio, server_bio);
    SSL_set_connect_state(client.get());
    SSL_set_accept_state(server.get());

    bool client_complete = false;
    bool server_complete = false;
    bool server_failed = false;
    for (int iteration = 0; iteration < 512; ++iteration) {
        if (!client_complete && !result.client_failed) {
            const int status = SSL_do_handshake(client.get());
            if (status == 1) {
                client_complete = true;
            } else {
                const int error = SSL_get_error(client.get(), status);
                if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
                    result.client_failed = true;
                }
            }
        }
        if (!server_complete && !server_failed) {
            const int status = SSL_do_handshake(server.get());
            if (status == 1) {
                server_complete = true;
            } else {
                const int error = SSL_get_error(server.get(), status);
                if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
                    server_failed = true;
                }
            }
        }
        if ((client_complete && server_complete) || result.client_failed || server_failed) {
            break;
        }
    }

    result.complete = client_complete && server_complete;
    result.verify_result = SSL_get_verify_result(client.get());
    return result;
}

std::unique_ptr<scratchbird::security::TLSContext> MakeServerContext(
    const TlsFixture& fixture) {
    scratchbird::security::TLSConfig config;
    config.enabled = true;
    config.cert_file = fixture.server_certificate.string();
    config.key_file = fixture.server_key.string();
    scratchbird::core::ErrorContext error;
    auto context = scratchbird::security::TLSContext::createServer(config, &error);
    if (!context) {
        std::cerr << "server TLS context failed: " << error.message << '\n';
    }
    return context;
}

std::unique_ptr<scratchbird::security::TLSContext> MakeClientContext(
    const TlsFixture& fixture,
    bool verify_identity,
    std::string expected_identity,
    scratchbird::core::ErrorContext* error) {
    scratchbird::security::TLSClientConfig config;
    config.verify_server = true;
    config.verify_identity = verify_identity;
    config.expected_hostname = std::move(expected_identity);
    config.use_system_ca = false;
    config.ca_file = fixture.ca_certificate.string();
    return scratchbird::security::TLSContext::createClient(config, error);
}

void TestIdentityHandshake(const TlsFixture& fixture,
                           const std::string& identity,
                           bool expect_success,
                           const std::string& description) {
    auto server = MakeServerContext(fixture);
    scratchbird::core::ErrorContext error;
    auto client = MakeClientContext(fixture, true, identity, &error);
    Expect(server != nullptr, description + ": server context creation");
    Expect(client != nullptr, description + ": client context creation: " + error.message);
    if (!server || !client) return;

    const auto handshake = RunHandshake(*client, *server);
    if (expect_success) {
        Expect(handshake.complete, description + ": handshake must succeed");
        Expect(handshake.verify_result == X509_V_OK,
               description + ": certificate verification must succeed");
    } else {
        Expect(!handshake.complete && handshake.client_failed,
               description + ": client handshake must fail closed");
#ifdef X509_V_ERR_HOSTNAME_MISMATCH
        Expect(handshake.verify_result == X509_V_ERR_HOSTNAME_MISMATCH,
               description + ": failure must be a hostname mismatch");
#endif
    }
}

void TestVerifyCaDoesNotCheckIdentity(const TlsFixture& fixture) {
    auto server = MakeServerContext(fixture);
    scratchbird::core::ErrorContext error;
    auto client = MakeClientContext(fixture, false, "wrong.example.test", &error);
    Expect(server != nullptr, "VERIFY_CA server context creation");
    Expect(client != nullptr, "VERIFY_CA client context creation: " + error.message);
    if (!server || !client) return;

    const auto handshake = RunHandshake(*client, *server);
    Expect(handshake.complete,
           "VERIFY_CA must verify the chain without enforcing the configured name");
    Expect(handshake.verify_result == X509_V_OK,
           "VERIFY_CA chain verification must succeed");
}

void TestVerifyFullRequiresIdentity(const TlsFixture& fixture) {
    scratchbird::core::ErrorContext error;
    auto client = MakeClientContext(fixture, true, "", &error);
    Expect(client == nullptr, "VERIFY_FULL must reject a missing expected identity");
    Expect(error.message.find("identity verification") != std::string::npos,
           "missing VERIFY_FULL identity must produce a stable validation detail");

    scratchbird::security::TLSClientConfig invalid_mode;
    invalid_mode.verify_server = false;
    invalid_mode.verify_identity = true;
    invalid_mode.expected_hostname = "db.example.test";
    scratchbird::core::ErrorContext invalid_mode_error;
    Expect(invalid_mode.validate(&invalid_mode_error) ==
               scratchbird::core::Status::INVALID_ARGUMENT,
           "identity verification must not be enabled without chain verification");
    Expect(invalid_mode_error.message.find("certificate verification") != std::string::npos,
           "invalid identity/chain mode must produce a stable validation detail");
}

}  // namespace

int main() {
    TlsFixture fixture;
    if (!PrepareFixture(&fixture)) {
        std::cerr << "FAIL: could not prepare TLS certificate fixture\n";
        return 1;
    }

    Expect(!scratchbird::security::isIpAddressLiteral("db.example.test"),
           "DNS name must not be classified as an IP literal for SNI policy");
    Expect(scratchbird::security::isIpAddressLiteral("127.0.0.1"),
           "IPv4 identity classification");
    Expect(scratchbird::security::isIpAddressLiteral("[::1]"),
           "bracketed IPv6 identity classification");

    TestIdentityHandshake(fixture, "db.example.test", true,
                          "VERIFY_FULL matching DNS SAN");
    TestIdentityHandshake(fixture, "127.0.0.1", true,
                          "VERIFY_FULL matching IP SAN");
    TestIdentityHandshake(fixture, "wrong.example.test", false,
                          "VERIFY_FULL wrong host");
    TestVerifyCaDoesNotCheckIdentity(fixture);
    TestVerifyFullRequiresIdentity(fixture);

    if (failures != 0) {
        std::cerr << failures << " TLS peer identity test failure(s)\n";
        return 1;
    }
    std::cout << "scratchbird_client_tls_peer_identity_test: PASS\n";
    return 0;
}
