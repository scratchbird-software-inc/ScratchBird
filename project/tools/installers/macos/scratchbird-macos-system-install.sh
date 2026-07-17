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
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH

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
legacy_log_default_migrations=0

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

ensure_services_not_loaded() {
    [ "$identity_mode" = system ] || return 0
    require_system_authority
    [ -x /bin/launchctl ] || fail BOOTSTRAP.SERVICE_STATE_INVALID
    /bin/launchctl print system >/dev/null 2>&1 || \
        fail BOOTSTRAP.SERVICE_STATE_INVALID
    for label in com.scratchbird.sbsrv com.scratchbird.sbmgr; do
        if /bin/launchctl print "system/$label" >/dev/null 2>&1; then
            fail BOOTSTRAP.SERVICE_STATE_INVALID
        else
            launchctl_status=$?
            [ "$launchctl_status" -eq 113 ] || \
                fail BOOTSTRAP.SERVICE_STATE_INVALID
        fi
    done
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

ensure_service_group_membership_is_exact() {
    service_user_generated_uid=$1

    membership=$(dscl . -read "/Groups/$SERVICE_GROUP" GroupMembership \
        2>/dev/null || true)
    membership_count=0
    for member in $membership; do
        [ "$member" = 'GroupMembership:' ] && continue
        [ "$member" = "$SERVICE_USER" ] || fail BOOTSTRAP.GROUP_INPUT_INVALID
        membership_count=$((membership_count + 1))
    done
    [ "$membership_count" -eq 1 ] || fail BOOTSTRAP.GROUP_INPUT_INVALID

    guid_membership=$(dscl . -read "/Groups/$SERVICE_GROUP" GroupMembers \
        2>/dev/null || true)
    guid_membership_count=0
    for member_guid in $guid_membership; do
        [ "$member_guid" = 'GroupMembers:' ] && continue
        [ "$member_guid" = "$service_user_generated_uid" ] || \
            fail BOOTSTRAP.GROUP_INPUT_INVALID
        guid_membership_count=$((guid_membership_count + 1))
    done
    [ "$guid_membership_count" -eq 1 ] || \
        fail BOOTSTRAP.GROUP_INPUT_INVALID

    nested_groups=$(dscl . -read "/Groups/$SERVICE_GROUP" NestedGroups \
        2>/dev/null || true)
    for nested_group in $nested_groups; do
        [ "$nested_group" = 'NestedGroups:' ] && continue
        fail BOOTSTRAP.GROUP_INPUT_INVALID
    done
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
    # checkmember reports the required non-member result with a nonzero status
    # on current macOS releases.  Its exact code-only answer remains the
    # authority; command failure or an unrecognized answer still fails closed.
    effective_admin_membership=$(
        dseditgroup -n . -o checkmember \
            -m "$SERVICE_USER" admin 2>/dev/null || true
    )
    set -- $effective_admin_membership
    case "${1:-}" in
        yes) fail BOOTSTRAP.GROUP_INPUT_INVALID ;;
        no) ;;
        *) fail BOOTSTRAP.GROUP_INPUT_INVALID ;;
    esac
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
    user_record=$(dscl . -read "/Users/$SERVICE_USER" 2>/dev/null) || \
        fail BOOTSTRAP.GROUP_INPUT_INVALID
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
    [ "$user_password" = '*' ] || fail BOOTSTRAP.GROUP_INPUT_INVALID
    if printf '%s\n' "$user_record" | \
        grep -Eq '^(AuthenticationAuthority|ShadowHashData|dsAttrType(Native|Standard):(AuthenticationAuthority|ShadowHashData)):'; then
        fail BOOTSTRAP.GROUP_INPUT_INVALID
    fi
    numeric_identity_is_unique /Users UniqueID "$user_uid" || \
        fail BOOTSTRAP.GROUP_INPUT_INVALID

    if ! local_group_has_member "$SERVICE_GROUP" "$SERVICE_USER"; then
        dseditgroup -o edit -a "$SERVICE_USER" -t user "$SERVICE_GROUP" \
            >/dev/null 2>&1 || fail BOOTSTRAP.GROUP_CREATE_FAILED
    fi
    local_group_has_member "$SERVICE_GROUP" "$SERVICE_USER" || \
        fail BOOTSTRAP.GROUP_INPUT_INVALID
    ensure_service_group_membership_is_exact "$user_generated_uid"
    ensure_service_has_no_explicit_supplementary_membership \
        "$group_generated_uid" "$user_generated_uid"
    ensure_service_is_not_admin "$group_generated_uid"
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

