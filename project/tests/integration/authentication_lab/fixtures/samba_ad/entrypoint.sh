#!/usr/bin/env bash
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

realm="${SAMBA_REALM:-SCRATCHBIRD.TEST}"
domain="${SAMBA_DOMAIN:-SCRATCHBIRD}"
admin_password="${SAMBA_ADMIN_PASSWORD:-ScratchBird-Admin-Password1!}"

if [[ ! -f /var/lib/samba/private/sam.ldb ]]; then
  rm -f /etc/samba/smb.conf
  samba-tool domain provision \
    --server-role=dc \
    --use-rfc2307 \
    --realm="${realm}" \
    --domain="${domain}" \
    --dns-backend=SAMBA_INTERNAL \
    --adminpass="${admin_password}"

  samba-tool user create alice 'Alice-Password1!'
  samba-tool user create admin-alice 'Admin-Alice-Password1!'
  samba-tool user create disabled-alice 'Disabled-Alice-Password1!'
  samba-tool user disable disabled-alice
  samba-tool group add database-users
  samba-tool group add database-admins
  samba-tool group add analytics
  samba-tool group add nested-groups
  samba-tool group addmembers database-users alice,admin-alice
  samba-tool group addmembers database-admins admin-alice
  samba-tool group addmembers analytics alice
  samba-tool group addmembers nested-groups analytics
fi

cat >/etc/krb5.conf <<EOF
[libdefaults]
  default_realm = ${realm}
  dns_lookup_realm = false
  dns_lookup_kdc = true
  rdns = false

[realms]
  ${realm} = {
    kdc = dc1.scratchbird.test
    admin_server = dc1.scratchbird.test
  }
EOF

exec samba --interactive --debug-stdout --no-process-group
