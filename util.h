#pragma once

#include <iostream>
#include <string>
#include <chrono>
#include <ctime>


typedef unsigned char u8;
typedef char s8;
typedef unsigned short u16;
typedef short s16;
typedef unsigned int u32;
typedef int s32;
typedef long long s64;
typedef unsigned long long u64;
typedef float f32;
typedef double f64;



#define KILOBYTES_BYTES(kb) (kb*1024)
#define MEGABYTES_BYTES(mb) (mb*KILOBYTES_BYTES(1024))
#define GIGABYTES_BYTES(gb) (gb*MEGABYTES_BYTES(1024))


u32 HashBytes(char* data, u32 size)
{
    // FNV-1 hash -> doing the *PRIME, and THEN the XOR.
    // FNV-1a hash -> doing the XOR, then the *PRIME
    // https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
    constexpr u32 FNV_offset_basis = 0x811c9dc5;
    constexpr u32 FNV_prime = 0x01000193;
    u32 hash = FNV_offset_basis;
    for (u32 i = 0; i < size; i++)
    {
        u8 byte_of_data = data[i];
        hash = hash ^ byte_of_data;
        hash = hash * FNV_prime;
    }
    return hash;
}