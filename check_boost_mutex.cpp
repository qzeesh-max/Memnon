#include <boost/interprocess/sync/interprocess_spinlock.hpp>
#include <boost/interprocess/sync/mutex_family.hpp>
#include <boost/interprocess/mem_algo/rbtree_best_fit.hpp>
#include <iostream>

struct spin_mutex_family {
    typedef boost::interprocess::interprocess_spinlock mutex_type;
    typedef boost::interprocess::interprocess_recursive_mutex recursive_mutex_type;
};

int main() {
    std::cout << "Compiles!\n";
    return 0;
}
