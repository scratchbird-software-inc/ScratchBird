#!/bin/sh
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

# ScratchBird macOS system-package lifecycle helper.
#
# The package installer must call this helper with an explicit lifecycle action.
# It never infers or admits an interactive user to the ScratchBird service
# group. Root is the sole create-time OS authorization gate; the service user
# and group exist only for ownership and process execution.

set -eu

SERVICE_USER=scratchbird
SERVICE_GROUP=scratchbird
SERVICE_HOME=/var/lib/scratchbird
NON_LOGIN_SHELL=/usr/bin/false
SERVICE_UID_MIN=501
SERVICE_UID_MAX_EXCLUSIVE=60000

ACTION=${1:-}
[ -n "$ACTION" ] || {
    printf '%s\n' BOOTSTRAP.INSTALL_DEFAULTS_INVALID >&2
    exit 2
}
shift

install_root=/
identity_mode=system
package_version=unknown
package_format=pkg

diagnostic() {
    printf '%s\n' "$1" >&2
}

fail() {
    diagnostic "$1"
    exit "${2:-1}"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --root)
            [ "$#" -ge 2 ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID 2
            install_root=$2
            shift 2
            ;;
        --identity-mode)
            [ "$#" -ge 2 ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID 2
            identity_mode=$2
            shift 2
            ;;
        --package-version)
            [ "$#" -ge 2 ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID 2
            package_version=$2
            shift 2
            ;;
        --package-format)
            [ "$#" -ge 2 ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID 2
            package_format=$2
            shift 2
            ;;
        *)
            fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID 2
            ;;
    esac
done

