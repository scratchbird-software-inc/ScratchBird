#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Bounded deterministic delta reduction of scheduled transaction actions.

The predicate must replay candidates against the real engine and preserve the
failure class. Epoch boundaries are retained: removing actions never merges
transactions or invents a different transaction authority.
"""
from __future__ import annotations


def reduce_schedule(schedule: list[list[dict]], reproduces, max_attempts: int = 32):
    tagged = [(epoch, action) for epoch, actions in enumerate(schedule) for action in actions]
    def unpack(items):
        result = [[] for _ in schedule]
        for epoch, action in items:
            result[epoch].append(dict(action))
        return result
    attempts = 0
    partitions = 2
    while tagged and attempts < max_attempts:
        chunk = max(1, (len(tagged) + partitions - 1) // partitions)
        reduced = False
        for start in range(0, len(tagged), chunk):
            candidate = tagged[:start] + tagged[start + chunk:]
            attempts += 1
            if reproduces(unpack(candidate), attempts):
                tagged = candidate
                partitions = max(2, partitions - 1)
                reduced = True
                break
            if attempts >= max_attempts:
                break
        if not reduced:
            if chunk == 1:
                break
            partitions = min(len(tagged), partitions * 2)
    return unpack(tagged), attempts
