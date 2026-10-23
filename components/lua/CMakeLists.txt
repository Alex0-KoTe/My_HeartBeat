idf_component_register(
    SRCS
        "src/lapi.c"
        "src/lauxlib.c"
        "src/lbaselib.c"
        "src/lcode.c"
        "src/lcorolib.c"
        "src/lctype.c"
        "src/ldblib.c"
        "src/ldebug.c"
        "src/ldo.c"
        "src/ldump.c"
        "src/lfunc.c"
        "src/lgc.c"
        "src/linit.c"
        "src/liolib.c"
        "src/llex.c"
        "src/lmathlib.c"
        "src/lmem.c"
        "src/loadlib.c"
        "src/lobject.c"
        "src/lopcodes.c"
        "src/loslib.c"
        "src/lparser.c"
        "src/lstate.c"
        "src/lstring.c"
        "src/lstrlib.c"
        "src/ltable.c"
        "src/ltablib.c"
        "src/ltm.c"
        "src/lundump.c"
        "src/lutf8lib.c"
        "src/lvm.c"
        "src/lzio.c"
    INCLUDE_DIRS "include"
)

# Отключаем предупреждения для Lua
target_compile_options(${COMPONENT_LIB} PRIVATE -w)

# Макросы для ESP32
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    # LUA_USE_POSIX  # Отключаем POSIX для ESP32
)