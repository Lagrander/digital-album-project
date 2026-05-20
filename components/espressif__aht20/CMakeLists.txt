idf_component_register(
    SRCS "aht20.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES driver esp_driver_gpio
)

include(package_manager)
cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})
