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
identity_validation_stage=
install_validation_stage=argument_validation
installer_failure_reported=false

diagnostic() {
    printf '%s\n' "$1" >&2
}

is_system_post_install() {
    [ "$ACTION" = post-install ] && \
    [ "$identity_mode" = system ] && \
    [ "$install_root" = / ]
}

is_known_install_validation_stage() {
    case "$1" in
        argument_validation|service_state_validation|service_identity_validation|\
        directory_configuration|default_configuration_content|\
        default_configuration_symlink_validation|\
        default_configuration_migration|default_configuration_permissions|\
        root_service_authority_validation|root_opt_directory_validation|\
        root_scratchbird_directory_normalization|\
        root_bin_directory_normalization|root_sblaunch_normalization|\
        root_sbsrv_normalization|root_sbmgr_normalization|\
        launchd_directory_validation|launchd_sbsrv_definition_validation|\
        launchd_sbmgr_definition_validation|install_evidence_write|\
        pre_remove_configuration_preservation)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

is_known_install_diagnostic_code() {
    case "$1" in
        BOOTSTRAP.OS_AUTHORITY_DENIED|BOOTSTRAP.SERVICE_STATE_INVALID|\
        BOOTSTRAP.GROUP_CREATE_FAILED|BOOTSTRAP.GROUP_INPUT_INVALID|\
        BOOTSTRAP.DIRECTORY_PERMISSION_INVALID|\
        BOOTSTRAP.INSTALL_DEFAULTS_INVALID|\
        BOOTSTRAP.MACOS_INSTALL_STAGE_UNCLASSIFIED_FAILURE)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

log_system_installer_diagnostic() {
    diagnostic_code=$1
    is_system_post_install || return 0
    is_known_install_diagnostic_code "$diagnostic_code" || return 0
    is_known_install_validation_stage "$install_validation_stage" || return 0
    [ -x /usr/bin/logger ] || return 0
    /usr/bin/logger -t scratchbird-installer \
        "scratchbird-installer stage=$install_validation_stage code=$diagnostic_code" \
        >/dev/null 2>&1 || true
}

report_unclassified_install_failure() {
    exit_status=$1
    trap - 0
    if [ "$exit_status" -ne 0 ] && \
       [ "$installer_failure_reported" != true ]; then
        log_system_installer_diagnostic \
            BOOTSTRAP.MACOS_INSTALL_STAGE_UNCLASSIFIED_FAILURE
    fi
    exit "$exit_status"
}

fail() {
    diagnostic "$1"
    if [ "$1" = BOOTSTRAP.GROUP_INPUT_INVALID ] && \
        [ -n "$identity_validation_stage" ]; then
        diagnostic "BOOTSTRAP.MACOS_IDENTITY_VALIDATION_STAGE=$identity_validation_stage"
    fi
    if is_system_post_install && \
       is_known_install_validation_stage "$install_validation_stage"; then
        diagnostic "BOOTSTRAP.MACOS_INSTALL_VALIDATION_STAGE=$install_validation_stage"
    fi
    log_system_installer_diagnostic "$1"
    installer_failure_reported=true
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

trap 'report_unclassified_install_failure "$?"' 0

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
    # macOS dseditgroup uses status 67 for a valid false checkmember
    # predicate.  Capture it in an if-condition so set -e does not mistake
    # the least-authority result for a directory-service failure.  Both the
    # documented false status and the complete C-locale response are required;
    # every other status or response fails closed.
    if effective_admin_membership=$(LC_ALL=C dseditgroup -n . -o checkmember \
        -m "$SERVICE_USER" admin 2>/dev/null); then
        effective_admin_membership_status=0
    else
        effective_admin_membership_status=$?
    fi
    case "$effective_admin_membership_status" in
        67) ;;
        *) fail BOOTSTRAP.GROUP_INPUT_INVALID ;;
    esac
    [ "$effective_admin_membership" = \
        "no $SERVICE_USER is NOT a member of admin" ] || \
        fail BOOTSTRAP.GROUP_INPUT_INVALID
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

    generated_uid=$(/usr/bin/uuidgen 2>/dev/null) || \
        fail BOOTSTRAP.GROUP_CREATE_FAILED
    case "$generated_uid" in
        ????????-????-????-????-????????????) ;;
        *) fail BOOTSTRAP.GROUP_CREATE_FAILED ;;
    esac
    generated_uid_hex=$(printf '%s' "$generated_uid" | tr -d '-')
    case "$generated_uid_hex" in
        ????????????????????????????????) ;;
        *) fail BOOTSTRAP.GROUP_CREATE_FAILED ;;
    esac
    case "$generated_uid_hex" in
        *[!0123456789ABCDEFabcdef]*) fail BOOTSTRAP.GROUP_CREATE_FAILED ;;
    esac

    if ! {
        dscl . -create "/Users/$SERVICE_USER" >/dev/null 2>&1 &&
        dscl . -create "/Users/$SERVICE_USER" RealName 'ScratchBird service account' >/dev/null 2>&1 &&
        dscl . -create "/Users/$SERVICE_USER" UniqueID "$next_uid" >/dev/null 2>&1 &&
        dscl . -create "/Users/$SERVICE_USER" GeneratedUID "$generated_uid" >/dev/null 2>&1 &&
        dscl . -create "/Users/$SERVICE_USER" PrimaryGroupID "$group_gid" >/dev/null 2>&1 &&
        dscl . -create "/Users/$SERVICE_USER" NFSHomeDirectory "$SERVICE_HOME" >/dev/null 2>&1 &&
        dscl . -create "/Users/$SERVICE_USER" UserShell "$NON_LOGIN_SHELL" >/dev/null 2>&1 &&
        dscl . -create "/Users/$SERVICE_USER" IsHidden 1 >/dev/null 2>&1 &&
        dscl . -create "/Users/$SERVICE_USER" Password '*' >/dev/null 2>&1
    }; then
        dscl . -delete "/Users/$SERVICE_USER" >/dev/null 2>&1 || true
        fail BOOTSTRAP.GROUP_CREATE_FAILED
    fi
    created_service_user_generated_uid=$generated_uid
}

