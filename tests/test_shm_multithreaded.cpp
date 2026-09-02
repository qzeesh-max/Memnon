#undef NDEBUG
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <mutex>
#include <string>
#include <stdexcept>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include "segmented_interprocess/segmented_managed_memory.hpp"
#include "segmented_interprocess/segmented_offset_ptr.hpp"

using namespace segmented_interprocess;

struct Node {
    int id;
    segmented_offset_ptr<Node> next;
    
    Node(int i) : id(i), next(nullptr) {}
};

void test_cross_manager_exception() {
    try {
        std::cout << "Removing old SHM files\n" << std::flush;
        boost::interprocess::shared_memory_object::remove("test_cross_mgr_1");
        boost::interprocess::shared_memory_object::remove("test_cross_mgr_2");
        std::cout << "Removed old SHM files\n" << std::flush;
        
        {
            std::cout << "Creating mgr1\n" << std::flush;
            segmented_managed_memory mgr1("test_cross_mgr_1", segmented_interprocess::create_only, 1024 * 1024);
            std::cout << "Creating mgr2\n" << std::flush;
            segmented_managed_memory mgr2("test_cross_mgr_2", segmented_interprocess::create_only, 1024 * 1024);
            
            std::cout << "Constructing n1\n" << std::flush;
            Node* n1 = mgr1.construct<Node>("Node1", 1);
            std::cout << "Constructing n2\n" << std::flush;
            Node* n2 = mgr2.construct<Node>("Node2", 2);
            
            std::cout << "Assigning n1->next = n2\n" << std::flush;
            bool caught = false;
            try {
                n1->next = n2; // This should throw a runtime_error!
            } catch (const std::runtime_error&) {
                caught = true;
            }
            std::cout << "Assignment done, caught=" << caught << "\n" << std::flush;
            
            assert(caught && "Expected cross-manager pointer assignment to throw");
            
            // Stack to SHM
            Node stack_node(3);
            caught = false;
            try {
                n1->next = &stack_node; // Should throw because it points to unmanaged memory!
            } catch (const std::runtime_error&) {
                caught = true;
            }
            assert(caught && "Expected SHM pointer to unmanaged memory assignment to throw");
            
            // Unmanaged to SHM (this actually works because case B kicks in)
            stack_node.next = n1;
            assert(stack_node.next.get() == n1);
        }
        
        boost::interprocess::shared_memory_object::remove("test_cross_mgr_1");
        boost::interprocess::shared_memory_object::remove("test_cross_mgr_2");
        std::cout << "test_cross_manager_exception finished\n";
    } catch (const std::exception& e) {
        std::cout << "test_cross_manager_exception failed with exception: " << e.what() << "\n";
    }
}

void test_multiprocess_lazy_discovery() {
    boost::interprocess::shared_memory_object::remove("test_lazy_discovery");
    
    {
        segmented_managed_memory mgr1("test_lazy_discovery", segmented_interprocess::create_only, 65536);
        
        segmented_offset_ptr<Node>* root = mgr1.construct<segmented_offset_ptr<Node>>("Root", nullptr);
        
        Node* curr = mgr1.construct<Node>("Node0", 0);
        *root = curr;
        
        // Open second manager "process"
        segmented_managed_memory mgr2("test_lazy_discovery", segmented_interprocess::open_only);
        
        segmented_offset_ptr<Node>* root2 = mgr2.find<segmented_offset_ptr<Node>>("Root").first;
        assert(root2 != nullptr);
        assert((*root2)->id == 0);
        
        // mgr1 grows the memory (we use manual allocate and placement new to avoid slow string names)
        for (int i = 1; i < 5000; ++i) { // Enough to trigger multiple growths
            void* mem = mgr1.allocate(sizeof(Node));
            Node* next = new(mem) Node(i);
            curr->next = next;
            curr = next;
        }
        
        // mgr2 tries to traverse. It should lazy-discover the growths.
        Node* curr2 = (*root2).get();
        int count = 0;
        while (curr2 != nullptr) {
            assert(curr2->id == count);
            curr2 = curr2->next.get(); // This triggers get(), which triggers lazy discovery
            count++;
        }
        
        assert(count == 5000);
    }
    
    boost::interprocess::shared_memory_object::remove("test_lazy_discovery");
    std::cout << "test_multiprocess_lazy_discovery finished\n";
}

void test_concurrent_growth() {
    boost::interprocess::shared_memory_object::remove("test_concurrent_growth");
    
    {
        segmented_managed_memory mgr("test_concurrent_growth", segmented_interprocess::create_only, 65536);
        
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&, i]() {
                while (!start) {} // Spin wait
                
                for (int j = 0; j < 1000; ++j) {
                    try {
                        void* mem = mgr.allocate(sizeof(Node));
                        new(mem) Node(i * 1000 + j);
                    } catch (const std::bad_alloc&) {
                        assert(false);
                    }
                }
            });
        }
        
        start = true;
        for (auto& t : threads) {
            t.join();
        }
        
        // Ensure it actually grew
        assert(mgr.get_size() > 65536);
    }
    
    boost::interprocess::shared_memory_object::remove("test_concurrent_growth");
    std::cout << "test_concurrent_growth finished\n";
}

int main() {
    std::cout << "Running test_cross_manager_exception...\n" << std::flush;
    test_cross_manager_exception();
    
    std::cout << "Running test_multiprocess_lazy_discovery...\n" << std::flush;
    test_multiprocess_lazy_discovery();
    
    std::cout << "Running test_concurrent_growth...\n" << std::flush;
    test_concurrent_growth();
    
    std::cout << "All SHM multithreaded/multiprocess tests passed!\n" << std::flush;
    return 0;
}
