idf_component_register(
    SRCS "task_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        freertos 
        esp_common 
        log
        lua_engine      
        mcp23017
       
)