ensure_service_identity() {
    identity_validation_stage=identity_record_validation
    created_service_user_generated_uid=
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
    if [ -n "$created_service_user_generated_uid" ] && \
       [ "$user_generated_uid" != "$created_service_user_generated_uid" ]; then
        fail BOOTSTRAP.GROUP_INPUT_INVALID
    fi
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
    ensure_service_group_membership_is_exact "$user_generated_uid"
    identity_validation_stage=service_supplementary_group_validation
    ensure_service_has_no_explicit_supplementary_membership \
        "$group_generated_uid" "$user_generated_uid"
    identity_validation_stage=service_admin_membership_validation
    ensure_service_is_not_admin "$group_generated_uid"
    identity_validation_stage=
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
    owner=$1
    group=$2
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
    [ "$actual" = "$owner:$group:$expected_mode" ] || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
}

require_existing_root_directory() {
    path=$1
    [ -d "$path" ] && [ ! -L "$path" ] || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    actual=$(stat -f '%Su:%Sg:%Lp' "$path" 2>/dev/null || true)
    [ "$actual" = root:wheel:755 ] || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    acl_mode=$(ls -lde "$path" 2>/dev/null | awk 'NR == 1 { print $1 }')
    case "$acl_mode" in
        ''|*+*) fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID ;;
    esac
}

normalize_root_directory() {
    path=$1
    [ -d "$path" ] && [ ! -L "$path" ] || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    chown root:wheel "$path" >/dev/null 2>&1 || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    chmod 0755 "$path" >/dev/null 2>&1 || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    remove_extended_acl root wheel 0755 "$path"
}

