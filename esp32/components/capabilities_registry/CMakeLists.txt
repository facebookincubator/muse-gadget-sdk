# The device registry (include/capabilities_registry.h): drivers describe their devices,
# apps and device.guide read them. Small and dependency-free, so board files,
# Link's drivers and the apps runtime can all use it.
# Built only with CONFIG_CAPABILITIES_REGISTRY (WebAssembly apps select it); the
# requirement on it stays unconditional, as requirements resolve before Kconfig.
set(srcs)
if(CONFIG_CAPABILITIES_REGISTRY)
    list(APPEND srcs "capabilities_registry.c")
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer
)
