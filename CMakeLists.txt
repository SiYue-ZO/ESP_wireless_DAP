# This repository contains two independent ESP-IDF projects.
# Build from transmitter/ or receiver/, not from the repository root.
cmake_minimum_required(VERSION 3.22)
message(FATAL_ERROR "Choose a firmware project: cd transmitter or cd receiver")
