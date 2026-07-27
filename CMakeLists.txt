cmake_minimum_required(VERSION 3.12)
project(CheckCapnpPackage CXX)
find_package(CapnProto 0.9)
if(NOT CapnProto_FOUND)
  message(
    "Cap'n Proto is required but was not found.\n"
  )
endif()
