// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * DBeaver - Universal Database Manager
 * Copyright (C) 2010-2026 DBeaver Corp and others
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.jkiss.dbeaver.ext.scratchbird.model;

import org.jkiss.code.NotNull;

import java.util.Arrays;
import java.util.Collection;
import java.util.List;

/**
 * Simple runnable smoke check for recursive schema tree shape and sibling uniqueness.
 */
public final class ScratchBirdSchemaTreeSmoke {

    private ScratchBirdSchemaTreeSmoke() {
    }

    public static void main(String[] args) {
        run();
        System.out.println("ScratchBird schema tree smoke check passed");
    }

    static void run() {
        List<ScratchBirdSchemaTreeBuilder.Node> roots = ScratchBirdSchemaTreeBuilder.build(Arrays.asList(
            "sys",
            "users",
            "users.alice.dev",
            "users.alice.prod",
            "users.bob.dev",
            "users.bob.dev", // duplicate input path
            "analytics.dev",
            "analytics.prod"
        ), true);

        require(roots.size() == 8, "Expected canonical roots plus analytics");

        ScratchBirdSchemaTreeBuilder.Node users = findByName(roots, "users");
        require(users != null, "Expected users root");

        ScratchBirdSchemaTreeBuilder.Node alice = findByName(users.getChildren(), "alice");
        ScratchBirdSchemaTreeBuilder.Node bob = findByName(users.getChildren(), "bob");
        require(alice != null, "Expected users.alice");
        require(bob != null, "Expected users.bob");

        require(findByName(alice.getChildren(), "dev") != null, "Expected users.alice.dev");
        require(findByName(alice.getChildren(), "prod") != null, "Expected users.alice.prod");
        require(findByName(bob.getChildren(), "dev") != null, "Expected users.bob.dev");

        // Per-parent uniqueness: duplicate users.bob.dev must not create sibling duplicates.
        require(bob.getChildren().size() == 1, "Expected unique child names per parent");

        ScratchBirdSchemaTreeBuilder.Node sys = findByName(roots, "sys");
        require(sys != null && sys.isTerminal(), "Expected sys terminal node");
        require(findByName(sys.getChildren(), "domains") != null, "Expected sys.domains branch");

        ScratchBirdSchemaTreeBuilder.Node metrics = findByName(roots, "metrics");
        require(metrics != null && metrics.isClientOnly(), "Expected client-only metrics branch");
        require(findByName(metrics.getChildren(), "health-scorecards") != null, "Expected metrics health-scorecards branch");
        require(findByName(metrics.getChildren(), "alerts") != null, "Expected metrics alerts branch");
    }

    private static void require(boolean condition, @NotNull String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }

    private static ScratchBirdSchemaTreeBuilder.Node findByName(
        @NotNull Collection<ScratchBirdSchemaTreeBuilder.Node> nodes,
        @NotNull String name
    ) {
        for (ScratchBirdSchemaTreeBuilder.Node node : nodes) {
            if (name.equals(node.getName())) {
                return node;
            }
        }
        return null;
    }
}
