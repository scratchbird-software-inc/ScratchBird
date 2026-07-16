#!/bin/sh
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

set -eu

SERVICE_NAME=scratchbird-sbsrv.service
SERVICE_USER=scratchbird
SERVICE_GROUP=scratchbird
ACTION=${1:-}
if [ -z "$ACTION" ]; then
    echo "BOOTSTRAP.INSTALL_DEFAULTS_INVALID: missing lifecycle action" >&2
    exit 2
fi
shift

install_root=/
identity_mode=system
package_version=unknown
package_format=unknown

while [ "$#" -gt 0 ]; do
    case "$1" in
        --root)
            [ "$#" -ge 2 ] || {
                echo "BOOTSTRAP.INSTALL_DEFAULTS_INVALID: --root requires a value" >&2
                exit 2
            }
            install_root=$2
            shift 2
            ;;
        --identity-mode)
            [ "$#" -ge 2 ] || {
                echo "BOOTSTRAP.INSTALL_DEFAULTS_INVALID: --identity-mode requires a value" >&2
                exit 2
            }
            identity_mode=$2
            shift 2
            ;;
        --package-version)
            [ "$#" -ge 2 ] || {
                echo "BOOTSTRAP.INSTALL_DEFAULTS_INVALID: --package-version requires a value" >&2
                exit 2
            }
            package_version=$2
            shift 2
            ;;
        --package-format)
            [ "$#" -ge 2 ] || {
                echo "BOOTSTRAP.INSTALL_DEFAULTS_INVALID: --package-format requires a value" >&2
                exit 2
            }
            package_format=$2
            shift 2
            ;;
        *)
            echo "BOOTSTRAP.INSTALL_DEFAULTS_INVALID: unknown lifecycle option: $1" >&2
            exit 2
            ;;
    esac
done

