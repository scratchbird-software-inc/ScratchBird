# Time-Series Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for time-series commands.

Generation task: `ebnf_timeseries_statement`

## Production

```ebnf
time_series_op_stmt ::=
    "TIMESERIES" timeseries_action timeseries_target timeseries_payload? return_clause? statement_option_list? ;

timeseries_action ::=
      "QUERY"
    | "INSERT"
    | "DELETE"
    | "DOWNSAMPLE"
    | "RETAIN"
    | "GAPFILL"
    | "DESCRIBE" ;

timeseries_target ::=
    qualified_name ;

timeseries_payload ::=
      timeseries_query_payload
    | timeseries_insert_payload
    | timeseries_delete_payload
    | timeseries_policy_payload ;

timeseries_query_payload ::=
    time_window_clause multimodel_where_clause? bucket_clause? aggregate_clause? ;

timeseries_insert_payload ::=
    "SERIES" expression "AT" expression "VALUE" expression ;

timeseries_delete_payload ::=
    time_window_clause multimodel_where_clause? ;

timeseries_policy_payload ::=
    time_window_clause? statement_option_list? ;

time_window_clause ::=
    "BETWEEN" expression "AND" expression ;

bucket_clause ::=
    "BUCKET" expression ;

aggregate_clause ::=
    "AGGREGATE" expression ("," expression)* ;
```

## Meaning

`time_series_op_stmt` recognizes time-window query, sample mutation, downsample, retention, gap-fill, and inspection commands. Timestamp descriptors, window bounds, bucket alignment, interpolation, and late-sample behavior are bound before execution.

## Used By

| Parent Production |
| --- |
| multi_model_op_stmt |

## Child Productions

| Child Production |
| --- |
| qualified_name |
| expression |
| return_clause |
| statement_option_list |
| multimodel_where_clause |

## Binding Contract

The target must resolve to a time-series capable descriptor. Window bounds, series identity, timestamp precision, bucket size, aggregate descriptors, gap-fill behavior, and retention policy must be admitted. Mutations must be transactional and rollback-safe.

## Practical Notes

- Time-zone and timestamp precision are descriptor owned.
- Gap-fill status should be explicit when synthetic rows are returned.
- Retention and downsample commands must not remove samples visible to an active transaction.
