#!/usr/bin/env bash
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab_dir="$(cd "${script_dir}/.." && pwd)"
pki_dir="${lab_dir}/generated/pki"

if [[ -f "${pki_dir}/ca.crt" && -f "${pki_dir}/ca.crl.pem" ]]; then
  exit 0
fi

mkdir -p "${pki_dir}/newcerts"
touch "${pki_dir}/index.txt"
printf '1000\n' >"${pki_dir}/serial"
printf '1000\n' >"${pki_dir}/crlnumber"

openssl_config="${pki_dir}/openssl.cnf"
cat >"${openssl_config}" <<EOF
[ ca ]
default_ca = scratchbird_ca

[ scratchbird_ca ]
dir = ${pki_dir}
database = \$dir/index.txt
new_certs_dir = \$dir/newcerts
certificate = \$dir/ca.crt
private_key = \$dir/ca.key
serial = \$dir/serial
crlnumber = \$dir/crlnumber
default_days = 30
default_crl_days = 30
default_md = sha256
policy = permissive
copy_extensions = copy
unique_subject = no

[ permissive ]
commonName = supplied

[ server_cert ]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @server_names

[ server_names ]
DNS.1 = mtls.scratchbird.test
DNS.2 = ldap.scratchbird.test
DNS.3 = localhost
IP.1 = 127.0.0.1

[ client_cert ]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = clientAuth
subjectAltName = URI:spiffe://scratchbird.test/user/alice

[ wrong_san_cert ]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = clientAuth
subjectAltName = URI:spiffe://untrusted.example/user/alice

[ wrong_eku_cert ]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
EOF

openssl req -x509 -newkey rsa:3072 -nodes -days 30 -sha256 \
  -subj '/CN=ScratchBird Authentication Lab Root CA' \
  -keyout "${pki_dir}/ca.key" -out "${pki_dir}/ca.crt" >/dev/null 2>&1

issue_certificate() {
  local name="$1"
  local common_name="$2"
  local extension="$3"
  openssl req -newkey rsa:2048 -nodes -sha256 -subj "/CN=${common_name}" \
    -keyout "${pki_dir}/${name}.key" -out "${pki_dir}/${name}.csr" >/dev/null 2>&1
  openssl ca -batch -config "${openssl_config}" -extensions "${extension}" \
    -in "${pki_dir}/${name}.csr" -out "${pki_dir}/${name}.crt" >/dev/null 2>&1
}

issue_certificate mtls mtls.scratchbird.test server_cert
cp "${pki_dir}/mtls.crt" "${pki_dir}/ldap.crt"
cp "${pki_dir}/mtls.key" "${pki_dir}/ldap.key"
issue_certificate alice-client alice@scratchbird.test client_cert
issue_certificate revoked-client revoked@scratchbird.test client_cert
issue_certificate wrong-eku-client wrong-eku@scratchbird.test wrong_eku_cert
issue_certificate wrong-san-client wrong-san@scratchbird.test wrong_san_cert

openssl ca -batch -config "${openssl_config}" \
  -revoke "${pki_dir}/revoked-client.crt" >/dev/null 2>&1
openssl ca -batch -config "${openssl_config}" \
  -gencrl -out "${pki_dir}/ca.crl.pem" >/dev/null 2>&1

openssl req -x509 -newkey rsa:2048 -nodes -days 30 -sha256 \
  -subj '/CN=Untrusted ScratchBird Client' \
  -keyout "${pki_dir}/untrusted-client.key" \
  -out "${pki_dir}/untrusted-client.crt" >/dev/null 2>&1

rm -f "${pki_dir}"/*.csr
chmod 0644 "${pki_dir}"/*.crt "${pki_dir}"/*.key "${pki_dir}"/*.pem
