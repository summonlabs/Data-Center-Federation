# Data Center Federation - downstream consumer.
# Copyright 2026 Summon Software Labs. Apache License 2.0.
#
# This project is deliberately independent of the federation build tree. It
# finds the installed package through CMAKE_PREFIX_PATH and links the exported
# namespaced target, which is what a real downstream consumer does.
cmake_minimum_required(VERSION 3.21)

project(dcf-consumer
  VERSION 1.0.0
  DESCRIPTION "Independent consumer of the installed Data Center Federation package"
  LANGUAGES CXX)

find_package(DataCenterFederation 1.0 REQUIRED CONFIG)

add_executable(dcf-consumer consumer_main.cpp)
target_link_libraries(dcf-consumer PRIVATE DataCenterFederation::dcf)

set_target_properties(dcf-consumer PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
