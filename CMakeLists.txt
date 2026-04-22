cmake_minimum_required(VERSION 3.16)

# Pull in LVGL and GT911 from the component manager before project() is called
# (idf_component.yml handles the actual fetch)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(wled_controller)

