#include "system.hpp"

#include <utility>
#include <algorithm>
#include <iostream>

using namespace std;

const bool debug = false;

unsigned int CoasterPager::nextId = 0;

void CoasterPager::wait() const {
    std::unique_lock<std::mutex> lock(this->vars->waiter);

    if (!this->vars->ready && !this->vars->failed) {
        vars->cond.wait(lock, [this]() { return this->vars->ready || this->vars->failed; });
    }

    if (this->vars->failed) {
        throw FulfillmentFailure();
    }
}

void CoasterPager::wait(unsigned int timeout) const {
    std::unique_lock<std::mutex> lock(this->vars->waiter);

    if (!this->vars->ready && !this->vars->failed) {
        vars->cond.wait_for(lock, std::chrono::milliseconds(timeout),
                      [this]() { return this->vars->ready || this->vars->failed; });
    }

    if (this->vars->failed) {
        throw FulfillmentFailure();
    }
}

unsigned int CoasterPager::getId() const {
    return this->orderId;
}

bool CoasterPager::isReady() const {
    return this->vars->ready;
}

System::System(System::machines_t machines, unsigned int numberOfWorkers,
               unsigned int clientTimeout) {
    this->machines = std::move(machines);
    this->numberOfWorkers = numberOfWorkers;
    this->clientTimeout = clientTimeout;
    for (auto &machine: this->machines) {
        this->menu.push_back(machine.first);
    }
    this->pagers = std::unordered_map<unsigned int, std::shared_ptr<Pager_variables>>();
    this->orders = std::unordered_map<unsigned int, std::vector<std::string>>();
    this->pendingOrders = std::vector<unsigned int>();
    this->ordersMade = std::unordered_map<unsigned int, std::vector<std::unique_ptr<Product>>>();
    this->orderCollected = std::unordered_map<unsigned int, bool>();
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
    this->all_workers_started.wait(lock, [this]() { return this->workersStarted >= this->numberOfWorkers; });
}

std::vector<WorkerReport> System::shutdown() {
    std::unique_lock<std::mutex> lock(this->mutex);

    if (debug) cout << "shutdown initiated" << endl;

    this->running = false;
    this->wait_to_restaurant.notify_all();
    this->queue_for_workers.notify_all();
    this->queue_for_reports.wait(lock , [this]() { return this->reports.size() == this->numberOfWorkers; });

    for (auto &worker: this->workers) {
        worker.join();
    }

    std::vector<WorkerReport> res = std::move(this->reports);

    for (auto &machine: this->machines) {
        machine.second->stop();
    }
    menu.clear();
    pagers.clear();
    orders.clear();
    pendingOrders.clear();
    ordersMade.clear();
    orderCollected.clear();
    abandonedOrdersId.clear();
    ordersQueue = std::queue<unsigned int>();
    nextOrderId = 0;
    CoasterPager::nextId = 0;
    occupiedWorkers = 0;
    workersStarted = 0;
    workers.clear();
    reports.clear();

    return res;
}

std::vector<std::string> System::getMenu() const {
    return menu;
}

std::vector<unsigned int> System::getPendingOrders() const {
    return pendingOrders;
}

std::unique_ptr<CoasterPager> System::order(std::vector<std::string> products) {
    std::unique_lock<std::mutex> lock(this->mutex);
    auto orderId = this->nextOrderId++;
    this->queue_for_orders.push(orderId);

    if (debug) {
        cout << "order: ";
        for (auto &product: products) {
            cout << product << " ";
        }
        cout << endl;
    }

    this->wait_to_restaurant.wait(lock, [this, orderId]() {
        return this->occupiedWorkers < this->numberOfWorkers && queue_for_orders.top() == orderId;
    });
    queue_for_orders.pop();

    if (debug) cout << "order: " << orderId << " got worker" << endl;

    if (this->running) {
        for (auto &product: products) {
            if (std::find(this->menu.begin(), this->menu.end(), product) == this->menu.end()) {
                if (debug) cout << "order: " << product << " -> bad order" << endl;
                throw BadOrderException();
            }
        }

        //auto orderId = this->nextOrderId++;
        this->orders.insert({orderId, products});
        this->pendingOrders.push_back(orderId);
        this->ordersQueue.push(orderId);
        auto pager = std::make_unique<CoasterPager>();
        //auto copy = pager.get();
        this->pagers.insert({orderId, pager->vars});
        this->queue_for_workers.notify_one();

        if (debug) {
            cout << "order: " << orderId << " ";
            for (auto &product: products) {
                cout << product << " ";
            }
            cout << "-> pager" << endl;
        }

        return pager;
    } else {
        if (debug) cout << "order: -> restaurant closed" << endl;
        throw RestaurantClosedException();
    }
}

