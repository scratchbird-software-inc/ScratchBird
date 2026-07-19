// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// The only privileged launchd boundary in the macOS system package.  It
// accepts a fixed selector, clears every supplementary group, assumes the
// locked service identity, and execs a fixed product target with a clean
// environment.  It never forwards an operator-controlled command or path.

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _DEFAULT_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L

#include <grp.h>
#include <pwd.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SCRATCHBIRD_ACCOUNT_NAME "scratchbird"
#define SCRATCHBIRD_HOME "/var/lib/scratchbird"
#define SCRATCHBIRD_SHELL "/usr/bin/false"
#define SCRATCHBIRD_CONFIG_ROOT "/Library/Application Support/ScratchBird"

#define SCRATCHBIRD_EXIT_DENIED 78
#define SCRATCHBIRD_LOOKUP_BUFFER_SIZE 16384U
#define SCRATCHBIRD_MAX_KERNEL_GROUPS 1024

#if defined(__APPLE__)
/*
 * Darwin's libc getgroups wrapper can synthesize directory-service group
 * membership. Bind the kernel-facing symbol explicitly so post-drop checks
 * observe only credentials attached to this process.
 */
extern int ScratchBirdKernelGetGroups(int size, gid_t groups[])
    __asm("_getgroups");
#endif

static void write_all(int descriptor, const char *data, size_t length)
{
    while (length > 0U)
    {
        const ssize_t written = write(descriptor, data, length);
        if (written <= 0)
        {
            return;
        }
        data += (size_t) written;
        length -= (size_t) written;
    }
}

static void deny(const char *code)
{
    write_all(STDERR_FILENO, code, strlen(code));
    write_all(STDERR_FILENO, "\n", 1U);
    _exit(SCRATCHBIRD_EXIT_DENIED);
}

static int raw_group_access_list_count(void)
{
#if defined(__APPLE__)
    return ScratchBirdKernelGetGroups(0, NULL);
#else
    return getgroups(0, NULL);
#endif
}

static void require_no_additional_group_authority(const char *failure_code)
{
    const int count = raw_group_access_list_count();

    if (count < 0 || count > SCRATCHBIRD_MAX_KERNEL_GROUPS)
    {
        deny(failure_code);
    }
#if defined(__APPLE__)
    {
        gid_t groups[SCRATCHBIRD_MAX_KERNEL_GROUPS];
        const gid_t effective_group = getegid();
        int index;

        /* XNU retains the effective GID at raw credential index zero. */
        if (count != 1)
        {
            deny(failure_code);
        }
        if (count > 0 &&
            ScratchBirdKernelGetGroups(count, groups) != count)
        {
            deny(failure_code);
        }
        for (index = 0; index < count; ++index)
        {
            /* Darwin may include the effective GID in its access list. */
            if (groups[index] != effective_group)
            {
                deny(failure_code);
            }
        }
    }
#else
    if (count != 0)
    {
        deny(failure_code);
    }
#endif
}

static void close_inherited_descriptors(void)
{
    long descriptor_limit = sysconf(_SC_OPEN_MAX);
    int descriptor;

    if (descriptor_limit < (long) (STDERR_FILENO + 1) ||
        descriptor_limit > 1048576L)
    {
        deny("SB_MACOS_LAUNCHER.DESCRIPTOR_RANGE_DENIED");
    }
    for (descriptor = STDERR_FILENO + 1;
         descriptor < (int) descriptor_limit;
         ++descriptor)
    {
        (void) close(descriptor);
    }
}

