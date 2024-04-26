@echo off

set SOURCES=main.cpp profiler.cpp mem.cpp tiny_arena.cpp job_system.cpp

set COMPILER_FLAGS= -g -O0 -DDEBUG
if ["%2"]==["release"] (
    set COMPILER_FLAGS= -g -O3 -DNDEBUG 
)

clang %SOURCES% %COMPILER_FLAGS% -std=c++20 -o out.exe -Iexternal -Iexternal/mimalloc/include -march=native

IF %ERRORLEVEL% NEQ 0 (echo Error:%ERRORLEVEL% && exit /b)

echo Built successfully

if ["%1"]==["run"] (
    out.exe
)