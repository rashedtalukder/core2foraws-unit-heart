cmake_minimum_required( VERSION 3.16.0 )

# core2foraws-unit-pahub is listed unconditionally: ESP-IDF expands component
# REQUIRES before Kconfig is processed, so it cannot be gated on
# CONFIG_UNIT_HEART_USE_PAHUB. Unused PA Hub code is dropped at link time.
idf_component_register(
    SRCS "unit_heart.c"
    INCLUDE_DIRS "./include"
    REQUIRES "Core2-for-AWS-IoT-Kit" "core2foraws-unit-pahub"
)
