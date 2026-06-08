# Server Engine Bridge

`sb_server_engine_bridge` is the private adapter target that lets `sb_server`
host the engine without exposing private engine module names through the server
product CMake boundary.

The public engine ABI remains `sb_engine`. Internal engine module targets remain
private build dependencies and are not deployable products.