static void validate_executable_target(const char *target, gid_t runtime_gid)
{
    struct stat metadata;
    mode_t execution_mask = S_IXOTH;

    if (lstat(target, &metadata) != 0)
    {
        deny("SB_MACOS_LAUNCHER.TARGET_LOOKUP_DENIED");
    }
    if (!S_ISREG(metadata.st_mode) || S_ISLNK(metadata.st_mode))
    {
        deny("SB_MACOS_LAUNCHER.TARGET_TYPE_DENIED");
    }
    if (metadata.st_nlink != (nlink_t) 1)
    {
        deny("SB_MACOS_LAUNCHER.TARGET_LINK_COUNT_DENIED");
    }
    if (metadata.st_uid != (uid_t) 0)
    {
        deny("SB_MACOS_LAUNCHER.TARGET_OWNER_DENIED");
    }
    if ((metadata.st_mode & (S_IWGRP | S_IWOTH)) != (mode_t) 0)
    {
        deny("SB_MACOS_LAUNCHER.TARGET_WRITE_AUTHORITY_DENIED");
    }
    if ((metadata.st_mode & (S_ISUID | S_ISGID)) != (mode_t) 0)
    {
        deny("SB_MACOS_LAUNCHER.TARGET_PRIVILEGE_BITS_DENIED");
    }

    if (metadata.st_gid == runtime_gid)
    {
        execution_mask = (mode_t) (execution_mask | S_IXGRP);
    }
    if ((metadata.st_mode & execution_mask) == (mode_t) 0)
    {
        deny("SB_MACOS_LAUNCHER.TARGET_EXECUTE_AUTHORITY_DENIED");
    }
}

