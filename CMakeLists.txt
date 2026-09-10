cmake_minimum_required(VERSION 3.22)
project(PIPE2)

include(CheckCXXSymbolExists)
check_cxx_symbol_exists(pipe2 "unistd.h" HAVE_DECL_PIPE2)