make_regular_file() {
    mode=$1
    owner=$2
    group=$3
    path=$(root_path "$4")
    [ ! -L "$path" ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    if [ -e "$path" ] && [ ! -f "$path" ]; then
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    fi
    if [ "$identity_mode" = system ] && [ -e "$path" ]; then
        link_count=$(stat -f '%l' "$path" 2>/dev/null || true)
        [ "$link_count" = 1 ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    fi
    if [ ! -e "$path" ]; then
        if [ "$identity_mode" = fixture ]; then
            install -m "$mode" /dev/null "$path" >/dev/null 2>&1 || \
                fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
        else
            install -m "$mode" -o "$owner" -g "$group" /dev/null "$path" \
                >/dev/null 2>&1 || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
        fi
    elif [ "$identity_mode" = fixture ]; then
        chmod "$mode" "$path" >/dev/null 2>&1 || \
            fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    else
        chown "$owner:$group" "$path" >/dev/null 2>&1 || \
            fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
        chmod "$mode" "$path" >/dev/null 2>&1 || \
            fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    fi
    if [ "$identity_mode" = system ]; then
        actual=$(stat -f '%Su:%Sg:%Lp' "$path" 2>/dev/null || true)
        link_count=$(stat -f '%l' "$path" 2>/dev/null || true)
        expected_mode=${mode#0}
        [ "$actual" = "$owner:$group:$expected_mode" ] || \
            fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
        [ "$link_count" = 1 ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    fi
}

remove_extended_acl() {
    expected_owner=$1
    expected_group=$2
    mode=$3
    path=$(root_path "$4")
    [ "$identity_mode" = system ] || return 0
    [ ! -L "$path" ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    chmod -N "$path" >/dev/null 2>&1 || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    acl_mode=$(ls -lde "$path" 2>/dev/null | awk 'NR == 1 { print $1 }')
    case "$acl_mode" in
        ''|*+*) fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID ;;
    esac
    actual=$(stat -f '%Su:%Sg:%Lp' "$path" 2>/dev/null || true)
    expected_mode=${mode#0}
    [ "$actual" = "$expected_owner:$expected_group:$expected_mode" ] || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
}

validate_root_service_launcher_path() {
    [ "$identity_mode" = system ] || return 0
    require_system_authority
    [ -d /opt ] && [ ! -L /opt ] || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    opt_authority=$(stat -f '%Su:%Sg:%Lp' /opt 2>/dev/null || true)
    [ "$opt_authority" = root:wheel:755 ] || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    opt_acl_mode=$(ls -lde /opt 2>/dev/null | awk 'NR == 1 { print $1 }')
    case "$opt_acl_mode" in
        ''|*+*) fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID ;;
    esac

    for directory in /opt/ScratchBird /opt/ScratchBird/bin; do
        [ -d "$directory" ] && [ ! -L "$directory" ] || \
            fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
        chown root:wheel "$directory" >/dev/null 2>&1 || \
            fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
        chmod 0755 "$directory" >/dev/null 2>&1 || \
            fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
        remove_extended_acl root wheel 0755 "$directory"
    done

    launcher=/opt/ScratchBird/bin/SBlaunch
    [ -f "$launcher" ] && [ ! -L "$launcher" ] || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    launcher_links=$(stat -f '%l' "$launcher" 2>/dev/null || true)
    [ "$launcher_links" = 1 ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    chown root:wheel "$launcher" >/dev/null 2>&1 || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    chmod 0755 "$launcher" >/dev/null 2>&1 || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    remove_extended_acl root wheel 0755 "$launcher"
}

validate_launchd_service_definition() {
    label=$1
    selector=$2
    stdout_path=$3
    stderr_path=$4
    plist=/Library/LaunchDaemons/$label.plist
    [ -f "$plist" ] && [ ! -L "$plist" ] || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    plist_links=$(stat -f '%l' "$plist" 2>/dev/null || true)
    [ "$plist_links" = 1 ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    chown root:wheel "$plist" >/dev/null 2>&1 || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    chmod 0644 "$plist" >/dev/null 2>&1 || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    remove_extended_acl root wheel 0644 "$plist"
    /usr/bin/plutil -lint "$plist" >/dev/null 2>&1 || \
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    plist_value() {
        /usr/libexec/PlistBuddy -c "Print $1" "$plist" 2>/dev/null || \
            fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    }
    [ "$(plist_value :Label)" = "$label" ] || \
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    [ "$(plist_value :ProgramArguments:0)" = \
      /opt/ScratchBird/bin/SBlaunch ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    [ "$(plist_value :ProgramArguments:1)" = "$selector" ] || \
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    if /usr/libexec/PlistBuddy -c 'Print :ProgramArguments:2' "$plist" \
        >/dev/null 2>&1 || \
       /usr/libexec/PlistBuddy -c 'Print :Program' "$plist" \
        >/dev/null 2>&1; then
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    fi
    [ "$(plist_value :UserName)" = root ] && \
    [ "$(plist_value :GroupName)" = wheel ] && \
    [ "$(plist_value :InitGroups)" = false ] && \
    [ "$(plist_value :RunAtLoad)" = false ] && \
    [ "$(plist_value :KeepAlive)" = false ] && \
    [ "$(plist_value :Disabled)" = true ] && \
    [ "$(plist_value :WorkingDirectory)" = /var/lib/scratchbird ] && \
    [ "$(plist_value :StandardOutPath)" = "$stdout_path" ] && \
    [ "$(plist_value :StandardErrorPath)" = "$stderr_path" ] || \
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
}

validate_launchd_service_definitions() {
    [ "$identity_mode" = system ] || return 0
    launchd_root=/Library/LaunchDaemons
    [ -d "$launchd_root" ] && [ ! -L "$launchd_root" ] || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    launchd_root_authority=$(stat -f '%Su:%Sg:%Lp' "$launchd_root" \
        2>/dev/null || true)
    [ "$launchd_root_authority" = root:wheel:755 ] || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    launchd_root_acl=$(ls -lde "$launchd_root" 2>/dev/null | \
        awk 'NR == 1 { print $1 }')
    case "$launchd_root_acl" in
        ''|*+*) fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID ;;
    esac
    validate_launchd_service_definition \
        com.scratchbird.sbsrv sbsrv \
        /var/log/scratchbird/launchd/SBsrv.out.log \
        /var/log/scratchbird/launchd/SBsrv.err.log
    validate_launchd_service_definition \
        com.scratchbird.sbmgr sbmgr \
        /var/log/scratchbird/launchd/SBmgr.out.log \
        /var/log/scratchbird/launchd/SBmgr.err.log
}

configure_directories() {
    make_directory 0750 root "$SERVICE_GROUP" "/Library/Application Support/ScratchBird"
    make_directory 0750 root "$SERVICE_GROUP" /var/lib/scratchbird
    make_directory 0750 "$SERVICE_USER" "$SERVICE_GROUP" /var/lib/scratchbird/data
    make_directory 0750 root "$SERVICE_GROUP" /var/lib/scratchbird/install
    # launchd opens StandardOutPath/StandardErrorPath while the tiny launcher
    # still has root authority. Keep those paths in a non-writable directory
    # and pre-create them as regular files. Application logs live separately
    # so the low-privilege service can perform its rename/recreate rotation.
    make_directory 0750 root "$SERVICE_GROUP" /var/log/scratchbird
    remove_extended_acl root "$SERVICE_GROUP" 0750 /var/log/scratchbird
    make_directory 0750 root "$SERVICE_GROUP" /var/log/scratchbird/launchd
    remove_extended_acl root "$SERVICE_GROUP" 0750 \
        /var/log/scratchbird/launchd
    make_directory 0750 "$SERVICE_USER" "$SERVICE_GROUP" \
        /var/log/scratchbird/runtime
    remove_extended_acl "$SERVICE_USER" "$SERVICE_GROUP" 0750 \
        /var/log/scratchbird/runtime
    make_regular_file 0640 root "$SERVICE_GROUP" \
        /var/log/scratchbird/launchd/SBsrv.out.log
    make_regular_file 0640 root "$SERVICE_GROUP" \
        /var/log/scratchbird/launchd/SBsrv.err.log
    make_regular_file 0640 root "$SERVICE_GROUP" \
        /var/log/scratchbird/launchd/SBmgr.out.log
    make_regular_file 0640 root "$SERVICE_GROUP" \
        /var/log/scratchbird/launchd/SBmgr.err.log
    remove_extended_acl root "$SERVICE_GROUP" 0640 \
        /var/log/scratchbird/launchd/SBsrv.out.log
    remove_extended_acl root "$SERVICE_GROUP" 0640 \
        /var/log/scratchbird/launchd/SBsrv.err.log
    remove_extended_acl root "$SERVICE_GROUP" 0640 \
        /var/log/scratchbird/launchd/SBmgr.out.log
    remove_extended_acl root "$SERVICE_GROUP" 0640 \
        /var/log/scratchbird/launchd/SBmgr.err.log
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

validate_configuration_topology() {
    config_root=$1
    [ -d "$config_root" ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    [ ! -L "$config_root" ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    if find "$config_root" -xdev -type l -print -quit | grep -q .; then
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    fi
    if find "$config_root" -xdev -type f -links +1 -print -quit | grep -q .; then
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    fi
    if find "$config_root" -xdev ! -type d ! -type f -print -quit | grep -q .; then
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    fi
}

install_default_configuration() {
    defaults_root=$(root_path /opt/ScratchBird/share/scratchbird/config-defaults)
    config_root=$(root_path "/Library/Application Support/ScratchBird")
    [ -d "$defaults_root" ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    [ ! -L "$defaults_root" ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    # Reject links before any recursive ownership/mode normalization so an
    # upgrade can never mutate an inode outside the canonical config tree.
    validate_configuration_topology "$config_root"

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

legacy_config_requires_migration() {
    target=$1
    legacy_line=$2
    replacement_line=$3
    assignment_key=$4
    assignment_mode=$5
    [ -f "$target" ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    [ ! -L "$target" ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    if [ "$(uname -s)" = Darwin ]; then
        link_count=$(stat -f '%l' "$target" 2>/dev/null || true)
    else
        link_count=$(stat -c '%h' "$target" 2>/dev/null || true)
    fi
    [ "$link_count" = 1 ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    legacy_count=$(grep -Fxc "$legacy_line" "$target" 2>/dev/null || true)
    replacement_count=$(grep -Fxc "$replacement_line" "$target" \
        2>/dev/null || true)
    root_log_assignments=$(LC_ALL=C awk \
        -v expected_key="$assignment_key" -v mode="$assignment_mode" '
        function trim(value) {
            sub(/^[ \t]+/, "", value)
            sub(/[ \t]+$/, "", value)
            return value
        }
        {
            line = $0
            equals = index(line, "=")
            if (equals == 0) next
            key = trim(substr(line, 1, equals - 1))
            if (mode == "server") key = tolower(key)
            if (key != expected_key) next
            value = trim(substr(line, equals + 1))
            if (mode == "server" && substr(value, 1, 1) == "\"") {
                value = substr(value, 2)
            }
            normalized = tolower(value)
            dot_component = normalized ~ /\/\.\.?([\/" \t#;]|$)/
            if (substr(normalized, 1, 1) != "/") {
                normalized = "/opt/scratchbird/" normalized
            }
            gsub(/\/+/, "/", normalized)
            previous = ""
            while (normalized != previous) {
                previous = normalized
                gsub(/\/\.\//, "/", normalized)
                sub(/\/[^\/]+\/\.\.\//, "/", normalized)
                sub(/^\/\.\.(\/|$)/, "/", normalized)
            }
            root = "/var/log/scratchbird/"
            runtime = "/var/log/scratchbird/runtime/"
            private_root = "/private/var/log/scratchbird/"
            private_runtime = "/private/var/log/scratchbird/runtime/"
            data_root = "/system/volumes/data/private/var/log/scratchbird/"
            data_runtime = "/system/volumes/data/private/var/log/scratchbird/runtime/"
            if ((index(normalized, root) == 1 &&
                 (index(normalized, runtime) != 1 || dot_component)) ||
                (index(normalized, private_root) == 1 &&
                 (index(normalized, private_runtime) != 1 || dot_component)) ||
                (index(normalized, data_root) == 1 &&
                 (index(normalized, data_runtime) != 1 || dot_component))) {
                print line
            }
        }
    ' "$target" 2>/dev/null || true)
    if [ -n "$root_log_assignments" ] && \
       [ "$root_log_assignments" != "$legacy_line" ]; then
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    fi
    case "$legacy_count:$replacement_count" in
        0:*) return 10 ;;
        1:0) ;;
        *) fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID ;;
    esac
    # Prior packaged defaults are newline-terminated. Refuse an operator file
    # with a different byte topology instead of normalizing it during rewrite.
    final_byte=$(tail -c 1 "$target" 2>/dev/null | od -An -tu1 | \
        tr -d '[:space:]')
    [ "$final_byte" = 10 ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    return 0
}

preflight_existing_configuration() {
    config_root=$(root_path "/Library/Application Support/ScratchBird")
    [ -e "$config_root" ] || return 0
    validate_configuration_topology "$config_root"
    server_target=$config_root/SBsrv.conf
    manager_target=$config_root/SBmgr.conf
    if [ -e "$server_target" ]; then
        if legacy_config_requires_migration \
            "$server_target" \
            'log_file = /var/log/scratchbird/SBsrv.log' \
            'log_file = /var/log/scratchbird/runtime/SBsrv.log' \
            log_file server; then
            :
        else
            preflight_status=$?
            [ "$preflight_status" -eq 10 ] || \
                fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
        fi
    fi
    if [ -e "$manager_target" ]; then
        if legacy_config_requires_migration \
            "$manager_target" \
            'manager.log.path = /var/log/scratchbird/SBmgr.log' \
            'manager.log.path = /var/log/scratchbird/runtime/SBmgr.log' \
            manager.log.path manager; then
            :
        else
            preflight_status=$?
            [ "$preflight_status" -eq 10 ] || \
                fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
        fi
    fi
}

stage_legacy_config_migration() {
    target=$1
    legacy_line=$2
    replacement_line=$3
    temporary=$4
    [ ! -e "$temporary" ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    umask 027
    if ! LC_ALL=C awk -v legacy="$legacy_line" \
        -v replacement="$replacement_line" \
        '{ if ($0 == legacy) { print replacement } else { print } }' \
        "$target" > "$temporary"; then
        rm -f -- "$temporary"
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    fi
    migrated_legacy_count=$(grep -Fxc "$legacy_line" "$temporary" \
        2>/dev/null || true)
    migrated_replacement_count=$(grep -Fxc "$replacement_line" "$temporary" \
        2>/dev/null || true)
    if [ "$migrated_legacy_count:$migrated_replacement_count" != 0:1 ]; then
        rm -f -- "$temporary"
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    fi
    chmod 0640 "$temporary" || {
        rm -f -- "$temporary"
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    }
    if [ "$identity_mode" = system ]; then
        chown root:"$SERVICE_GROUP" "$temporary" || {
            rm -f -- "$temporary"
            fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
        }
    fi
}

migrate_legacy_packaged_log_defaults() {
    config_root=$(root_path "/Library/Application Support/ScratchBird")
    server_target=$config_root/SBsrv.conf
    manager_target=$config_root/SBmgr.conf
    server_legacy='log_file = /var/log/scratchbird/SBsrv.log'
    server_replacement='log_file = /var/log/scratchbird/runtime/SBsrv.log'
    manager_legacy='manager.log.path = /var/log/scratchbird/SBmgr.log'
    manager_replacement='manager.log.path = /var/log/scratchbird/runtime/SBmgr.log'
    server_temporary=$config_root/.SBsrv.conf.scratchbird-log-migration.$$
    manager_temporary=$config_root/.SBmgr.conf.scratchbird-log-migration.$$
    server_migration=false
    manager_migration=false

    if legacy_config_requires_migration \
        "$server_target" "$server_legacy" "$server_replacement" \
        log_file server; then
        server_migration=true
    else
        migration_status=$?
        [ "$migration_status" -eq 10 ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    fi
    if legacy_config_requires_migration \
        "$manager_target" "$manager_legacy" "$manager_replacement" \
        manager.log.path manager; then
        manager_migration=true
    else
        migration_status=$?
        [ "$migration_status" -eq 10 ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    fi

    if [ "$server_migration" = true ]; then
        stage_legacy_config_migration "$server_target" "$server_legacy" \
            "$server_replacement" "$server_temporary"
    fi
    if [ "$manager_migration" = true ]; then
        stage_legacy_config_migration "$manager_target" "$manager_legacy" \
            "$manager_replacement" "$manager_temporary"
    fi

    if [ "$server_migration" = true ]; then
        if ! mv -f "$server_temporary" "$server_target"; then
            rm -f -- "$manager_temporary"
            fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
        fi
    fi
    if [ "$manager_migration" = true ]; then
        if ! mv -f "$manager_temporary" "$manager_target"; then
            fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
        fi
    fi

    [ "$server_migration" = false ] || \
        legacy_log_default_migrations=$((legacy_log_default_migrations + 1))
    [ "$manager_migration" = false ] || \
        legacy_log_default_migrations=$((legacy_log_default_migrations + 1))
}

write_fixture_identity_evidence() {
    [ "$identity_mode" = fixture ] || return 0
    fixture_root=$(root_path /var/lib/scratchbird/install/fixture-identities)
    install -d -m 0750 "$fixture_root"
    umask 027
    fixture_user_generated_uid=fixture-scratchbird-user-generated-uid
    printf '%s\n' '{"name":"scratchbird","kind":"local_group","group_membership":["scratchbird"],"group_members":["fixture-scratchbird-user-generated-uid"],"nested_groups":[]}' \
        > "$fixture_root/group.json"
    printf '%s\n' "{\"name\":\"scratchbird\",\"kind\":\"non_login_service\",\"hidden\":true,\"password_record\":\"literal_asterisk_lock\",\"authentication_authority_present\":false,\"shadow_hash_data_present\":false,\"administrator_group_membership\":false,\"primary_group\":\"scratchbird\",\"generated_uid\":\"$fixture_user_generated_uid\"}" \
        > "$fixture_root/service.json"
    chmod 0640 "$fixture_root/group.json" "$fixture_root/service.json"
}

preserve_configuration() {
    config_root=$(root_path "/Library/Application Support/ScratchBird")
    preserve_root=$(root_path /var/lib/scratchbird/install/config-preserve)
    [ -d "$config_root" ] || return 0
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
        printf '  "service_password_record_locked": true,\n'
        printf '  "service_authentication_authority_present": false,\n'
        printf '  "service_shadow_hash_data_present": false,\n'
        printf '  "service_administrator_group_membership": false,\n'
        printf '  "service_authority_scope": "filesystem_directory_and_process_execution_only_no_database_or_security_authority",\n'
        printf '  "service_group_membership_policy": "exact_scratchbird_name_and_generated_uid_only_no_nested_groups",\n'
        printf '  "resolved_effective_group_policy": "launchd_host_computed_groups_cleared_before_scratchbird_product_exec",\n'
        printf '  "launchd_init_groups": false,\n'
        printf '  "launchd_bootstrap_identity": "root:wheel",\n'
        printf '  "service_launcher": "/opt/ScratchBird/bin/SBlaunch",\n'
        printf '  "service_launcher_path_policy": "root_owned_nonwritable_no_extended_acl_no_symlink_single_link_launcher",\n'
        printf '  "launchd_definition_path_policy": "root_owned_0644_no_extended_acl_no_symlink_single_link_exact_fixed_selector",\n'
        printf '  "package_postinstall_helper_path_policy": "pre_exec_root_owned_0755_no_extended_acl_no_symlink_single_link_helper",\n'
        printf '  "final_product_identity": "scratchbird:scratchbird",\n'
        printf '  "final_supplementary_groups_empty": true,\n'
        printf '  "human_service_group_membership_mutated": false,\n'
        printf '  "create_time_os_authorization": "root_only",\n'
        printf '  "service_enablement_default": "disabled",\n'
        printf '  "service_activity_default": "not_started",\n'
        printf '  "launchd_load_performed": false,\n'
        printf '  "legacy_packaged_log_default_migration_policy": "exact_prior_packaged_line_only_preserve_all_other_configuration_lines",\n'
        printf '  "legacy_packaged_log_default_migrations": %s,\n' \
            "$legacy_log_default_migrations"
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
        ensure_services_not_loaded
        preflight_existing_configuration
        validate_root_service_launcher_path
        validate_launchd_service_definitions
        if [ "$identity_mode" = system ]; then
            ensure_service_identity
        fi
        configure_directories
        install_default_configuration
        migrate_legacy_packaged_log_defaults
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
