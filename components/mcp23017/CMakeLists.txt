idf_component_register(
    SRCS "mcp23017.c" "lua_bindings.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        driver            # <-- ОСТАВЛЯЕМ для legacy driver/i2c.h
        esp_driver_gpio   # <-- ДОБАВЛЯЕМ для driver/gpio.h в v6
        freertos 
        lua_engine
)