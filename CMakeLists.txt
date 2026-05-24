cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wled_controller)

# ── SPIFFS storage image ───────────────────────────────────────────────────────
# Builds a SPIFFS image from the  spiffs_data/  directory and teaches
# CMake about a  storage-flash  target so you can push a new devices.cfg
# to the device without reflashing firmware:
#
#   idf.py storage-flash
#
# The partition label "storage" matches the name in partitions.csv.
spiffs_create_partition_image(storage spiffs_data FLASH_IN_PROJECT)