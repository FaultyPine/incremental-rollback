#pragma once

#include <vector>

// quick and simple tree structure to partition our address space
// for quick lookup. Uses vector as backing stretchy buffer
// Built on the assumption that our allocations are all fixed size (1 page large).
// could be modified to accept variable-size allocations too, but this makes things a lot simpler

// TODO: balance the tree. Prioritize lookup speed - they need to be very very fast

template <typename T>
struct PageTree
{
    void Insert(T page);
    bool Find(T bufToFind, T& out) const;
    void Print() const;
    void Clear();
    std::vector<T> pages = {};
};

#include "pagetree.cpp"