normalize_root_executable() {
    path=$1
    [ -f "$path" ] && [ ! -L "$path" ] || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    link_count=$(stat -f '%l' "$path" 2>/dev/null || true)
    [ "$link_count" = 1 ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    chown root:wheel "$path" >/dev/null 2>&1 || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    chmod 0755 "$path" >/dev/null 2>&1 || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    remove_extended_acl root wheel 0755 "$path"
}

validate_root_service_launcher_path() {
    [ "$identity_mode" = system ] || return 0
    install_validation_stage=root_service_authority_validation
    require_system_authority
    install_validation_stage=root_opt_directory_validation
    require_existing_root_directory /opt
    install_validation_stage=root_scratchbird_directory_normalization
    normalize_root_directory /opt/ScratchBird
    install_validation_stage=root_bin_directory_normalization
    normalize_root_directory /opt/ScratchBird/bin
    install_validation_stage=root_sblaunch_normalization
    normalize_root_executable /opt/ScratchBird/bin/SBlaunch
    install_validation_stage=root_sbsrv_normalization
    normalize_root_executable /opt/ScratchBird/bin/SBsrv
    install_validation_stage=root_sbmgr_normalization
    normalize_root_executable /opt/ScratchBird/bin/SBmgr
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
    [ "$(plist_value :ProgramArguments:0)" = /opt/ScratchBird/bin/SBlaunch ] || \
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
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
    install_validation_stage=launchd_directory_validation
    require_existing_root_directory /Library/LaunchDaemons
    install_validation_stage=launchd_sbsrv_definition_validation
    validate_launchd_service_definition \
        com.scratchbird.sbsrv sbsrv \
        /var/log/scratchbird/launchd/SBsrv.out.log \
        /var/log/scratchbird/launchd/SBsrv.err.log
    install_validation_stage=launchd_sbmgr_definition_validation
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
    make_directory 0750 root "$SERVICE_GROUP" /var/log/scratchbird
    make_directory 0750 root "$SERVICE_GROUP" /var/log/scratchbird/launchd
    make_directory 0750 "$SERVICE_USER" "$SERVICE_GROUP" /var/log/scratchbird/runtime
    remove_extended_acl root "$SERVICE_GROUP" 0750 /var/log/scratchbird
    remove_extended_acl root "$SERVICE_GROUP" 0750 /var/log/scratchbird/launchd
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

migrate_exact_packaged_log_default() {
    target=$1
    legacy_line=$2
    replacement_line=$3
    [ -f "$target" ] && [ ! -L "$target" ] || \
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    legacy_count=$(grep -Fxc "$legacy_line" "$target" 2>/dev/null || true)
    case "$legacy_count" in
        0) return 0 ;;
        1) ;;
        *) fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID ;;
    esac
    temp="$target.scratchbird-log-migration.$$"
    umask 027
    awk -v old="$legacy_line" -v replacement="$replacement_line" '
        $0 == old { print replacement; next }
        { print }
    ' "$target" > "$temp" || {
        rm -f -- "$temp"
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    }
    mv -f "$temp" "$target" || {
        rm -f -- "$temp"
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    }
}

migrate_legacy_packaged_log_defaults() {
    config_root=$1
    migrate_exact_packaged_log_default \
        "$config_root/SBsrv.conf" \
        'log_file = /var/log/scratchbird/SBsrv.log' \
        'log_file = /var/log/scratchbird/runtime/SBsrv.log'
    migrate_exact_packaged_log_default \
        "$config_root/SBmgr.conf" \
        'manager.log.path = /var/log/scratchbird/SBmgr.log' \
        'manager.log.path = /var/log/scratchbird/runtime/SBmgr.log'
}

install_default_configuration() {
    defaults_root=$(root_path /opt/ScratchBird/share/scratchbird/config-defaults)
    config_root=$(root_path "/Library/Application Support/ScratchBird")
    [ -d "$defaults_root" ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    [ ! -L "$defaults_root" ] || fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID
    [ ! -L "$config_root" ] || fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID

    install_validation_stage=default_configuration_content
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
    install_validation_stage=default_configuration_symlink_validation
    if find "$config_root" -xdev -type l -print -quit | grep -q .; then
        fail BOOTSTRAP.DIRECTORY_PERMISSION_INVALID
    fi
    install_validation_stage=default_configuration_migration
    migrate_legacy_packaged_log_defaults "$config_root"

    install_validation_stage=default_configuration_permissions
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
    [ "$identity_mode" = fixture ] || return 0
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
        printf '  "service_administrator_group_membership": false,\n'
        printf '  "service_authority_scope": "filesystem_directory_and_process_execution_only_no_database_or_security_authority",\n'
        printf '  "resolved_effective_group_policy": "launchd_host_computed_groups_cleared_before_scratchbird_product_exec",\n'
        printf '  "service_launcher": "/opt/ScratchBird/bin/SBlaunch",\n'
        printf '  "launchd_bootstrap_identity": "root:wheel",\n'
        printf '  "launchd_init_groups": false,\n'
        printf '  "final_product_identity": "scratchbird:scratchbird",\n'
        printf '  "final_supplementary_groups": [],\n'
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
            install_validation_stage=service_state_validation
            ensure_services_not_loaded
            install_validation_stage=service_identity_validation
            ensure_service_identity
        fi
        install_validation_stage=directory_configuration
        configure_directories
        install_default_configuration
        validate_root_service_launcher_path
        validate_launchd_service_definitions
        write_fixture_identity_evidence
        install_validation_stage=install_evidence_write
        write_install_evidence
        install_validation_stage=
        ;;
    pre-remove)
        if [ "$identity_mode" = system ]; then
            require_system_authority
        fi
        install_validation_stage=pre_remove_configuration_preservation
        preserve_configuration
        install_validation_stage=
        ;;
    *)
        fail BOOTSTRAP.INSTALL_DEFAULTS_INVALID 2
        ;;
esac

exit 0
