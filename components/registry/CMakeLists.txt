# Kept separate from `shell` on purpose: both the shell (to list apps) and the
# widget runtime (to open one) need this, and having it live in either of them
# would make those two components depend on each other.
idf_component_register(
    SRCS
        "app_registry.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        bsp
        json
)
