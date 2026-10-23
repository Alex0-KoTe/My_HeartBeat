idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES 
        filesystem
        web_server
        lua_engine
        led_driver
        nvs_flash
        mcp23017
)