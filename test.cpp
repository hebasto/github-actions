#include <iostream>
#include <locale>
int main()
{
    try {
        std::locale("");
    } catch (const std::runtime_error& e) {
        std::cout << e.what() << '\n';
    }
}
