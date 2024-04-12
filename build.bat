@echo off


clang main.cpp -g -o out.exe

if ["%1"]==["run"] (
    out.exe
)