#include "system.hpp"

#include <utility>
#include <algorithm>
#include <iostream>

using namespace std;

const bool debug = false;

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
    return this->vars->id;
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
        this->mutex_for_machines[machine.first] = std::make_shared<std::mutex>();
        this->mutex_for_returns[machine.first] = std::make_shared<std::mutex>();
        this->queues_for_machines[machine.first] = pq<unsigned int>();
        this->machine_used[machine.first] = false;
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

    if (workers.empty()) {
        return {};
    }

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
    queues_for_machines.clear();
    nextOrderId = 0;
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

    if (this->running) {
        for (auto &product: products) {
            if (std::find(this->menu.begin(), this->menu.end(), product) == this->menu.end()) {
                if (debug) cout << "order: " << product << " -> bad order" << endl;
                throw BadOrderException();
            }
        }
        for (auto &product: products) {
            this->queues_for_machines[product].push(orderId);
        }

        this->orders.insert({orderId, products});
        this->pendingOrders.push_back(orderId);
        this->ordersQueue.push(orderId);
        auto pager = std::make_unique<CoasterPager>();
        pager->vars->id = orderId;
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

    if (!CoasterPager) throw BadPagerException();

    auto orderId = CoasterPager->getId();
    auto it = std::find(this->abandonedOrdersId.begin(), this->abandonedOrdersId.end(), orderId);

    if (this->pagers.find(orderId) == this->pagers.end() || this->orderCollected[orderId]) {
        if (debug) cout << "collectOrder: " << orderId << " -> pager: " << CoasterPager->getId() << " -> exception: BadPagerException" << endl;
        throw BadPagerException();
    } else if (this->pagers[orderId]->failed) {
        if (debug) cout << "collectOrder: " << orderId << " -> pager: " << CoasterPager->getId() << " -> exception: FulfillmentFailure" << endl;
        throw FulfillmentFailure();
    } else if (!this->pagers[orderId]->ready) {
        if (debug) cout << "collectOrder: " << orderId << " -> pager: " << CoasterPager->getId() << " -> exception: OrderNotReadyException" << endl;
        throw OrderNotReadyException();
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

void rec(std::string& p,
         unsigned int& id,
         std::map<std::string, std::shared_ptr<std::mutex>>& mutex_for_machines,
         std::unordered_map<std::string, std::shared_ptr<Machine>>& machines,
         std::map<std::string, bool>& machine_used,
         std::map<std::string, std::priority_queue<unsigned int, std::vector<unsigned int>, std::greater<>>>& queues_for_machines,
         std::map<std::string, std::condition_variable>& wait_for_machines,
         std::promise<std::unique_ptr<Product>>&& product_promise,
         std::promise<bool>&& failed_promise) {

    std::unique_lock<std::mutex> lock(*mutex_for_machines[p]);

    if (machine_used[p] || queues_for_machines[p].top() != id) {
        wait_for_machines[p].wait(lock, [&machine_used, &p, &queues_for_machines, &id]() {
            return !machine_used[p] && queues_for_machines[p].top() == id;
        });
    }
    queues_for_machines[p].pop();
    machine_used[p] = true;

    try {
        product_promise.set_value(std::move(machines[p]->getProduct()));
        failed_promise.set_value(false);
    } catch (MachineFailure &e) {
        failed_promise.set_value(true);
    }
    machine_used[p] = false;
    wait_for_machines[p].notify_all();
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
        }

        auto orderId = this->ordersQueue.front();
        auto order = this->orders[orderId];
        this->ordersQueue.pop();
        this->occupiedWorkers++;
        if (debug) cout << "worker(" << id << "): order: " << orderId << "\n";
        lock.unlock();

        std::vector<std::thread> receivers = {};
        std::vector<std::future<std::unique_ptr<Product>>> product_futures = {};
        std::vector<std::future<bool>> failed_futures = {};

        for (auto &product: order) {
            std::promise<std::unique_ptr<Product>> product_promise;
            std::promise<bool> failed_promise;
            std::future<std::unique_ptr<Product>> future = product_promise.get_future();
            std::future<bool> failed_future = failed_promise.get_future();
            std::thread t(rec, std::ref(product), std::ref(orderId), std::ref(this->mutex_for_machines),
                          std::ref(this->machines), std::ref(this->machine_used),
                          std::ref(this->queues_for_machines), std::ref(this->wait_for_machines),
                          std::move(product_promise), std::move(failed_promise));

            receivers.push_back(std::move(t));
            product_futures.push_back(std::move(future));
            failed_futures.push_back(std::move(failed_future));
        }

        std::vector<std::pair<std::string, std::unique_ptr<Product>>> products = {};
        unsigned int it = 0;
        bool nothing_failed = true;

        for (auto &future : failed_futures) {
            auto failed = future.get();
            if (failed) {
                nothing_failed = false;
                failedProducts.emplace_back(order[it]);
                auto prodIt = std::find(this->menu.begin(), this->menu.end(), order[it]);
                if (prodIt != this->menu.end()) this->menu.erase(prodIt);
            } else {
                products.emplace_back(order[it], std::move(product_futures[it].get()));
            }
            receivers[it].join();
            it++;
        }

        lock.lock();
        if (nothing_failed) {
            this->orderCollected.insert({orderId, false});
//            auto res = std::vector<std::unique_ptr<Product>>();
//            for (auto &product : products) res.push_back(std::move(product.second));
//            this->ordersMade.insert({orderId, std::move(res)});
            this->ordersMade.insert({orderId, std::vector<std::unique_ptr<Product>>()});
            for (auto &product : products) this->ordersMade[orderId].push_back(std::move(product.second));

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
                unsigned int it_p = 0;
                for (auto &p: this->ordersMade[orderId]) {
                    products[it_p].second = std::move(p);
                    it_p++;
                }
                this->ordersMade.erase(orderId);
                lock.unlock();
//                for (auto &machine: this->machines) {
//                    for (auto &p: products) {
//                        std::unique_lock<std::mutex> lock_return(*this->mutex_for_returns[machine.first]);
//                        try {
//                            machine.second->returnProduct(std::move(p.second));
//                            break;
//                        } catch (BadProductException &e) {
//                            continue;
//                        }
//                    }
//                }
                for (auto &p: products) {
                    std::unique_lock<std::mutex> lock_return(*this->mutex_for_returns[p.first]);
                    if (debug) cout << "worker(" << id << "): order: " << orderId << " returning product: " << p.first << endl;
                    this->machines[p.first]->returnProduct(std::move(p.second));
                    lock_return.unlock();
                }
                lock.lock();
            } else {
                if (debug) cout << "worker(" << id << "): order: " << orderId << " collected" << endl;
                collectedOrders.push_back(order);
            }

            this->orders.erase(orderId);
            lock.unlock();
        } else {
            failedOrders.push_back(order);

            this->pagers[orderId]->failed = true;

            this->orders.erase(orderId);
            auto orderIt = std::find(this->pendingOrders.begin(), this->pendingOrders.end(), orderId);
            if (orderIt != this->pendingOrders.end()) this->pendingOrders.erase(orderIt);

            this->pagers[orderId]->cond.notify_one();
            lock.unlock();

//            for (auto &machine: this->machines) {
//                for (auto &p: products) {
//                    std::unique_lock<std::mutex> lock_return(*this->mutex_for_returns[machine.first]);
//                    try {
//                        machine.second->returnProduct(std::move(p.second));
//                        break;
//                    } catch (BadProductException &e) {
//                        continue;
//                    }
//                }
//            }
            for (auto &p: products) {
                std::unique_lock<std::mutex> lock_return(*this->mutex_for_returns[p.first]);
                if (debug) cout << "worker(" << id << "): order: " << orderId << " returning product: " << p.first << endl;
                this->machines[p.first]->returnProduct(std::move(p.second));
                lock_return.unlock();
            }
        }

        lock.lock();
        this->occupiedWorkers--;
        this->wait_to_restaurant.notify_all();
        if (debug) cout << "worker(" << id << "): order: " << orderId << " finished" << endl;
        lock.unlock();
    }
}