std::vector<std::unique_ptr<Product>> System::collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {
    std::unique_lock<std::mutex> lock(this->mutex);

    auto orderId = CoasterPager->getId();
    auto it = std::find(this->abandonedOrdersId.begin(), this->abandonedOrdersId.end(), orderId);

    if (this->pagers.find(orderId) == this->pagers.end()) {
        if (debug) cout << "collectOrder: " << orderId << " -> pager: " << CoasterPager->getId() << " -> exception: BadPagerException" << endl;
        throw BadPagerException();
    } else if (!this->pagers[orderId]->ready) {
        if (debug) cout << "collectOrder: " << orderId << " -> pager: " << CoasterPager->getId() << " -> exception: OrderNotReadyException" << endl;
        throw OrderNotReadyException();
    } else if (this->pagers[orderId]->failed) {
        if (debug) cout << "collectOrder: " << orderId << " -> pager: " << CoasterPager->getId() << " -> exception: FulfillmentFailure" << endl;
        throw FulfillmentFailure();
    } else if (it != this->abandonedOrdersId.end()) {
        if (debug) cout << "collectOrder: " << orderId << " -> pager: " << CoasterPager->getId() << " -> exception: OrderExpiredException" << endl;
        throw OrderExpiredException();
    }

    this->orderCollected[orderId] = true;
    auto res = std::move(this->ordersMade[orderId]);
    if (debug) cout << "collectOrder: " << orderId << " -> pager: " << CoasterPager->getId() << " -> collecting" << endl;
    auto orderIt = std::find(this->pendingOrders.begin(), this->pendingOrders.end(), orderId);
    if (orderIt != this->pendingOrders.end()) {
        this->pendingOrders.erase(orderIt);
    }
    this->queue_for_pagers[orderId].notify_one();

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
    if (debug) cout << "worker(" << id << "/" << this->numberOfWorkers << "): started" << endl;

    if (this->workersStarted == this->numberOfWorkers) {
        this->all_workers_started.notify_one();
    }
    lock.unlock();


    while (true) {
        lock.lock();
        this->queue_for_workers.wait(lock, [this]() {
            return !this->ordersQueue.empty() || !this->running;
        });

        if (!this->running && this->ordersQueue.empty()) {
            if (debug) cout << "worker(" << id << "): creating report" << endl;
            WorkerReport report = {collectedOrders, abandonedOrders, failedOrders, failedProducts};
            this->reports.push_back(report);

            if (this->reports.size() == this->numberOfWorkers) {
                this->queue_for_reports.notify_one();
            }
            lock.unlock();
            return;
        } else {
            if (debug) cout << "worker(" << id << "): got order" << endl;
        }

        auto orderId = this->ordersQueue.front();
        auto order = this->orders[orderId];
        this->ordersQueue.pop();
        this->occupiedWorkers++;
        if (debug) cout << "worker(" << id << "): order: " << orderId << "\n";
        lock.unlock();

        std::vector<std::unique_ptr<Product>> products;
        for (auto &product: order) {
            try {
                // possibly lock for each machine here
                // worker should wait for every product at the same time
                products.push_back(this->machines[product]->getProduct());
            } catch (MachineFailure &e) {
                failedProducts.push_back(product);
                failedOrders.push_back(order);

                lock.lock();
                if (debug) cout << "worker(" << id << "): order: " << orderId << " failed for " << product << endl;
                this->pagers[orderId]->failed = true;
                // notify the pager - or maybe later?
                this->pagers[orderId]->cond.notify_one();

                this->orders.erase(orderId);
                auto orderIt = std::find(this->pendingOrders.begin(), this->pendingOrders.end(), orderId);
                if (orderIt != this->pendingOrders.end()) {
                    this->pendingOrders.erase(orderIt);
                }
                //this->pagers.erase(orderId);
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
            this->pagers[orderId]->ready = true;
            if (debug) cout << "worker(" << id << "): order: " << orderId << " ready to be collected" << endl;
            this->pagers[orderId]->cond.notify_one();

            this->queue_for_pagers[orderId].wait_for(lock, std::chrono::milliseconds(this->clientTimeout), [this, orderId]() {
                return this->orderCollected[orderId];
            });


            if (!this->orderCollected[orderId]) {
                if (debug) cout << "worker(" << id << "): order: " << orderId << " expired" << endl;
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
                if (debug) cout << "worker(" << id << "): order: " << orderId << " collected" << endl;
                collectedOrders.push_back(order);
            }

            this->orderCollected.erase(orderId);
            //this->pagers.erase(orderId);
            this->orders.erase(orderId);
        } else {
            if (debug) cout << "worker(" << id << "): order: " << orderId << " failed" << endl;
            // eventually notify the pager
        }
        this->occupiedWorkers--;
        this->wait_to_restaurant.notify_all();
        if (debug) cout << "worker(" << id << "): order: " << orderId << " finished" << endl;
        lock.unlock();
    }
}