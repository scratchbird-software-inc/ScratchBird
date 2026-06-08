# Recreate Function EBNF Production

This page is part of the SBsql Language Reference Manual. It explains the user-facing grammar contract for replacing a function definition through a single lifecycle route.

Generation task: `ebnf_recreate_function`

## Production

```ebnf
recreate_function ::=
    "RECREATE" "FUNCTION" function_signature
    "RETURNS" function_return_descriptor
    function_attribute_list?
    "AS" function_body ;
```

## Meaning

`recreate_function` replaces the visible function definition after parsing, binding, encoding, dependency validation, security checks, and invalidation. It is not a shortcut for storing new text over old text.

## Used By

| Parent Production |
| --- |
| function_lifecycle_statement |
| recreate_statement |

## Child Productions

| Child Production |
| --- |
| function_signature |
| function_return_descriptor |
| function_attribute_list |
| function_body |

## Binding Contract

The recreate route must either produce one complete, admitted replacement definition or leave the prior visible definition unchanged. It must fail closed if dependencies, grants, executable encoding, or transaction state make replacement unsafe.

## Practical Notes

- Replacement is transactional and rollback-safe.
- Dependent objects must be revalidated, invalidated, or refused.
- The recreated function retains or replaces durable identity only according to the documented catalog policy for the active profile.
