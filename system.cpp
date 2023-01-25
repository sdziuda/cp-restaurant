#include "system.hpp"

#include <utility>
#include <algorithm>
#include <iostream>

using namespace std;

CoasterPager::CoasterPager(unsigned int orderId) {
    this->orderId = orderId;
    this->ready = false;
    this->failed = false;
}

void CoasterPager::wait() const {
    std::mutex waiter;
    std::unique_lock<std::mutex> lock(waiter);
    std::condition_variable cond;

    cond.wait(lock, [this]() { return this->ready || this->failed; });

    if (this->failed) {
        throw FulfillmentFailure();
    }
}

void CoasterPager::wait(unsigned int timeout) const {
    std::mutex waiter;
    std::unique_lock<std::mutex> lock(waiter);
    std::condition_variable cond;

    cond.wait_for(lock, std::chrono::milliseconds(timeout), [this]() { return this->ready || this->failed; });

    if (this->failed) {
        throw FulfillmentFailure();
    }
}

unsigned int CoasterPager::getId() const {
    return this->orderId;
}

bool CoasterPager::isReady() const {
    return this->ready;
}

bool CoasterPager::isFailed() const {
    return this->failed;
}

void CoasterPager::setReady(bool val) {
    this->ready = val;
}

void CoasterPager::setFailed(bool val) {
    this->failed = val;
}

System::System(System::machines_t machines, unsigned int numberOfWorkers,
               unsigned int clientTimeout) {
    this->machines = std::move(machines);
    this->numberOfWorkers = numberOfWorkers;
    this->clientTimeout = clientTimeout;
    for (auto &machine: this->machines) {
        this->menu.push_back(machine.first);
    }
    this->pagers = std::map<unsigned int, std::unique_ptr<CoasterPager>>();
    this->orders = std::map<unsigned int, std::vector<std::string>>();
    this->ordersMade = std::map<unsigned int, std::vector<std::unique_ptr<Product>>>();
    this->orderCollected = std::map<unsigned int, bool>();
    this->abandonedOrdersId = std::vector<unsigned int>();
    this->ordersQueue = std::queue<unsigned int>();
    this->nextOrderId = 0;
    this->occupiedWorkers = 0;
    this->workersStarted = 0;
    this->running = true;

    for (auto &machine: this->machines) {
        machine.second->start();
    }
    for (unsigned int i = 0; i < numberOfWorkers; i++) {
        this->workers.emplace_back(std::thread(&System::worker, this));
    }

    std::unique_lock<std::mutex> lock(this->mutex);
    this->queue_for_workers.wait(lock, [this]() { return this->workersStarted >= this->numberOfWorkers; });
}

std::vector<WorkerReport> System::shutdown() {
    std::unique_lock<std::mutex> lock(this->mutex);

    this->running = false;
    this->queue_to_restaurant.notify_all();

    for (auto &worker: this->workers) {
        worker.join();
    }

    std::vector<WorkerReport> res = this->reports;

    for (auto &machine: this->machines) {
        machine.second->stop();
    }

    return res;
}

std::vector<std::string> System::getMenu() const {
    return menu;
}

std::vector<unsigned int> System::getPendingOrders() const {
    std::vector<unsigned int> currOrders;
    for (auto &pager: this->pagers) {
        currOrders.emplace_back(pager.first);
    }
    return currOrders;
}

std::unique_ptr<CoasterPager> System::order(std::vector<std::string> products) {
    std::unique_lock<std::mutex> lock(this->mutex);

    cout << "order: ";
    for (auto &product: products) {
        cout << product << " ";
    }
    cout << endl;

    this->queue_to_restaurant.wait(lock, [this]() { return this->occupiedWorkers < this->numberOfWorkers; });

    if (this->running) {
        for (auto &product: products) {
            if (std::find(this->menu.begin(), this->menu.end(), product) == this->menu.end()) {
                throw BadOrderException();
            }
        }

        auto orderId = this->nextOrderId++;
        this->orders.insert({orderId, products});
        this->ordersQueue.push(orderId);
        this->pagers.insert({orderId, std::make_unique<CoasterPager>(orderId)});
        auto pager = std::move(this->pagers[orderId]);

        cout << "order: " << orderId << " ";
        for (auto &product: products) {
            cout << product << " ";
        }
        cout << "-> pager: " << pager->getId() << endl;

        lock.unlock();

        return pager;
    } else {
        throw RestaurantClosedException();
    }
}

