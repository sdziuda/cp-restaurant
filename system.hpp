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

class CoasterPager
{
public:
    explicit CoasterPager(unsigned int orderId);

    void wait() const;

    void wait(unsigned int timeout) const;

    [[nodiscard]] unsigned int getId() const;

    [[nodiscard]] bool isReady() const;

    [[nodiscard]] bool isFailed() const;

    void setReady(bool val);

    void setFailed(bool val);

private:
    unsigned int orderId;
    bool ready;
    bool failed;
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
    std::map<unsigned int, std::unique_ptr<CoasterPager>> pagers;
    std::map<unsigned int, std::vector<std::string>> orders;
    std::map<unsigned int, std::vector<std::unique_ptr<Product>>> ordersMade;
    std::map<unsigned int, bool> orderCollected;
    std::vector<unsigned int> abandonedOrdersId;
    std::queue<unsigned int> ordersQueue;
    unsigned int nextOrderId;

    std::vector<std::thread> workers;
    std::vector<WorkerReport> reports;
    unsigned int workersStarted;
    unsigned int occupiedWorkers;

    bool running;

    std::mutex mutex;
    std::condition_variable queue_to_restaurant;
    std::condition_variable queue_for_workers;

    void worker();
};

#endif // SYSTEM_HPP