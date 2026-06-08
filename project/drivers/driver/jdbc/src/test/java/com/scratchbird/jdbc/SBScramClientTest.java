// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver tests
 */
package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

public class SBScramClientTest {

    @Test
    public void scramHandshakeProducesExpectedFormat() {
        SBScramClient scram = new SBScramClient("user", "rOprNGfwEbeRWgbNEkqO");
        String clientFirst = scram.getClientFirstMessage();
        assertEquals("n,,n=user,r=rOprNGfwEbeRWgbNEkqO", clientFirst);

        String serverFirst =
            "r=rOprNGfwEbeRWgbNEkqO+3rfcNHYJY1ZVvWVs7j," +
            "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";

        String clientFinal = scram.handleServerFirst(serverFirst, "pencil");
        assertTrue(clientFinal.startsWith("c=biws,r=rOprNGfwEbeRWgbNEkqO+3rfcNHYJY1ZVvWVs7j,"));

        String serverFinal = "v=" + scram.getServerSignatureBase64();
        scram.verifyServerFinal(serverFinal);
    }

    @Test
    public void scramSha512HandshakeProducesExpectedFormat() {
        SBScramClient scram = new SBScramClient("user", SBScramClient.Algorithm.SHA_512, "clientNonce");
        String clientFirst = scram.getClientFirstMessage();
        assertEquals("n,,n=user,r=clientNonce", clientFirst);

        String serverFirst =
            "r=clientNonce-server," +
            "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";

        String clientFinal = scram.handleServerFirst(serverFirst, "pencil");
        assertTrue(clientFinal.startsWith("c=biws,r=clientNonce-server,"));

        String serverFinal = "v=" + scram.getServerSignatureBase64();
        scram.verifyServerFinal(serverFinal);
    }
}
