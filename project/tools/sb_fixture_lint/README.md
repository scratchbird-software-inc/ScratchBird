# sb_fixture_lint

Golden conformance fixture schema checker for the initial parser vertical slice.

Current scope:

- check required fixture fields;
- distinguish accepted/alias/refusal fixture shape;
- require parser command fixtures to declare expected engine API bridge evidence;
- reject executable operation expectations in refusal fixtures;
- reject private authority terms in public fixtures;
- emit deterministic JSON report.

Controlling contract: `public_contract_snapshot`.
