#include "pagetree.h"
#include "util.h"
#include "mem.h"
#include "profiler.h"


inline u32 LeftChild(u32 idx)
{
    return 2 * idx + 1;
}
inline u32 RightChild(u32 idx)
{
    return 2 * idx + 2;
}

// TODO: rebalancing
template <typename T>
void PageTree<T>::Insert(T buf)
{
    PROFILE_FUNCTION();
    if (pages.empty())
    {
        pages.resize(100, {});
        pages.push_back(buf);
        return;
    }
    if (pages.size()+1 >= pages.capacity())
    {
        pages.resize(pages.capacity() * 2); // this hopefully inits to 0. 
    }
    u32 pagesSize = pages.size();
    u32 idx = 0;
    while (idx < pagesSize && pages[idx])
    {
        T& currentNode = pages[idx];
        if (buf < currentNode)
        {
            idx = LeftChild(idx);
        }
        else if (buf > currentNode)
        {
            idx = RightChild(idx);
        }
        else
        {
            printf("WARNING: tried to insert two of the same address into PageTree. Ignoring...");
            return;
        }
    }
    pages[idx] = buf;
}

template <typename T>
bool PageTree<T>::Find(T bufToFind, T& out) const
{
    PROFILE_FUNCTION();
    if (pages.empty()) return false;
    u32 idx = 0;
    u32 numPages = pages.size();
    T currentNode = pages[idx];
    while (currentNode && idx < numPages)
    {
        if (bufToFind < currentNode)
        {
            idx = LeftChild(idx);
        }
        else if (bufToFind > currentNode)
        {
            idx = RightChild(idx);
        }
        else
        {
            out = currentNode;
            return true;
        }
        if (idx >= numPages) break;
        currentNode = pages[idx];
    }
    return false;
}

template <typename T>
static void HelpPrint(const std::vector<T>& pages, u32 idx)
{
    if (idx >= pages.size()) return;
    if (pages[idx])
    {
        pages[idx].Print();
        for (u32 i = 0; i < sqrt(idx); i++)
        {
            printf("\t");
        }
        printf("\n");
    }
    HelpPrint(pages, LeftChild(idx));
    HelpPrint(pages, RightChild(idx));
}

template <typename T>
void PageTree<T>::Print() const
{
    u32 pagesSize = pages.size();
    u32 idx = 0;
    HelpPrint(pages, idx);
}

template <typename T>
void PageTree<T>::Clear()
{
    pages.clear();
}