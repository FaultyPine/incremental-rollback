@echo off

set SOURCES=main.cpp incremental_rb.cpp profiler.cpp mem.cpp tiny_arena.cpp job_system.cpp
set COMPILER_FLAGS= -g -O0 -DDEBUG

:: unpack cmd line args
for %%a in (%*) do set "%%a=1"


if "%release%"=="1" (
    echo Release mode...
    set COMPILER_FLAGS= -g -O3 -DNDEBUG 
)

clang %SOURCES% %COMPILER_FLAGS% -std=c++20 -o out.exe -Iexternal -Iexternal/mimalloc/include -march=native

IF %ERRORLEVEL% NEQ 0 (echo Error:%ERRORLEVEL% && exit /b)

echo Built successfully

if "%run%"=="1" (
    out.exe
)