case "$install_root" in
    /*) ;;
    *)
        echo "BOOTSTRAP.INSTALL_DEFAULTS_INVALID: install root must be absolute" >&2
        exit 2
        ;;
esac
case "$install_root" in
    *'/../'*|*'/..'|*'/./'*|*'/.'|*'//'*)
        echo "BOOTSTRAP.INSTALL_DEFAULTS_INVALID: install root is not normalized" >&2
        exit 2
        ;;
esac
case "$identity_mode" in
    system|fixture) ;;
    *)
        echo "BOOTSTRAP.INSTALL_DEFAULTS_INVALID: invalid identity mode" >&2
        exit 2
        ;;
esac
if [ "$identity_mode" = fixture ] && [ "$install_root" = / ]; then
    echo "BOOTSTRAP.INSTALL_DEFAULTS_INVALID: fixture mode requires an isolated root" >&2
    exit 2
fi
if [ "$identity_mode" = system ] && [ "$install_root" != / ]; then
    echo "BOOTSTRAP.INSTALL_DEFAULTS_INVALID: system identity mode requires root /" >&2
    exit 2
fi
case "$package_version" in
    *[!ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+~-]*)
        echo "BOOTSTRAP.INSTALL_DEFAULTS_INVALID: invalid package version" >&2
        exit 2
        ;;
esac
case "$package_format" in
    *[!ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-]*)
        echo "BOOTSTRAP.INSTALL_DEFAULTS_INVALID: invalid package format" >&2
        exit 2
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
    if [ "$(id -u)" -ne 0 ]; then
        echo "BOOTSTRAP.OS_AUTHORITY_DENIED: system package lifecycle requires root" >&2
        exit 1
    fi
}

ensure_service_identity() {
    require_system_authority

    sysusers_conf=$(root_path /usr/lib/sysusers.d/scratchbird.conf)
    if command -v systemd-sysusers >/dev/null 2>&1 && [ -f "$sysusers_conf" ]; then
        systemd-sysusers "$sysusers_conf"
    fi

    if ! getent group "$SERVICE_GROUP" >/dev/null 2>&1; then
        command -v groupadd >/dev/null 2>&1 || {
            echo "BOOTSTRAP.GROUP_CREATE_FAILED: groupadd is unavailable" >&2
            exit 1
        }
        groupadd --system "$SERVICE_GROUP"
    fi

    if ! getent passwd "$SERVICE_USER" >/dev/null 2>&1; then
        command -v useradd >/dev/null 2>&1 || {
            echo "BOOTSTRAP.GROUP_CREATE_FAILED: useradd is unavailable" >&2
            exit 1
        }
        nologin_shell=/usr/sbin/nologin
        if [ ! -x "$nologin_shell" ]; then
            nologin_shell=/sbin/nologin
        fi
        if [ ! -x "$nologin_shell" ]; then
            nologin_shell=/bin/false
        fi
        useradd --system --gid "$SERVICE_GROUP" --home-dir /var/lib/scratchbird \
            --no-create-home --shell "$nologin_shell" \
            --comment "ScratchBird service account" "$SERVICE_USER"
    fi

    group_entry=$(getent group "$SERVICE_GROUP")
    user_entry=$(getent passwd "$SERVICE_USER")
    local_group_count=$(awk -F: '$1 == "scratchbird" { count++ } END { print count + 0 }' /etc/group)
    local_user_count=$(awk -F: '$1 == "scratchbird" { count++ } END { print count + 0 }' /etc/passwd)
    if [ "$local_group_count" -ne 1 ] || [ "$local_user_count" -ne 1 ]; then
        echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird identity must be uniquely local" >&2
        exit 1
    fi
    local_group_entry=$(awk -F: '$1 == "scratchbird" { print; exit }' /etc/group)
    local_user_entry=$(awk -F: '$1 == "scratchbird" { print; exit }' /etc/passwd)
    if [ "$group_entry" != "$local_group_entry" ] || [ "$user_entry" != "$local_user_entry" ]; then
        echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird NSS identity is ambiguous" >&2
        exit 1
    fi
    group_explicit_members=$(printf '%s\n' "$local_group_entry" | cut -d: -f4)
    case "$group_explicit_members" in
        ''|scratchbird) ;;
        *)
            echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird group has forbidden explicit members" >&2
            exit 1
            ;;
    esac
    group_gid=$(printf '%s\n' "$group_entry" | cut -d: -f3)
    user_uid=$(printf '%s\n' "$user_entry" | cut -d: -f3)
    user_gid=$(printf '%s\n' "$user_entry" | cut -d: -f4)
    user_home=$(printf '%s\n' "$user_entry" | cut -d: -f6)
    user_shell=$(printf '%s\n' "$user_entry" | cut -d: -f7)
    case "$group_gid:$user_uid:$user_gid" in
        *[!0123456789:]*)
            echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird numeric identity is malformed" >&2
            exit 1
            ;;
    esac
    if [ "$group_gid" -eq 0 ] || [ "$user_uid" -eq 0 ]; then
        echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird identity must not have root authority" >&2
        exit 1
    fi
    if [ "$user_gid" != "$group_gid" ]; then
        echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird user primary group mismatch" >&2
        exit 1
    fi
    service_group_ids=$(id -G "$SERVICE_USER" 2>/dev/null) || {
        echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird effective groups are unavailable" >&2
        exit 1
    }
    service_group_seen=false
    for effective_gid in $service_group_ids; do
        case "$effective_gid" in
            ''|*[!0123456789]*)
                echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird effective group is malformed" >&2
                exit 1
                ;;
        esac
        if [ "$effective_gid" != "$group_gid" ]; then
            echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird supplementary group authority is forbidden" >&2
            exit 1
        fi
        service_group_seen=true
    done
    if [ "$service_group_seen" != true ]; then
        echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird effective group set is empty" >&2
        exit 1
    fi
    if [ "$user_home" != /var/lib/scratchbird ]; then
        echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird service home mismatch" >&2
        exit 1
    fi
    case "$user_shell" in
        /usr/sbin/nologin|/sbin/nologin|/usr/bin/nologin|/bin/false) ;;
        *)
            echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird identity is not non-login" >&2
            exit 1
            ;;
    esac
    if [ ! -x "$user_shell" ]; then
        echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird non-login shell is unavailable" >&2
        exit 1
    fi
    sys_uid_max=$(awk '$1 == "SYS_UID_MAX" { value=$2 } END { print value }' /etc/login.defs 2>/dev/null || true)
    sys_gid_max=$(awk '$1 == "SYS_GID_MAX" { value=$2 } END { print value }' /etc/login.defs 2>/dev/null || true)
    case "$sys_uid_max" in ''|*[!0123456789]*) sys_uid_max=999 ;; esac
    case "$sys_gid_max" in ''|*[!0123456789]*) sys_gid_max=999 ;; esac
    if [ "$user_uid" -gt "$sys_uid_max" ] || [ "$group_gid" -gt "$sys_gid_max" ]; then
        echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird identity is outside the local system-account range" >&2
        exit 1
    fi
    duplicate_uid_count=$(awk -F: -v expected_uid="$user_uid" '$3 == expected_uid { count++ } END { print count + 0 }' /etc/passwd)
    duplicate_gid_count=$(awk -F: -v expected_gid="$group_gid" '$3 == expected_gid { count++ } END { print count + 0 }' /etc/group)
    if [ "$duplicate_uid_count" -ne 1 ] || [ "$duplicate_gid_count" -ne 1 ]; then
        echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird numeric identity is shared" >&2
        exit 1
    fi
    local_shadow_count=$(awk -F: '$1 == "scratchbird" { count++ } END { print count + 0 }' /etc/shadow)
    shadow_password=$(awk -F: '$1 == "scratchbird" { print $2; exit }' /etc/shadow)
    if [ "$local_shadow_count" -ne 1 ]; then
        echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird local shadow identity is missing or ambiguous" >&2
        exit 1
    fi
    case "$shadow_password" in
        '!'*|'*'*) ;;
        *)
            echo "BOOTSTRAP.GROUP_INPUT_INVALID: scratchbird service account is not locked" >&2
            exit 1
            ;;
    esac
}

make_directory() {
    mode=$1
    owner=$2
    group=$3
    path=$(root_path "$4")
    if [ "$identity_mode" = fixture ]; then
        install -d -m "$mode" "$path"
    else
        install -d -m "$mode" -o "$owner" -g "$group" "$path"
    fi
}

configure_directories() {
    make_directory 0750 root scratchbird /var/lib/scratchbird
    make_directory 0750 scratchbird scratchbird /var/lib/scratchbird/data
    make_directory 0750 root scratchbird /var/lib/scratchbird/install
    make_directory 0750 scratchbird scratchbird /var/log/scratchbird
    make_directory 0750 scratchbird scratchbird /run/scratchbird
    make_directory 0750 scratchbird scratchbird /run/scratchbird/control
    make_directory 0750 scratchbird scratchbird /run/scratchbird/runtime
    make_directory 0750 scratchbird scratchbird /run/scratchbird/listener
    make_directory 0750 scratchbird scratchbird /run/scratchbird/listener/control
    make_directory 0750 scratchbird scratchbird /run/scratchbird/listener/runtime
    make_directory 0750 scratchbird scratchbird /run/scratchbird/manager
    make_directory 0750 scratchbird scratchbird /run/scratchbird/manager/control
    make_directory 0750 scratchbird scratchbird /run/scratchbird/manager/runtime

    config_root=$(root_path /etc/scratchbird)
    if [ -d "$config_root" ]; then
        if [ "$identity_mode" = fixture ]; then
            find "$config_root" -xdev -type d -exec chmod 0750 {} +
            find "$config_root" -xdev -type f -exec chmod 0640 {} +
        else
            find "$config_root" -xdev -type d -exec chown root:scratchbird {} +
            find "$config_root" -xdev -type d -exec chmod 0750 {} +
            find "$config_root" -xdev -type f -exec chown root:scratchbird {} +
            find "$config_root" -xdev -type f -exec chmod 0640 {} +
        fi
    fi
}

preserve_configuration() {
    config_root=$(root_path /etc/scratchbird)
    preserve_root=$(root_path /var/lib/scratchbird/install/config-preserve)
    if [ ! -d "$config_root" ]; then
        return
    fi
    rm -rf -- "$preserve_root"
    if [ "$identity_mode" = fixture ]; then
        install -d -m 0700 "$preserve_root"
    else
        install -d -m 0700 -o root -g scratchbird "$preserve_root"
    fi
    cp -a "$config_root"/. "$preserve_root"/
    find "$preserve_root" -xdev -type d -exec chmod 0700 {} +
    find "$preserve_root" -xdev -type f -exec chmod 0600 {} +
    if [ "$identity_mode" = system ]; then
        find "$preserve_root" -xdev -type d -exec chown root:scratchbird {} +
        find "$preserve_root" -xdev -type f -exec chown root:scratchbird {} +
    fi
}

service_state() {
    query=$1
    if [ "$identity_mode" = system ] && command -v systemctl >/dev/null 2>&1; then
        systemctl "$query" "$SERVICE_NAME" 2>/dev/null || true
    else
        printf '%s\n' not-queried
    fi
}

write_install_evidence() {
    evidence_dir=$(root_path /var/lib/scratchbird/install)
    evidence_file=$evidence_dir/LINUX_SYSTEM_INSTALL_STATE.json
    evidence_temp=$evidence_dir/.LINUX_SYSTEM_INSTALL_STATE.json.$$
    timestamp=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
    enabled_state=$(service_state is-enabled | head -n 1)
    active_state=$(service_state is-active | head -n 1)
    umask 027
    {
        printf '{\n'
        printf '  "schema_id": "scratchbird.linux_system_install_state.v1",\n'
        printf '  "installed_at_utc": "%s",\n' "$timestamp"
        printf '  "package_format": "%s",\n' "$package_format"
        printf '  "package_version": "%s",\n' "$package_version"
        printf '  "service_user": "scratchbird",\n'
        printf '  "service_group": "scratchbird",\n'
        printf '  "service_authority_scope": "filesystem_directory_and_process_execution_only_no_database_or_security_authority",\n'
        printf '  "human_service_group_membership_mutated": false,\n'
        printf '  "create_time_os_authorization": "root_only",\n'
        printf '  "service_unit": "scratchbird-sbsrv.service",\n'
        printf '  "service_enablement_observed": "%s",\n' "$enabled_state"
        printf '  "service_activity_observed": "%s",\n' "$active_state"
        printf '  "native_default_port": 3092,\n'
        printf '  "database_files_created": false,\n'
        printf '  "security_sidecars_created": false\n'
        printf '}\n'
    } > "$evidence_temp"
    chmod 0640 "$evidence_temp"
    if [ "$identity_mode" = system ]; then
        chown root:scratchbird "$evidence_temp"
    fi
    mv -f "$evidence_temp" "$evidence_file"
}

reload_service_manager() {
    if [ "$identity_mode" = system ] && command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload >/dev/null 2>&1 || true
    fi
}

pre_remove_service() {
    require_system_authority
    if command -v systemctl >/dev/null 2>&1; then
        systemctl stop "$SERVICE_NAME" >/dev/null 2>&1 || true
        systemctl disable "$SERVICE_NAME" >/dev/null 2>&1 || true
    fi
}

case "$ACTION" in
    post-install)
        if [ "$identity_mode" = system ]; then
            ensure_service_identity
        fi
        configure_directories
        reload_service_manager
        write_install_evidence
        ;;
    pre-remove)
        if [ "$identity_mode" = system ]; then
            pre_remove_service
        fi
        preserve_configuration
        ;;
    *)
        echo "BOOTSTRAP.INSTALL_DEFAULTS_INVALID: unsupported lifecycle action: $ACTION" >&2
        exit 2
        ;;
esac

exit 0
