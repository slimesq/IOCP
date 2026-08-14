#include <atomic>
#include <ostream>
#include <thread>
#include "include/work.h"

void test1()
{
    std::thread thread1{work};
    std::thread thread2{work};
    thread1.join();
    thread2.join();
    std::cout << counter << std::endl;
}

void test2()
{
    std::atomic<int> value{10};
    // value.store(20);
    int result{value.load()};
    std::cout << result << std::endl;

    int oldValue{value.exchange(30)};
    std::cout << oldValue << std::endl;
    std::cout << value.load() << std::endl;
    std::cout << value.fetch_add(5) << std::endl;
    std::cout << value.load() << std::endl;
}

void test3()
{
    std::atomic<int> value{15};
    int expected{10};
    bool success{value.compare_exchange_strong(expected, 20)};
    std::cout << success << std::endl;
    std::cout << value.load() << std::endl;
    std::cout << expected << std::endl;
}

void test4()
{
    updateMax(10);
    std::cout << maxValue << std::endl;
    updateMax(5);
    std::cout << maxValue << std::endl;
}

int data{0};
std::atomic<bool> ready{false};

void producer()
{
    data = 42;
    ready.store(true, std::memory_order_release);
}

void consumer()
{
    while (!ready.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    std::cout << data << std::endl;
}

void test5()
{
    std::thread t1{producer};
    std::thread t2{consumer};
    t1.join();
    t2.join();
}

int main()
{
    // test1();
    // test2();
    // test3();
    // test5();

    std::atomic<int> value{0};
    std::cout << value.is_lock_free() << std::endl;
    std::cout << std::atomic<int>::is_always_lock_free << std::endl;

    std::atomic<int> a{10};
    // std::atomic<int> b{a};  // Call to deleted constructor of 'std::atomic<int>'

    return EXIT_SUCCESS;
}
