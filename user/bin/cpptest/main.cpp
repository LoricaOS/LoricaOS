#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <memory>

struct Shape { virtual ~Shape() = default; virtual std::string name() const = 0; };
struct Circle : Shape { std::string name() const override { return "circle"; } };

static int risky(int x) { if (x < 0) throw std::runtime_error("negative"); return x * 2; }

int main() {
    std::vector<std::string> v;
    for (int i = 0; i < 5; i++) v.push_back("item" + std::to_string(i));
    std::string joined;
    for (auto& s : v) joined += s + " ";
    std::cout << "vec: " << joined << "\n";

    std::unique_ptr<Shape> sh = std::make_unique<Circle>();
    std::cout << "poly: " << sh->name() << "\n";

    try { risky(-1); } catch (const std::exception& e) { std::cout << "caught: " << e.what() << "\n"; }
    std::cout << "doubled: " << risky(21) << "\n";

    std::cout << "CPPTEST_OK\n";
    return 0;
}
