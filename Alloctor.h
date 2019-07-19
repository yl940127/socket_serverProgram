//
// Created by yl on 19-6-20.
//

#ifndef HELLOSOCKET_ALLOCTOR_H
#define HELLOSOCKET_ALLOCTOR_H

#include "stdlib.h"
void* operator new(size_t size);
void operator delete(void* p)noexcept;
void* operator new[](size_t size);
void operator delete[](void* p)noexcept;
void* mem_alloc(size_t size);
void mem_free(void* p);

#endif //HELLOSOCKET_ALLOCTOR_H