case "$install_root" in
    /*) ;;
    *) fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID 2 ;;
esac
case "$install_root" in
    *'/../'*|*'/..'|*'/./'*|*'/.'|*'//'*|'')
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID 2
        ;;
esac
case "$identity_mode" in
    system|fixture) ;;
    *) fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID 2 ;;
esac
if [ "$identity_mode" = fixture ] && [ "$install_root" = / ]; then
    fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID 2
fi
if [ "$identity_mode" = system ] && [ "$install_root" != / ]; then
    fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID 2
fi
case "$package_version" in
    ''|*[!ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+~-]*)
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID 2
        ;;
esac
case "$package_format" in
    ''|*[!ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-]*)
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID 2
        ;;
esac
root_path() {
    if [ "$install_root" = / ]; then
        printf '%s\n' "$1"
    else
        printf '%s%s\n' "$install_root" "$1"
    fi
}

require_system_authority() {
    [ "$(id -u)" -eq 0 ] || fail BOOTSTRAP.OS_AUTHORITY_DENIED
    [ "$(uname -s)" = Darwin ] || fail BOOTSTRAP.OS_AUTHORITY_DENIED
}

dscl_value() {
    record=$1
    property=$2
    dscl . -read "$record" "$property" 2>/dev/null | awk 'NR == 1 { print $2 }'
}

numeric_identity_is_unique() {
    record_class=$1
    property=$2
    expected=$3
    count=$(dscl . -list "$record_class" "$property" 2>/dev/null | \
        awk -v value="$expected" '$2 == value { count++ } END { print count + 0 }')
    [ "$count" -eq 1 ]
}

local_group_has_member() {
    group=$1
    expected=$2
    membership=$(dscl . -read "/Groups/$group" GroupMembership 2>/dev/null || true)
    for member in $membership; do
        [ "$member" = 'GroupMembership:' ] && continue
        [ "$member" = "$expected" ] && return 0
    done
    return 1
}

local_group_has_guid_member() {
    group=$1
    expected=$2
    members=$(dscl . -read "/Groups/$group" GroupMembers 2>/dev/null || true)
    for member in $members; do
        [ "$member" = 'GroupMembers:' ] && continue
        [ "$member" = "$expected" ] && return 0
    done
    return 1
}

local_group_nests_group_guid() {
    group=$1
    expected=$2
    nested_groups=$(dscl . -read "/Groups/$group" NestedGroups 2>/dev/null || true)
    for nested_group in $nested_groups; do
        [ "$nested_group" = 'NestedGroups:' ] && continue
        [ "$nested_group" = "$expected" ] && return 0
    done
    return 1
}

ensure_service_has_no_explicit_supplementary_membership() {
    service_group_generated_uid=$1
    service_user_generated_uid=$2
    local_groups=$(dscl . -list /Groups 2>/dev/null) || \
        fail BOOTSTRAP.GROUP_INPUT_INVALID
    printf '%s\n' "$local_groups" | while IFS= read -r candidate_group; do
        [ -n "$candidate_group" ] || continue
        [ "$candidate_group" = "$SERVICE_GROUP" ] && continue
        if local_group_has_member "$candidate_group" "$SERVICE_USER" || \
            local_group_has_guid_member "$candidate_group" \
                "$service_user_generated_uid" || \
            local_group_nests_group_guid "$candidate_group" \
                "$service_group_generated_uid"; then
            fail BOOTSTRAP.GROUP_INPUT_INVALID
        fi
    done
}

ensure_service_is_not_admin() {
    group_generated_uid=$1
    if ! dscl . -read /Groups/admin >/dev/null 2>&1; then
        fail BOOTSTRAP.GROUP_INPUT_INVALID
    fi
    if local_group_has_member admin "$SERVICE_USER"; then
        fail BOOTSTRAP.GROUP_INPUT_INVALID
    fi
    admin_nested_groups=$(dscl . -read /Groups/admin NestedGroups 2>/dev/null || true)
    for nested_group in $admin_nested_groups; do
        [ "$nested_group" = 'NestedGroups:' ] && continue
        if [ "$nested_group" = "$group_generated_uid" ]; then
            fail BOOTSTRAP.GROUP_INPUT_INVALID
        fi
    done
    effective_admin_membership=$(dseditgroup -n . -o checkmember \
        -m "$SERVICE_USER" admin 2>/dev/null) || \
        fail BOOTSTRAP.GROUP_INPUT_INVALID
    set -- $effective_admin_membership
    case "${1:-}" in
        yes) fail BOOTSTRAP.GROUP_INPUT_INVALID ;;
        no) ;;
        *) fail BOOTSTRAP.GROUP_INPUT_INVALID ;;
    esac
}

ensure_service_resolved_group_set_is_least_authority() {
    required_gid=$1
    resolved_group_ids=$(id -G "$SERVICE_USER" 2>/dev/null) || \
        fail BOOTSTRAP.GROUP_INPUT_INVALID
    required_group_seen=false
    for effective_gid in $resolved_group_ids; do
        case "$effective_gid" in
            ''|*[!0123456789]*) fail BOOTSTRAP.GROUP_INPUT_INVALID ;;
        esac
        case "$effective_gid" in
            "$required_gid") required_group_seen=true ;;
            12|61) ;;
            *) fail BOOTSTRAP.GROUP_INPUT_INVALID ;;
        esac
    done
    [ "$required_group_seen" = true ] || fail BOOTSTRAP.GROUP_INPUT_INVALID
}

create_service_user() {
    group_gid=$1
    next_uid=$(dscl . -list /Users UniqueID 2>/dev/null | awk \
        -v minimum="$SERVICE_UID_MIN" \
        -v maximum="$SERVICE_UID_MAX_EXCLUSIVE" '
        $2 ~ /^[0-9]+$/ && $2 >= minimum && $2 < maximum {
            used[$2] = 1
        }
        END {
            for (candidate = minimum; candidate < maximum; candidate++) {
                if (!(candidate in used)) {
                    print candidate
                    exit
                }
            }
        }
    ')
    case "$next_uid" in
        ''|*[!0123456789]*) fail BOOTSTRAP.GROUP_CREATE_FAILED ;;
    esac
    [ "$next_uid" -ge "$SERVICE_UID_MIN" ] || fail BOOTSTRAP.GROUP_CREATE_FAILED
    [ "$next_uid" -lt "$SERVICE_UID_MAX_EXCLUSIVE" ] || \
        fail BOOTSTRAP.GROUP_CREATE_FAILED

    if ! {
        dscl . -create "/Users/$SERVICE_USER" >/dev/null 2>&1 &&
        dscl . -create "/Users/$SERVICE_USER" RealName 'ScratchBird service account' >/dev/null 2>&1 &&
        dscl . -create "/Users/$SERVICE_USER" UniqueID "$next_uid" >/dev/null 2>&1 &&
        dscl . -create "/Users/$SERVICE_USER" PrimaryGroupID "$group_gid" >/dev/null 2>&1 &&
        dscl . -create "/Users/$SERVICE_USER" NFSHomeDirectory "$SERVICE_HOME" >/dev/null 2>&1 &&
        dscl . -create "/Users/$SERVICE_USER" UserShell "$NON_LOGIN_SHELL" >/dev/null 2>&1 &&
        dscl . -create "/Users/$SERVICE_USER" IsHidden 1 >/dev/null 2>&1 &&
        dscl . -create "/Users/$SERVICE_USER" Password '*' >/dev/null 2>&1
    }; then
        dscl . -delete "/Users/$SERVICE_USER" >/dev/null 2>&1 || true
        fail BOOTSTRAP.GROUP_CREATE_FAILED
    fi
}

ensure_service_identity() {
    require_system_authority
    command -v dscl >/dev/null 2>&1 || fail BOOTSTRAP.GROUP_CREATE_FAILED
    command -v dseditgroup >/dev/null 2>&1 || fail BOOTSTRAP.GROUP_CREATE_FAILED
    [ -x "$NON_LOGIN_SHELL" ] || fail BOOTSTRAP.GROUP_INPUT_INVALID

    if ! dscl . -read "/Groups/$SERVICE_GROUP" >/dev/null 2>&1; then
        dseditgroup -o create "$SERVICE_GROUP" >/dev/null 2>&1 || \
            fail BOOTSTRAP.GROUP_CREATE_FAILED
    fi
    group_gid=$(dscl_value "/Groups/$SERVICE_GROUP" PrimaryGroupID)
    case "$group_gid" in
        ''|*[!0123456789]*) fail BOOTSTRAP.GROUP_INPUT_INVALID ;;
    esac
    [ "$group_gid" -ne 0 ] || fail BOOTSTRAP.GROUP_INPUT_INVALID
    numeric_identity_is_unique /Groups PrimaryGroupID "$group_gid" || \
        fail BOOTSTRAP.GROUP_INPUT_INVALID
    group_generated_uid=$(dscl_value "/Groups/$SERVICE_GROUP" GeneratedUID)
    case "$group_generated_uid" in
        ''|*[!0123456789ABCDEFabcdef-]*) fail BOOTSTRAP.GROUP_INPUT_INVALID ;;
    esac

    if ! dscl . -read "/Users/$SERVICE_USER" >/dev/null 2>&1; then
        create_service_user "$group_gid"
    fi
    user_uid=$(dscl_value "/Users/$SERVICE_USER" UniqueID)
    user_gid=$(dscl_value "/Users/$SERVICE_USER" PrimaryGroupID)
    user_home=$(dscl_value "/Users/$SERVICE_USER" NFSHomeDirectory)
    user_shell=$(dscl_value "/Users/$SERVICE_USER" UserShell)
    user_hidden=$(dscl_value "/Users/$SERVICE_USER" IsHidden)
    user_password=$(dscl_value "/Users/$SERVICE_USER" Password)
    user_generated_uid=$(dscl_value "/Users/$SERVICE_USER" GeneratedUID)
    case "$user_uid" in
        ''|*[!0123456789]*) fail BOOTSTRAP.GROUP_INPUT_INVALID ;;
    esac
    case "$user_gid" in
        ''|*[!0123456789]*) fail BOOTSTRAP.GROUP_INPUT_INVALID ;;
    esac
    case "$user_generated_uid" in
        ''|*[!0123456789ABCDEFabcdef-]*) fail BOOTSTRAP.GROUP_INPUT_INVALID ;;
    esac
    [ "$user_uid" -ne 0 ] || fail BOOTSTRAP.GROUP_INPUT_INVALID
    [ "$user_uid" -ge "$SERVICE_UID_MIN" ] || fail BOOTSTRAP.GROUP_INPUT_INVALID
    [ "$user_uid" -lt "$SERVICE_UID_MAX_EXCLUSIVE" ] || \
        fail BOOTSTRAP.GROUP_INPUT_INVALID
    [ "$user_gid" = "$group_gid" ] || fail BOOTSTRAP.GROUP_INPUT_INVALID
    [ "$user_home" = "$SERVICE_HOME" ] || fail BOOTSTRAP.GROUP_INPUT_INVALID
    [ "$user_shell" = "$NON_LOGIN_SHELL" ] || fail BOOTSTRAP.GROUP_INPUT_INVALID
    [ "$user_hidden" = 1 ] || fail BOOTSTRAP.GROUP_INPUT_INVALID
    case "$user_password" in
        \**) ;;
        *) fail BOOTSTRAP.GROUP_INPUT_INVALID ;;
    esac
    numeric_identity_is_unique /Users UniqueID "$user_uid" || \
        fail BOOTSTRAP.GROUP_INPUT_INVALID

    if ! local_group_has_member "$SERVICE_GROUP" "$SERVICE_USER"; then
        dseditgroup -o edit -a "$SERVICE_USER" -t user "$SERVICE_GROUP" \
            >/dev/null 2>&1 || fail BOOTSTRAP.GROUP_CREATE_FAILED
    fi
    local_group_has_member "$SERVICE_GROUP" "$SERVICE_USER" || \
        fail BOOTSTRAP.GROUP_INPUT_INVALID
    ensure_service_has_no_explicit_supplementary_membership \
        "$group_generated_uid" "$user_generated_uid"
    ensure_service_is_not_admin "$group_generated_uid"
    ensure_service_resolved_group_set_is_least_authority "$group_gid"
}

make_directory() {
    mode=$1
    owner=$2
    group=$3
    path=$(root_path "$4")
    [ ! -L "$path" ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    if [ "$identity_mode" = fixture ]; then
        install -d -m "$mode" "$path" >/dev/null 2>&1 || \
            fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    else
        install -d -m "$mode" -o "$owner" -g "$group" "$path" \
            >/dev/null 2>&1 || \
            fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
        actual=$(stat -f '%Su:%Sg:%Lp' "$path" 2>/dev/null || true)
        expected_mode=${mode#0}
        [ "$actual" = "$owner:$group:$expected_mode" ] || \
            fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    fi
}

configure_directories() {
    make_directory 0750 root "$SERVICE_GROUP" "/Library/Application Support/ScratchBird"
    make_directory 0750 root "$SERVICE_GROUP" /var/lib/scratchbird
    make_directory 0750 "$SERVICE_USER" "$SERVICE_GROUP" /var/lib/scratchbird/data
    make_directory 0750 root "$SERVICE_GROUP" /var/lib/scratchbird/install
    make_directory 0750 "$SERVICE_USER" "$SERVICE_GROUP" /var/log/scratchbird
    make_directory 0750 "$SERVICE_USER" "$SERVICE_GROUP" /var/run/scratchbird
    make_directory 0750 "$SERVICE_USER" "$SERVICE_GROUP" /var/run/scratchbird/sb_server
    make_directory 0750 "$SERVICE_USER" "$SERVICE_GROUP" /var/run/scratchbird/sb_server/control
    make_directory 0750 "$SERVICE_USER" "$SERVICE_GROUP" /var/run/scratchbird/listener
    make_directory 0750 "$SERVICE_USER" "$SERVICE_GROUP" /var/run/scratchbird/listener/control
    make_directory 0750 "$SERVICE_USER" "$SERVICE_GROUP" /var/run/scratchbird/listener/runtime
    make_directory 0750 "$SERVICE_USER" "$SERVICE_GROUP" /var/run/scratchbird/manager
    make_directory 0750 "$SERVICE_USER" "$SERVICE_GROUP" /var/run/scratchbird/manager/control
    make_directory 0750 "$SERVICE_USER" "$SERVICE_GROUP" /var/run/scratchbird/manager/runtime
}

install_default_configuration() {
    defaults_root=$(root_path /opt/ScratchBird/share/scratchbird/config-defaults)
    config_root=$(root_path "/Library/Application Support/ScratchBird")
    [ -d "$defaults_root" ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    [ ! -L "$defaults_root" ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    [ ! -L "$config_root" ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID

    found_default=false
    for source in "$defaults_root"/*; do
        [ -f "$source" ] || continue
        [ ! -L "$source" ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
        name=${source##*/}
        case "$name" in
            *[!ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-]*)
                fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
                ;;
        esac
        found_default=true
        target=$config_root/$name
        [ ! -L "$target" ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
        if [ -e "$target" ] && [ ! -f "$target" ]; then
            fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
        fi
        if [ ! -e "$target" ]; then
            cp "$source" "$target" >/dev/null 2>&1 || \
                fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
        fi
    done
    [ "$found_default" = true ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    if find "$config_root" -xdev -type l -print -quit | grep -q .; then
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    fi

    if [ "$identity_mode" = fixture ]; then
        find "$config_root" -xdev -type d -exec chmod 0750 {} +
        find "$config_root" -xdev -type f -exec chmod 0640 {} +
    else
        find "$config_root" -xdev -type d -exec chown root:"$SERVICE_GROUP" {} +
        find "$config_root" -xdev -type d -exec chmod 0750 {} +
        find "$config_root" -xdev -type f -exec chown root:"$SERVICE_GROUP" {} +
        find "$config_root" -xdev -type f -exec chmod 0640 {} +
    fi
}

write_fixture_identity_evidence() {
    [ "$identity_mode" = fixture ] || return
    fixture_root=$(root_path /var/lib/scratchbird/install/fixture-identities)
    install -d -m 0750 "$fixture_root"
    umask 027
    printf '%s\n' '{"name":"scratchbird","kind":"local_group"}' \
        > "$fixture_root/group.json"
    printf '%s\n' '{"name":"scratchbird","kind":"non_login_service","hidden":true,"administrator_group_membership":false,"primary_group":"scratchbird"}' \
        > "$fixture_root/service.json"
    chmod 0640 "$fixture_root/group.json" "$fixture_root/service.json"
}

preserve_configuration() {
    config_root=$(root_path "/Library/Application Support/ScratchBird")
    preserve_root=$(root_path /var/lib/scratchbird/install/config-preserve)
    [ -d "$config_root" ] || return
    [ ! -L "$config_root" ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    [ ! -L "$preserve_root" ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    rm -rf -- "$preserve_root"
    if [ "$identity_mode" = fixture ]; then
        install -d -m 0700 "$preserve_root"
    else
        require_system_authority
        install -d -m 0700 -o root -g "$SERVICE_GROUP" "$preserve_root"
    fi
    cp -pR "$config_root"/. "$preserve_root"/ || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    find "$preserve_root" -xdev -type d -exec chmod 0700 {} +
    find "$preserve_root" -xdev -type f -exec chmod 0600 {} +
    if [ "$identity_mode" = system ]; then
        find "$preserve_root" -xdev -type d -exec chown root:"$SERVICE_GROUP" {} +
        find "$preserve_root" -xdev -type f -exec chown root:"$SERVICE_GROUP" {} +
    fi
}

write_install_evidence() {
    evidence_dir=$(root_path /var/lib/scratchbird/install)
    evidence_file=$evidence_dir/MACOS_SYSTEM_INSTALL_STATE.json
    evidence_temp=$evidence_dir/.MACOS_SYSTEM_INSTALL_STATE.json.$$
    timestamp=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
    umask 027
    {
        printf '{\n'
        printf '  "schema_id": "scratchbird.macos_system_install_state.v1",\n'
        printf '  "installed_at_utc": "%s",\n' "$timestamp"
        printf '  "package_format": "%s",\n' "$package_format"
        printf '  "package_version": "%s",\n' "$package_version"
        printf '  "service_user": "scratchbird",\n'
        printf '  "service_group": "scratchbird",\n'
        printf '  "service_uid_policy": "locally_unique_501_through_59999",\n'
        printf '  "service_administrator_group_membership": false,\n'
        printf '  "service_authority_scope": "filesystem_directory_and_process_execution_only_no_database_or_security_authority",\n'
        printf '  "resolved_effective_group_policy": "primary_scratchbird_plus_macos_implicit_gid_12_and_61_only",\n'
        printf '  "human_service_group_membership_mutated": false,\n'
        printf '  "create_time_os_authorization": "root_only",\n'
        printf '  "service_enablement_default": "disabled",\n'
        printf '  "service_activity_default": "not_started",\n'
        printf '  "launchd_load_performed": false,\n'
        printf '  "native_default_port": 3092,\n'
        printf '  "database_files_created": false,\n'
        printf '  "security_sidecars_created": false\n'
        printf '}\n'
    } > "$evidence_temp" || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    chmod 0640 "$evidence_temp"
    if [ "$identity_mode" = system ]; then
        chown root:"$SERVICE_GROUP" "$evidence_temp"
    fi
    mv -f "$evidence_temp" "$evidence_file"
}

case "$ACTION" in
    post-install)
        if [ "$identity_mode" = system ]; then
            ensure_service_identity
        fi
        configure_directories
        install_default_configuration
        write_fixture_identity_evidence
        write_install_evidence
        ;;
    pre-remove)
        if [ "$identity_mode" = system ]; then
            require_system_authority
        fi
        preserve_configuration
        ;;
    *)
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID 2
        ;;
esac

exit 0
