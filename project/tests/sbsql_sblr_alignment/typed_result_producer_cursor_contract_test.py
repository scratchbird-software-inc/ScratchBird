#!/usr/bin/env python3
"""Independent oracle for the Core producer cursor carrier lifecycle."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import sys

import yaml


TERMINAL_STATES = {"eos", "cancelled", "closed", "revoked"}


class ContractFailure(RuntimeError):
    pass


def require(condition: bool, detail: str) -> None:
    if not condition:
        raise ContractFailure(detail)


@dataclass(frozen=True)
class PullRequest:
    cursor_uuid: str = "cursor-1"
    carrier_generation: int = 7
    stream_uuid: str = "stream-1"
    stream_version: int = 1
    stream_generation: int = 9
    expected_batch_ordinal: int = 0
    maximum_rows: int = 4
    maximum_bytes: int = 4096
    timeout_millis: int = 1000


@dataclass(frozen=True)
class ProducerOutcome:
    kind: str
    row_count: int = 0
    packet_bytes: int = 0
    batch_ordinal: int = 0
    end_of_cursor: bool = False
    canonical_packet: bool = True
    cancellation_before_publication: bool = False
    cancellation_after_publication: bool = False


@dataclass(frozen=True)
class PullResult:
    ok: bool
    diagnostic: str = ""
    published_rows: int = 0
    published_bytes: int = 0


@dataclass
class ProducerCursorOracle:
    state: str = "open"
    next_batch_ordinal: int = 0
    position: int = 0
    release_count: int = 0
    producer_calls: int = 0
    cursor_uuid: str = "cursor-1"
    carrier_generation: int = 7
    stream_uuid: str = "stream-1"
    stream_version: int = 1
    stream_generation: int = 9
    maximum_rows: int = 8
    maximum_bytes: int = 8192
    grant_bytes: int = 8192
    owner_live: bool = True
    statement_receipt_live: bool = True
    snapshot_live: bool = True
    cancellation_receipt_live: bool = True
    resource_grant_live: bool = True
    descriptor_live: bool = True

    def _release(self, terminal: str) -> None:
        require(terminal in TERMINAL_STATES, "release requires terminal state")
        require(self.release_count == 0, "retained authority released twice")
        self.state = terminal
        self.release_count += 1

    def _refuse(self, diagnostic: str, revoke: bool = False) -> PullResult:
        if revoke:
            self._release("revoked")
        return PullResult(False, diagnostic)

    def pull(self, request: PullRequest, outcome: ProducerOutcome) -> PullResult:
        if self.state in TERMINAL_STATES or self.state != "open":
            return self._refuse("CURSOR.STALE")
        if (
            request.stream_version != 1
            or request.maximum_rows <= 0
            or request.maximum_bytes <= 0
            or request.timeout_millis <= 0
        ):
            return self._refuse("SB_ENGINE_STATUS_INVALID_ARGUMENT")
        if not self.owner_live:
            return self._refuse("SECURITY.ACCESS_DENIED", revoke=True)
        if not self.statement_receipt_live or not self.snapshot_live:
            return self._refuse("MGA.TRANSACTION.STALE", revoke=True)
        if not self.descriptor_live:
            return self._refuse("DATATYPE.DESCRIPTOR_INVALID", revoke=True)
        if (
            request.cursor_uuid != self.cursor_uuid
            or request.carrier_generation != self.carrier_generation
            or request.stream_uuid != self.stream_uuid
            or request.stream_version != self.stream_version
            or request.stream_generation != self.stream_generation
            or request.expected_batch_ordinal != self.next_batch_ordinal
        ):
            return self._refuse("CURSOR.STALE")
        if (
            not self.resource_grant_live
            or request.maximum_rows > self.maximum_rows
            or request.maximum_bytes > self.maximum_bytes
            or request.maximum_bytes > self.grant_bytes
        ):
            return self._refuse("RESOURCE.BUDGET_EXCEEDED")
        if not self.cancellation_receipt_live:
            self._release("cancelled")
            return PullResult(False, "PROCESS.CANCELLED")

        old_position = self.position
        old_ordinal = self.next_batch_ordinal
        self.state = "pulling"
        self.producer_calls += 1

        if outcome.cancellation_before_publication or outcome.kind == "cancelled":
            self.position = old_position
            self.next_batch_ordinal = old_ordinal
            self._release("cancelled")
            return PullResult(False, "PROCESS.CANCELLED")

        if outcome.kind == "refused":
            self.state = "open"
            return PullResult(False, "CURSOR.FETCH_FAILED")

        if outcome.kind == "empty_open":
            if (
                outcome.row_count != 0
                or outcome.packet_bytes != 0
                or outcome.end_of_cursor
            ):
                self.state = "open"
                return PullResult(False, "SB_ENGINE_STATUS_INVALID_ARGUMENT")
            self.state = "open"
            return PullResult(True)

        if outcome.kind == "empty_eos":
            if (
                outcome.row_count != 0
                or outcome.packet_bytes != 0
                or not outcome.end_of_cursor
            ):
                self.state = "open"
                return PullResult(False, "SB_ENGINE_STATUS_INVALID_ARGUMENT")
            self._release("eos")
            return PullResult(True)

        if outcome.kind != "batch":
            self.state = "open"
            return PullResult(False, "SB_ENGINE_STATUS_INVALID_ARGUMENT")
        if (
            outcome.row_count <= 0
            or outcome.row_count > request.maximum_rows
            or outcome.packet_bytes <= 0
            or outcome.packet_bytes > request.maximum_bytes
            or outcome.batch_ordinal != old_ordinal
            or not outcome.canonical_packet
        ):
            self.position = old_position
            self.next_batch_ordinal = old_ordinal
            self.state = "open"
            return PullResult(False, "DATATYPE.DESCRIPTOR_INVALID")

        # This is the sole publication barrier in the independent model.
        self.position = old_position + outcome.row_count
        self.next_batch_ordinal = old_ordinal + 1
        published = PullResult(
            True,
            published_rows=outcome.row_count,
            published_bytes=outcome.packet_bytes,
        )
        if outcome.end_of_cursor:
            self._release("eos")
        elif outcome.cancellation_after_publication:
            self._release("cancelled")
        else:
            self.state = "open"
        return published

    def close(self) -> PullResult:
        if self.state in TERMINAL_STATES or self.state != "open":
            return PullResult(False, "CURSOR.STALE")
        self._release("closed")
        return PullResult(True)

    def recover(self) -> PullResult:
        if self.state in TERMINAL_STATES:
            return PullResult(False, "CURSOR.STALE")
        self._release("revoked")
        return PullResult(True)


def load_authority(workspace: Path) -> dict:
    registry_path = workspace / "Specifications/Core/registries/result-shape-registry.yaml"
    registry = yaml.safe_load(registry_path.read_text(encoding="utf-8"))
    return registry["result_shape_registry"]["semantic_contracts"][
        "producer_cursor_carrier_v1"
    ]


def contract_shape_is_exact(workspace: Path) -> None:
    contract = load_authority(workspace)
    require(contract["version"] == 1, "producer carrier version drift")
    require(
        contract["visibility"] == "server_private_nonserializable",
        "producer carrier became serializable",
    )
    require(
        contract["state_machine"]["terminal_states"]
        == ["eos", "cancelled", "closed", "revoked"],
        "terminal state set drift",
    )
    require(
        contract["state_machine"]["allowed_transitions"]
        == {
            "open": ["pulling", "cancelled", "closed", "revoked"],
            "pulling": ["open", "eos", "cancelled", "revoked"],
            "eos": [],
            "cancelled": [],
            "closed": [],
            "revoked": [],
        },
        "producer transition matrix drift",
    )
    require(
        contract["retained_authority"]["required_handles"]
        == [
            "statement_context_receipt",
            "MGA_snapshot_pin",
            "cancellation_receipt",
            "resource_grant_receipt",
            "producer_state",
        ],
        "retained authority set drift",
    )
    forbidden = set(contract["forbidden"])
    require(
        {
            "aggregate_result_vector_as_cursor_source",
            "callback_recovery_or_plan_reexecution",
            "partial_batch_publication",
            "concurrent_pull_or_double_release",
        }
        <= forbidden,
        "producer forbidden-set lost a safety invariant",
    )

    native_path = workspace / (
        "Specifications/Core/chapters/wire-ipc/native-wire/"
        "appendix-native-type-parameter-result-metadata-layout.md"
    )
    native = native_path.read_text(encoding="utf-8")
    for anchor in (
        "QUERY-EXECUTE-PRODUCER-CURSOR-CARRIER-V1",
        "TypedResultProducerCursorCarrierV1",
        "PullTypedResultProducerCursorV1",
        "at most one mutation operation on a carrier at a",
        "private producer callback and its in-memory state never recover",
        "Exactly one terminal transition wins",
    ):
        require(anchor in native, f"native producer contract missing {anchor!r}")


def bounded_pull_and_atomicity() -> None:
    cursor = ProducerCursorOracle()
    over_rows = PullRequest(maximum_rows=9)
    refused = cursor.pull(over_rows, ProducerOutcome("batch", 1, 64))
    require(
        not refused.ok
        and refused.diagnostic == "RESOURCE.BUDGET_EXCEEDED"
        and cursor.producer_calls == 0
        and cursor.position == 0
        and cursor.next_batch_ordinal == 0,
        "over-bound request entered producer or advanced state",
    )

    first = cursor.pull(
        PullRequest(),
        ProducerOutcome("batch", row_count=2, packet_bytes=256),
    )
    require(
        first.ok
        and first.published_rows == 2
        and cursor.position == 2
        and cursor.next_batch_ordinal == 1
        and cursor.state == "open",
        "first bounded typed batch was not atomically published",
    )

    empty = cursor.pull(
        PullRequest(expected_batch_ordinal=1), ProducerOutcome("empty_open")
    )
    require(
        empty.ok
        and cursor.position == 2
        and cursor.next_batch_ordinal == 1,
        "empty nonterminal poll advanced cursor state",
    )

    malformed = cursor.pull(
        PullRequest(expected_batch_ordinal=1),
        ProducerOutcome(
            "batch",
            row_count=1,
            packet_bytes=128,
            batch_ordinal=1,
            canonical_packet=False,
        ),
    )
    require(
        not malformed.ok
        and malformed.diagnostic == "DATATYPE.DESCRIPTOR_INVALID"
        and cursor.position == 2
        and cursor.next_batch_ordinal == 1
        and cursor.state == "open",
        "malformed staged batch leaked a partial state transition",
    )

    terminal = cursor.pull(
        PullRequest(expected_batch_ordinal=1),
        ProducerOutcome(
            "batch",
            row_count=1,
            packet_bytes=128,
            batch_ordinal=1,
            end_of_cursor=True,
        ),
    )
    require(
        terminal.ok
        and terminal.published_rows == 1
        and cursor.state == "eos"
        and cursor.next_batch_ordinal == 2
        and cursor.release_count == 1,
        "terminal typed batch did not publish and release exactly once",
    )
    replay = cursor.pull(
        PullRequest(expected_batch_ordinal=2), ProducerOutcome("empty_open")
    )
    require(
        not replay.ok
        and replay.diagnostic == "CURSOR.STALE"
        and cursor.release_count == 1,
        "post-EOS replay was admitted or released twice",
    )


def cancellation_close_and_recovery() -> None:
    cancelled = ProducerCursorOracle()
    result = cancelled.pull(
        PullRequest(),
        ProducerOutcome(
            "batch",
            row_count=2,
            packet_bytes=256,
            cancellation_before_publication=True,
        ),
    )
    require(
        not result.ok
        and result.diagnostic == "PROCESS.CANCELLED"
        and cancelled.state == "cancelled"
        and cancelled.position == 0
        and cancelled.next_batch_ordinal == 0
        and cancelled.release_count == 1,
        "pre-publication cancellation exposed or advanced a batch",
    )

    after_barrier = ProducerCursorOracle()
    published = after_barrier.pull(
        PullRequest(),
        ProducerOutcome(
            "batch",
            row_count=1,
            packet_bytes=128,
            cancellation_after_publication=True,
        ),
    )
    require(
        published.ok
        and published.published_rows == 1
        and after_barrier.position == 1
        and after_barrier.next_batch_ordinal == 1
        and after_barrier.state == "cancelled"
        and after_barrier.release_count == 1,
        "post-barrier cancellation retracted or duplicated a published batch",
    )

    closed = ProducerCursorOracle()
    require(closed.close().ok, "valid explicit close was refused")
    require(
        not closed.close().ok and closed.release_count == 1,
        "close race released retained authority twice",
    )

    recovered = ProducerCursorOracle()
    require(recovered.recover().ok, "open carrier recovery revoke failed")
    require(
        recovered.state == "revoked"
        and recovered.release_count == 1
        and recovered.producer_calls == 0,
        "recovery reconstructed or invoked a producer callback",
    )
    require(
        not recovered.pull(PullRequest(), ProducerOutcome("empty_open")).ok
        and recovered.release_count == 1,
        "recovered carrier replay was admitted or released twice",
    )


def immutable_authority_refusals() -> None:
    crossed = ProducerCursorOracle()
    result = crossed.pull(
        PullRequest(stream_generation=10), ProducerOutcome("empty_open")
    )
    require(
        not result.ok
        and result.diagnostic == "CURSOR.STALE"
        and crossed.producer_calls == 0
        and crossed.state == "open",
        "crossed stream generation entered producer or revoked live authority",
    )

    stale_snapshot = ProducerCursorOracle(snapshot_live=False)
    result = stale_snapshot.pull(PullRequest(), ProducerOutcome("empty_open"))
    require(
        not result.ok
        and result.diagnostic == "MGA.TRANSACTION.STALE"
        and stale_snapshot.state == "revoked"
        and stale_snapshot.release_count == 1
        and stale_snapshot.producer_calls == 0,
        "stale MGA snapshot entered producer or remained live",
    )

    stale_descriptor = ProducerCursorOracle(descriptor_live=False)
    result = stale_descriptor.pull(PullRequest(), ProducerOutcome("empty_open"))
    require(
        not result.ok
        and result.diagnostic == "DATATYPE.DESCRIPTOR_INVALID"
        and stale_descriptor.state == "revoked"
        and stale_descriptor.release_count == 1,
        "stale descriptor did not revoke the producer carrier",
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace-root", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        contract_shape_is_exact(args.workspace_root.resolve())
        bounded_pull_and_atomicity()
        cancellation_close_and_recovery()
        immutable_authority_refusals()
    except (ContractFailure, KeyError, OSError, TypeError, yaml.YAMLError) as error:
        print(f"typed result producer cursor contract failed: {error}", file=sys.stderr)
        return 1
    print("typed_result_producer_cursor_contract=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
