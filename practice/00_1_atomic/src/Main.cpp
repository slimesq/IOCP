#include <atomic>
#include <cstdlib>
#include <iostream>

int main()
{
    std::atomic_int value{0};

    std::cout << "Atomic practice environment is ready.\n";
    std::cout << "Initial atomic value: " << value.load() << '\n';
    std::cout << "Edit src/Main.cpp to begin the first exercise.\n";

    return EXIT_SUCCESS;
}
