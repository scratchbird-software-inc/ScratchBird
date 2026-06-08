# sb_parse_probe

Developer probe for the initial parser vertical slice.

Current scope:

- parse `SHOW VERSION` or `SHOW DATABASE`;
- bind using command-line context;
- lower to a logical SBLR/internal envelope;
- bridge the logical envelope to the engine internal API dispatch request contract;
- emit stage-tagged JSON.

This tool does not execute the envelope and does not call engine operation implementation. It only checks that the parser vertical slice can produce an engine-owned dispatch request for the minimal internal API surface.
