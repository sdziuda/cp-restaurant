#ifndef SYSTEM_HPP
#define SYSTEM_HPP

#include <exception>
#include <vector>
#include <unordered_map>
#include <functional>
#include <future>
#include <queue>
#include <map>

#include "machine.hpp"

class FulfillmentFailure : public std::exception
{
};

class OrderNotReadyException : public std::exception
{
};

class BadOrderException : public std::exception
{
};

class BadPagerException : public std::exception
{
};

class OrderExpiredException : public std::exception
{
};

class RestaurantClosedException : public std::exception
{
};

struct WorkerReport
{
    std::vector<std::vector<std::string>> collectedOrders;
    std::vector<std::vector<std::string>> abandonedOrders;
    std::vector<std::vector<std::string>> failedOrders;
    std::vector<std::string> failedProducts;
};

struct Pager_variables {
    std::mutex waiter;
    std::condition_variable cond;
    bool ready = false;
    bool failed = false;
};

class CoasterPager
{
public:
    void wait() const;

    void wait(unsigned int timeout) const;

    [[nodiscard]] unsigned int getId() const;

    [[nodiscard]] bool isReady() const;

private:
    friend class System;

    static unsigned int nextId;
    unsigned int orderId = nextId++;
    std::shared_ptr<Pager_variables> vars = std::make_shared<Pager_variables>();
};

class System
{
public:
    typedef std::unordered_map<std::string, std::shared_ptr<Machine>> machines_t;
    
    System(machines_t machines, unsigned int numberOfWorkers, unsigned int clientTimeout);

    std::vector<WorkerReport> shutdown();

    std::vector<std::string> getMenu() const;

    std::vector<unsigned int> getPendingOrders() const;

    std::unique_ptr<CoasterPager> order(std::vector<std::string> products);

    std::vector<std::unique_ptr<Product>> collectOrder(std::unique_ptr<CoasterPager> CoasterPager);

    unsigned int getClientTimeout() const;

private:
    machines_t machines;
    unsigned int numberOfWorkers;
    unsigned int clientTimeout;

    std::vector<std::string> menu;
    std::unordered_map<unsigned int, std::shared_ptr<Pager_variables>> pagers;
    std::unordered_map<unsigned int, std::vector<std::string>> orders;
    std::vector<unsigned int> pendingOrders;
    std::unordered_map<unsigned int, std::vector<std::unique_ptr<Product>>> ordersMade;
    std::unordered_map<unsigned int, bool> orderCollected;
    std::vector<unsigned int> abandonedOrdersId;
    std::queue<unsigned int> ordersQueue;
    unsigned int nextOrderId;

    std::vector<std::thread> workers;
    std::vector<WorkerReport> reports;
    unsigned int workersStarted;
    unsigned int occupiedWorkers;

    bool running;

    std::priority_queue<unsigned int, std::vector<unsigned int>, std::greater<>> queue_for_orders;

    mutable std::mutex mutex;
    std::condition_variable wait_to_restaurant;
    std::condition_variable queue_for_workers;
    std::map<unsigned int, std::condition_variable> queue_for_pagers;
    std::condition_variable queue_for_reports;
    std::condition_variable all_workers_started;

    void worker();
};

#endif // SYSTEM_HPP