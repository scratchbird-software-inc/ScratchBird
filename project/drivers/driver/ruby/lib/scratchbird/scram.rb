# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "openssl"
require "base64"

module Scratchbird
  class Scram
    def initialize(username, digest = "sha256")
      @username = username
      @digest = digest.to_s.strip.empty? ? "sha256" : digest.to_s
      @client_nonce = Base64.strict_encode64(OpenSSL::Random.random_bytes(18))
      @client_first_bare = ""
      @server_signature = nil
    end

    def client_first_message
      @client_first_bare = "n=#{escape(@username)},r=#{@client_nonce}"
      "n,,#{@client_first_bare}"
    end

    def handle_server_first(password, server_first)
      attrs = parse_attributes(server_first)
      nonce = attrs["r"]
      salt_b64 = attrs["s"]
      iter_str = attrs["i"]
      raise "SCRAM server nonce mismatch" unless nonce && nonce.start_with?(@client_nonce)
      raise "SCRAM server-first missing fields" if salt_b64.nil? || iter_str.nil?

      iterations = iter_str.to_i
      salt = Base64.decode64(salt_b64)
      salted = OpenSSL::PKCS5.pbkdf2_hmac(password, salt, iterations, digest_length, @digest)
      client_key = hmac(salted, "Client Key")
      stored_key = digest_bytes(client_key)
      client_final_without_proof = "c=biws,r=#{nonce}"
      auth_message = "#{@client_first_bare},#{server_first},#{client_final_without_proof}"
      client_signature = hmac(stored_key, auth_message)
      client_proof = xor_bytes(client_key, client_signature)
      server_key = hmac(salted, "Server Key")
      @server_signature = hmac(server_key, auth_message)
      "#{client_final_without_proof},p=#{Base64.strict_encode64(client_proof)}"
    end

    def verify_server_final(server_final)
      attrs = parse_attributes(server_final)
      verifier = attrs["v"]
      raise "SCRAM server-final missing verifier" if verifier.nil? || @server_signature.nil?
      expected = Base64.strict_encode64(@server_signature)
      raise "SCRAM server signature mismatch" unless verifier == expected
    end

    private

    def escape(value)
      value.to_s.gsub("=", "=3D").gsub(",", "=2C")
    end

    def parse_attributes(message)
      attrs = {}
      return attrs if message.to_s.empty?
      message.split(",").each do |part|
        idx = part.index("=")
        next unless idx && idx > 0
        attrs[part[0...idx]] = part[(idx + 1)..]
      end
      attrs
    end

    def hmac(key, data)
      OpenSSL::HMAC.digest(@digest, key, data)
    end

    def digest_bytes(data)
      OpenSSL::Digest.digest(@digest, data)
    end

    def digest_length
      OpenSSL::Digest.new(@digest).digest_length
    end

    def xor_bytes(left, right)
      out = +""
      left.bytes.each_with_index do |byte, idx|
        out << (byte ^ right.getbyte(idx)).chr
      end
      out
    end
  end
end
