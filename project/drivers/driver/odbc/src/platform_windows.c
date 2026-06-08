// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird ODBC Driver - Windows Platform Implementation
 * Copyright (c) 2025-2026 Dalton Calford
 */
#include "scratchbird/odbc/platform.h"

#if SB_PLATFORM_WINDOWS

/* Platform initialization */
int sb_platform_init(void) {
    /* Initialize Windows sockets if needed */
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    return 0;
}

void sb_platform_cleanup(void) {
    WSACleanup();
}

#endif /* SB_PLATFORM_WINDOWS */
