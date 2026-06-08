# Agent Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for agent inspection, lifecycle, policy, action, and override commands.

Generation task: `ebnf_agent_statement`

## Production

```ebnf
agent_statement ::=
      show_agent_statement
    | alter_agent_statement
    | create_agent_override_statement
    | drop_agent_override_statement ;

show_agent_statement ::=
      "SHOW" "AGENTS" agent_filter? agent_show_option_list?
    | "SHOW" "AGENT" agent_ref agent_inspection_target? agent_show_option_list?
    | "SHOW" "AGENT" "ACTIONS" agent_show_option_list?
    | "SHOW" "AGENT" "OVERRIDES" agent_show_option_list? ;

agent_filter ::=
    "WHERE" "AGENT_TYPE" "=" agent_type_ref ;

agent_inspection_target ::=
      "METRICS"
    | "POLICY"
    | "EVIDENCE"
    | "AUDIT"
    | "ACTIONS"
    | "OVERRIDES" ;

alter_agent_statement ::=
      "ALTER" "AGENT" agent_ref agent_lifecycle_action agent_reason_clause? agent_option_list?
    | "ALTER" "AGENT" agent_ref agent_policy_action agent_option_list?
    | "ALTER" "AGENT" "ACTION" uuid_ref agent_action_action agent_reason_clause? agent_option_list? ;

agent_lifecycle_action ::=
      "START"
    | "STOP"
    | "PAUSE"
    | "RESUME"
    | "DRAIN"
    | "RESTART"
    | "ENABLE"
    | "DISABLE"
    | "RUN"
    | "DRY" "RUN"
    | "QUARANTINE"
    | "UNQUARANTINE"
    | "SET" "MODE" agent_activation_profile ;

agent_policy_action ::=
      "ATTACH" "POLICY" policy_ref
    | "DETACH" "POLICY" policy_ref
    | "VALIDATE" "POLICY" policy_ref
    | "SIMULATE" "POLICY" policy_ref
    | "APPLY" "POLICY" policy_ref
    | "ROLLBACK" "POLICY" policy_ref ;

agent_action_action ::=
      "APPROVE"
    | "CANCEL"
    | "RETRY"
    | "SUPPRESS" ;

create_agent_override_statement ::=
    "CREATE" "AGENT" "OVERRIDE" "FOR" agent_ref agent_override_action
    "UNTIL" expression agent_reason_clause agent_option_list? ;

drop_agent_override_statement ::=
    "DROP" "AGENT" "OVERRIDE" uuid_ref agent_option_list? ;

agent_override_action ::=
      "SUPPRESS" identifier
    | "APPROVE" identifier
    | "SET" "MODE" agent_activation_profile ;

agent_activation_profile ::=
      "DISABLED"
    | "OBSERVE_ONLY"
    | "RECOMMEND_ONLY"
    | "DRY_RUN"
    | "LIVE_ACTION" ;

agent_ref ::=
      agent_type_ref
    | uuid_ref ;

agent_type_ref ::=
    identifier ;

agent_reason_clause ::=
    "REASON" identifier ;

agent_show_option_list ::=
    "WITH" option ("," option)* ;

agent_option_list ::=
    "WITH" option ("," option)* ;
```

## Meaning

`agent_statement` covers SBsql agent inspection, lifecycle control, policy attachment, policy simulation, action approval, action cancellation, and operator overrides. The grammar recognizes shape only. The engine agent management route owns authority, policy validation, evidence persistence, and fail-closed behavior.

## Used By

| Parent Production |
| --- |
| management_statement |
| native_statement |
| statement |

## Child Productions

| Child Production |
| --- |
| identifier |
| uuid_ref |
| policy_ref |
| expression |
| option |

## Binding Contract

The binder must resolve the agent type or UUID, scope, policy, action UUID, override UUID, reason, options, and result descriptor. Agent management routes require explicit rights and must apply sandbox, disclosure, policy, feature, metric, lifecycle, lease, cooldown, and approval gates.

## Practical Notes

- Agent statements do not require transaction finality for runtime state inspection.
- Agent actions that touch durable database state must route through the engine-owned subsystem that owns that state.
- Cluster agent statements are cluster-gated and fail closed without admitted cluster authority.
- Parser packages never execute agent action directly.
