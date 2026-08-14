#pragma once

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

std::atomic<int> counter{0};
void work()
{
    for (int i{0}; i < 1000000000; ++i)
    {
        // ++counter;
        // counter += 1;
        counter = counter.load() + 1;
    }
}

std::atomic<int> maxValue{0};
void updateMax(int newValue)
{
    int current{maxValue.load()};
    while (current < newValue && !maxValue.compare_exchange_strong(current, newValue))
        ;
}

