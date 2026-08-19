#include <locale>
int main() { for (int i = 0; i < 100000; ++i) std::locale(""); }
