@echo off


clang main.cpp profiler.cpp -g -o out.exe -Iexternal

if ["%1"]==["run"] (
    out.exe
)