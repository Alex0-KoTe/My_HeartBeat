idf_component_register(
    SRCS "lua_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        filesystem
        lua
        mcp23017
        task_manager

)