int main(int argc, char *argv[])
{
    static char *const sbsrv_argv[] = {
        "/opt/ScratchBird/bin/SBsrv",
        "--config",
        SCRATCHBIRD_CONFIG_ROOT "/SBsrv.conf",
        "--foreground",
        NULL
    };
    static char *const sbmgr_argv[] = {
        "/opt/ScratchBird/bin/SBmgr",
        "--config",
        SCRATCHBIRD_CONFIG_ROOT "/SBmgr.conf",
        "--foreground",
        NULL
    };
    static char *const credential_probe_argv[] = {
        "/var/lib/scratchbird/install/sb_bootstrap_launchd_credential_probe",
        "--verify-running-service-identity",
        SCRATCHBIRD_CONFIG_ROOT "/SBbootstrap.profile",
        "/var/lib/scratchbird/install/launchd-credential-canaries",
        NULL
    };
    static char *const clean_environment[] = {
        "HOME=" SCRATCHBIRD_HOME,
        "USER=" SCRATCHBIRD_ACCOUNT_NAME,
        "LOGNAME=" SCRATCHBIRD_ACCOUNT_NAME,
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
        "TMPDIR=/var/run/scratchbird",
        NULL
    };

    char passwd_buffer[SCRATCHBIRD_LOOKUP_BUFFER_SIZE];
    char group_buffer[SCRATCHBIRD_LOOKUP_BUFFER_SIZE];
    struct passwd passwd_entry;
    struct passwd *passwd_result = NULL;
    struct group group_entry;
    struct group *group_result = NULL;
    char *const *selected_argv = NULL;
    const char *target = NULL;
    uid_t runtime_uid;
    gid_t runtime_gid;

    if (argc != 2 || argv == NULL || argv[0] == NULL || argv[1] == NULL)
    {
        deny("SB_MACOS_LAUNCHER.SELECTOR_DENIED");
    }

    if (strcmp(argv[1], "sbsrv") == 0)
    {
        target = sbsrv_argv[0];
        selected_argv = sbsrv_argv;
    }
    else if (strcmp(argv[1], "sbmgr") == 0)
    {
        target = sbmgr_argv[0];
        selected_argv = sbmgr_argv;
    }
    else if (strcmp(argv[1], "credential-probe") == 0)
    {
        target = credential_probe_argv[0];
        selected_argv = credential_probe_argv;
    }
    else
    {
        deny("SB_MACOS_LAUNCHER.SELECTOR_DENIED");
    }

    if (getuid() != (uid_t) 0 || geteuid() != (uid_t) 0)
    {
        deny("SB_MACOS_LAUNCHER.ROOT_AUTHORITY_REQUIRED");
    }

    if (getpwnam_r(SCRATCHBIRD_ACCOUNT_NAME,
                   &passwd_entry,
                   passwd_buffer,
                   sizeof(passwd_buffer),
                   &passwd_result) != 0 ||
        passwd_result == NULL)
    {
        deny("SB_MACOS_LAUNCHER.PASSWD_LOOKUP_DENIED");
    }
    if (passwd_result->pw_name == NULL ||
        strcmp(passwd_result->pw_name, SCRATCHBIRD_ACCOUNT_NAME) != 0 ||
        passwd_result->pw_dir == NULL ||
        strcmp(passwd_result->pw_dir, SCRATCHBIRD_HOME) != 0 ||
        passwd_result->pw_shell == NULL ||
        strcmp(passwd_result->pw_shell, SCRATCHBIRD_SHELL) != 0 ||
        passwd_result->pw_uid == (uid_t) 0 ||
        passwd_result->pw_gid == (gid_t) 0)
    {
        deny("SB_MACOS_LAUNCHER.PASSWD_RECORD_DENIED");
    }

    if (getgrnam_r(SCRATCHBIRD_ACCOUNT_NAME,
                   &group_entry,
                   group_buffer,
                   sizeof(group_buffer),
                   &group_result) != 0 ||
        group_result == NULL)
    {
        deny("SB_MACOS_LAUNCHER.GROUP_LOOKUP_DENIED");
    }
    if (group_result->gr_name == NULL ||
        strcmp(group_result->gr_name, SCRATCHBIRD_ACCOUNT_NAME) != 0 ||
        group_result->gr_gid == (gid_t) 0 ||
        group_result->gr_gid != passwd_result->pw_gid)
    {
        deny("SB_MACOS_LAUNCHER.GROUP_RECORD_DENIED");
    }

    runtime_uid = passwd_result->pw_uid;
    runtime_gid = group_result->gr_gid;
    validate_executable_target(target, runtime_gid);
    close_inherited_descriptors();

    if (setgroups(0, NULL) != 0)
    {
        deny("SB_MACOS_LAUNCHER.CLEAR_GROUPS_DENIED");
    }
    require_no_additional_group_authority(
        "SB_MACOS_LAUNCHER.CLEAR_GROUPS_VERIFICATION_DENIED");

    if (setgid(runtime_gid) != 0)
    {
        deny("SB_MACOS_LAUNCHER.SETGID_DENIED");
    }
    if (setuid(runtime_uid) != 0)
    {
        deny("SB_MACOS_LAUNCHER.SETUID_DENIED");
    }

    if (getuid() != runtime_uid || geteuid() != runtime_uid ||
        getgid() != runtime_gid || getegid() != runtime_gid)
    {
        deny("SB_MACOS_LAUNCHER.IDENTITY_VERIFICATION_DENIED");
    }
    require_no_additional_group_authority(
        "SB_MACOS_LAUNCHER.GROUPS_VERIFICATION_DENIED");

    if (setgid((gid_t) 0) == 0)
    {
        deny("SB_MACOS_LAUNCHER.GID_REGAINED");
    }
    if (setuid((uid_t) 0) == 0)
    {
        deny("SB_MACOS_LAUNCHER.UID_REGAINED");
    }
    if (getuid() != runtime_uid || geteuid() != runtime_uid ||
        getgid() != runtime_gid || getegid() != runtime_gid)
    {
        deny("SB_MACOS_LAUNCHER.POST_REGAIN_IDENTITY_DENIED");
    }
    require_no_additional_group_authority(
        "SB_MACOS_LAUNCHER.POST_REGAIN_GROUPS_DENIED");

    (void) umask((mode_t) 0027);
    if (chdir(SCRATCHBIRD_HOME) != 0)
    {
        deny("SB_MACOS_LAUNCHER.WORKING_DIRECTORY_DENIED");
    }
    /* Revalidate immediately before exec after authority has been dropped. */
    validate_executable_target(target, runtime_gid);

    execve(target, selected_argv, clean_environment);
    deny("SB_MACOS_LAUNCHER.EXEC_DENIED");
}