std::vector<std::unique_ptr<Product>> System::collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {
    std::unique_lock<std::mutex> lock(this->mutex);

    auto orderId = CoasterPager->getId();
    auto it = std::find(this->abandonedOrdersId.begin(), this->abandonedOrdersId.end(), orderId);

    if (this->pagers.find(orderId) == this->pagers.end()) {
        throw BadPagerException();
    } else if (!this->pagers[orderId]->isReady()) {
        throw OrderNotReadyException();
    } else if (this->pagers[orderId]->isFailed()) {
        throw FulfillmentFailure();
    } else if (it != this->abandonedOrdersId.end()) {
        throw OrderExpiredException();
    }

    this->orderCollected[orderId] = true;
    auto res = std::move(this->ordersMade[orderId]);

    lock.unlock();

    return res;
}

unsigned int System::getClientTimeout() const {
    return clientTimeout;
}

void System::worker() {
    std::vector<std::vector<std::string>> collectedOrders;
    std::vector<std::vector<std::string>> abandonedOrders;
    std::vector<std::vector<std::string>> failedOrders;
    std::vector<std::string> failedProducts;

    std::unique_lock<std::mutex> lock(this->mutex);
    this->workersStarted++;
    unsigned int id = this->workersStarted;
    cout << "worker(" << id << "/" << this->numberOfWorkers << "): started" << endl;

    if (this->workersStarted == this->numberOfWorkers) {
        this->queue_for_workers.notify_all();
    }
    lock.unlock();


    while (true) {
        lock.lock();
        this->queue_for_workers.wait(lock, [this]() {
            return !this->ordersQueue.empty() || !this->running;
        });

        if (!this->running && this->ordersQueue.empty()) {
            cout << "worker(" << id << "): creating report" << endl;
            WorkerReport report = {collectedOrders, abandonedOrders, failedOrders, failedProducts};
            this->reports.push_back(report);
            lock.unlock();
            break;
        }

        auto orderId = this->ordersQueue.front();
        auto order = this->orders[orderId];
        this->ordersQueue.pop();
        this->occupiedWorkers++;
        cout << "worker(" << id << "): order: " << orderId << " ";
        for (auto &product: order) {
            cout << product << " ";
        }
        cout << endl;
        lock.unlock();

        std::vector<std::unique_ptr<Product>> products;
        for (auto &product: order) {
            try {
                products.push_back(this->machines[product]->getProduct());
            } catch (MachineFailure &e) {
                failedProducts.push_back(product);
                failedOrders.push_back(order);

                lock.lock();
                this->pagers[orderId]->setFailed(true);
                this->orders.erase(orderId);
                this->pagers.erase(orderId);
                auto it = std::find(this->menu.begin(), this->menu.end(), product);
                if (it != this->menu.end()) {
                    this->menu.erase(it);
                }
                lock.unlock();

                for (auto &machine: this->machines) {
                    for (auto &p: products) {
                        try {
                            machine.second->returnProduct(std::move(p));
                            break;
                        } catch (BadProductException &e) {
                            continue;
                        }
                    }
                }

                break;
            }
        }

        lock.lock();
        if (failedProducts.empty()) {
            this->orderCollected.insert({orderId, false});
            this->ordersMade.insert({orderId, std::move(products)});
            this->pagers[orderId]->setReady(true);

            std::condition_variable cond;
            cond.wait_for(lock, std::chrono::milliseconds(this->clientTimeout), [this, orderId]() {
                return this->orderCollected[orderId];
            });

            if (!this->orderCollected[orderId]) {
                abandonedOrders.push_back(order);
                this->abandonedOrdersId.push_back(orderId);
                products = std::move(this->ordersMade[orderId]);
                this->ordersMade.erase(orderId);
                for (auto &machine: this->machines) {
                    for (auto &p: products) {
                        try {
                            machine.second->returnProduct(std::move(p));
                            break;
                        } catch (BadProductException &e) {
                            continue;
                        }
                    }
                }
            } else {
                collectedOrders.push_back(order);
            }

            this->orderCollected.erase(orderId);
            this->pagers.erase(orderId);
            this->orders.erase(orderId);
        }
        this->occupiedWorkers--;
        lock.unlock();
    }
}