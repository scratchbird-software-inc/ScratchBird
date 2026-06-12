# Graph Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for graph commands.

Generation task: `ebnf_graph_statement`

## Production

```ebnf
graph_op_stmt ::=
    "GRAPH" graph_action graph_target graph_payload? return_clause? statement_option_list? ;

graph_action ::=
      "MATCH"
    | "TRAVERSE"
    | "SHORTEST_PATH"
    | "CREATE_NODE"
    | "CREATE_EDGE"
    | "UPDATE_NODE"
    | "UPDATE_EDGE"
    | "DELETE_NODE"
    | "DELETE_EDGE"
    | "VALIDATE" ;

graph_target ::=
    qualified_name ;

graph_payload ::=
      graph_match_payload
    | graph_traverse_payload
    | graph_path_payload
    | graph_mutation_payload ;

graph_match_payload ::=
    "PATTERN" graph_pattern multimodel_where_clause? ;

graph_traverse_payload ::=
    "FROM" "NODE" expression "OVER" "EDGE_TYPE" expression depth_clause? ;

graph_path_payload ::=
    "FROM" "NODE" expression "TO" "NODE" expression ("OVER" "EDGE_TYPE" expression)? ;

graph_mutation_payload ::=
    graph_property_list? ;

depth_clause ::=
    "DEPTH" expression "TO" expression ;

graph_property_list ::=
    "SET" graph_property_assignment ("," graph_property_assignment)* ;

graph_property_assignment ::=
    identifier "=" expression ;
```

## Meaning

`graph_op_stmt` recognizes graph match, traversal, path, mutation, and validation commands. Nodes, edges, paths, graph properties, direction, and traversal depth must bind to graph descriptors before execution.

## Used By

| Parent Production |
| --- |
| nosql_statement (multi_model family) |

## Child Productions

| Child Production |
| --- |
| qualified_name |
| graph_pattern |
| expression |
| identifier |
| return_clause |
| statement_option_list |
| multimodel_where_clause |

## Binding Contract

The target must resolve to a graph-capable descriptor. Node identity, edge identity, endpoint direction, path shape, property descriptors, and graph constraints must be validated. Traversal evidence is not final row authority.

## Practical Notes

- Stable path order exists only when the operation descriptor defines it or an outer ordering surface is used.
- Node and edge deletion must obey graph constraints.
- Property values bind to declared descriptors.
