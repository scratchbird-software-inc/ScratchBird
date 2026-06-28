// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use base64::{engine::general_purpose, Engine as _};
use hmac::{Hmac, Mac};
use pbkdf2::pbkdf2_hmac;
use rand::RngCore;
use sha2::{Digest, Sha256, Sha512};

use crate::errors::{Error, ErrorKind, Result};

type HmacSha256 = Hmac<Sha256>;
type HmacSha512 = Hmac<Sha512>;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ScramAlgorithm {
    Sha256,
    Sha512,
}

pub struct ScramExchange {
    username: String,
    client_nonce: String,
    client_first_bare: String,
    server_signature: Option<Vec<u8>>,
    algorithm: ScramAlgorithm,
}

impl ScramExchange {
    pub fn new(username: &str) -> Self {
        Self::with_algorithm(username, ScramAlgorithm::Sha256)
    }

    pub fn with_algorithm(username: &str, algorithm: ScramAlgorithm) -> Self {
        let mut nonce = [0u8; 18];
        rand::thread_rng().fill_bytes(&mut nonce);
        Self {
            username: username.to_string(),
            client_nonce: general_purpose::STANDARD.encode(nonce),
            client_first_bare: String::new(),
            server_signature: None,
            algorithm,
        }
    }

    pub fn client_first_message(&mut self) -> String {
        let escaped = self.username.replace("=", "=3D").replace(",", "=2C");
        self.client_first_bare = format!("n={},r={}", escaped, self.client_nonce);
        format!("n,,{}", self.client_first_bare)
    }

    pub fn handle_server_first(&mut self, password: &str, server_first: &str) -> Result<String> {
        let attrs = parse_attributes(server_first);
        let nonce = attrs
            .get("r")
            .ok_or_else(|| Error::new(ErrorKind::Auth, "SCRAM missing nonce"))?;
        if !nonce.starts_with(&self.client_nonce) {
            return Err(Error::new(ErrorKind::Auth, "SCRAM server nonce mismatch"));
        }
        let salt_b64 = attrs
            .get("s")
            .ok_or_else(|| Error::new(ErrorKind::Auth, "SCRAM missing salt"))?;
        let iter = attrs
            .get("i")
            .ok_or_else(|| Error::new(ErrorKind::Auth, "SCRAM missing iterations"))?
            .parse::<u32>()
            .map_err(|_| Error::new(ErrorKind::Auth, "SCRAM invalid iterations"))?;
        let salt = general_purpose::STANDARD
            .decode(salt_b64.as_bytes())
            .map_err(|_| Error::new(ErrorKind::Auth, "SCRAM invalid salt"))?;

        let salted = pbkdf2_bytes(self.algorithm, password.as_bytes(), &salt, iter);
        let client_key = hmac_bytes(self.algorithm, &salted, b"Client Key");
        let stored_key = digest_bytes(self.algorithm, &client_key);
        let client_final_without_proof = format!("c=biws,r={}", nonce);
        let auth_message = format!(
            "{},{},{}",
            self.client_first_bare, server_first, client_final_without_proof
        );
        let client_signature = hmac_bytes(self.algorithm, &stored_key, auth_message.as_bytes());
        let client_proof = xor_bytes(&client_key, &client_signature);
        let server_key = hmac_bytes(self.algorithm, &salted, b"Server Key");
        self.server_signature = Some(hmac_bytes(
            self.algorithm,
            &server_key,
            auth_message.as_bytes(),
        ));
        let proof_b64 = general_purpose::STANDARD.encode(client_proof);
        Ok(format!("{},p={}", client_final_without_proof, proof_b64))
    }

    pub fn verify_server_final(&self, server_final: &str) -> Result<()> {
        let attrs = parse_attributes(server_final);
        let verifier = attrs
            .get("v")
            .ok_or_else(|| Error::new(ErrorKind::Auth, "SCRAM missing verifier"))?;
        let signature = self
            .server_signature
            .as_ref()
            .ok_or_else(|| Error::new(ErrorKind::Auth, "SCRAM missing server signature"))?;
        let expected = general_purpose::STANDARD.encode(signature);
        if &expected != verifier {
            return Err(Error::new(
                ErrorKind::Auth,
                "SCRAM server signature mismatch",
            ));
        }
        Ok(())
    }
}

fn parse_attributes(message: &str) -> std::collections::HashMap<String, String> {
    let mut attrs = std::collections::HashMap::new();
    for part in message.split(',') {
        if let Some(idx) = part.find('=') {
            let key = &part[..idx];
            let value = &part[idx + 1..];
            attrs.insert(key.to_string(), value.to_string());
        }
    }
    attrs
}

fn pbkdf2_bytes(
    algorithm: ScramAlgorithm,
    password: &[u8],
    salt: &[u8],
    iterations: u32,
) -> Vec<u8> {
    match algorithm {
        ScramAlgorithm::Sha256 => {
            let mut salted = [0u8; 32];
            pbkdf2_hmac::<Sha256>(password, salt, iterations, &mut salted);
            salted.to_vec()
        }
        ScramAlgorithm::Sha512 => {
            let mut salted = [0u8; 64];
            pbkdf2_hmac::<Sha512>(password, salt, iterations, &mut salted);
            salted.to_vec()
        }
    }
}

fn hmac_bytes(algorithm: ScramAlgorithm, key: &[u8], data: &[u8]) -> Vec<u8> {
    match algorithm {
        ScramAlgorithm::Sha256 => {
            let mut mac = HmacSha256::new_from_slice(key).expect("hmac key");
            mac.update(data);
            mac.finalize().into_bytes().to_vec()
        }
        ScramAlgorithm::Sha512 => {
            let mut mac = HmacSha512::new_from_slice(key).expect("hmac key");
            mac.update(data);
            mac.finalize().into_bytes().to_vec()
        }
    }
}

fn digest_bytes(algorithm: ScramAlgorithm, data: &[u8]) -> Vec<u8> {
    match algorithm {
        ScramAlgorithm::Sha256 => {
            let mut hasher = Sha256::new();
            hasher.update(data);
            hasher.finalize().to_vec()
        }
        ScramAlgorithm::Sha512 => {
            let mut hasher = Sha512::new();
            hasher.update(data);
            hasher.finalize().to_vec()
        }
    }
}

fn xor_bytes(left: &[u8], right: &[u8]) -> Vec<u8> {
    left.iter().zip(right.iter()).map(|(l, r)| l ^ r).collect()
}
