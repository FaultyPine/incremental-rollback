@echo off

set SOURCES=main.cpp profiler.cpp mem.cpp

clang %SOURCES% -g -o out.exe -Iexternal -Iexternal/mimalloc/include

if ["%1"]==["run"] (
    out.exe
)