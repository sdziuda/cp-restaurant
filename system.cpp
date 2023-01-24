#include "system.hpp"

#include <utility>

CoasterPager::CoasterPager(unsigned int orderId) {
    this->orderId = orderId;
    this->ready = false;
}

void CoasterPager::wait() const {
    while (!this->ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void CoasterPager::wait(unsigned int timeout) const {
    unsigned int time = 0;
    while (!this->ready && time < timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        time++;
    }
}

unsigned int CoasterPager::getId() const {
    return this->orderId;
}

bool CoasterPager::isReady() const {
    return this->ready;
}

System::System(System::machines_t machines, unsigned int numberOfWorkers,
               unsigned int clientTimeout) {
    this->machines = std::move(machines);
    this->numberOfWorkers = numberOfWorkers;
    this->clientTimeout = clientTimeout;
    for (auto &machine: this->machines) {
        this->menu.push_back(machine.first);
    }
    this->pendingOrders = std::vector<unsigned int>();
    this->nextOrderId = 0;
    this->workingWorkers = 0;
    this->running = true;

    for (auto &machine: this->machines) {
        machine.second->start();
    }
    for (unsigned int i = 0; i < numberOfWorkers; i++) {
        this->workers.emplace_back(std::thread(&System::worker, this));
    }
}

std::vector<WorkerReport> System::shutdown() {
    std::unique_lock<std::mutex> lock(this->mutex);

    this->running = false;
    this->queue_to_restaurant.notify_all();

    for (auto &worker: this->workers) {
        worker.join();
    }

    std::vector<WorkerReport> reports;
    for (auto &machine: this->machines) {
        machine.second->stop();
    }

    return reports;
}

std::vector<std::string> System::getMenu() const {
    return menu;
}

std::vector<unsigned int> System::getPendingOrders() const {
    return pendingOrders;
}

std::unique_ptr<CoasterPager> System::order(std::vector<std::string> products) {
    std::unique_lock<std::mutex> lock(this->mutex);

    if (!this->running) {
        throw RestaurantClosedException();
    }

    for (auto &product: products) {
        if (this->machines.find(product) == this->machines.end()) {
            throw BadOrderException();
        }
    }

    pendingOrders.push_back(nextOrderId++);

    return std::make_unique<CoasterPager>(nextOrderId - 1);
}

std::vector<std::unique_ptr<Product>> System::collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {
    std::unique_lock<std::mutex> lock(this->mutex);

    if (!CoasterPager->isReady()) {
        throw OrderNotReadyException();
    }

    auto orderId = CoasterPager->getId();
    auto it = std::find(this->pendingOrders.begin(), this->pendingOrders.end(), orderId);

    if (it == this->pendingOrders.end()) {
        throw BadPagerException();
    }

    std::vector<std::unique_ptr<Product>> products;

    return products;
}

unsigned int System::getClientTimeout() const {
    return clientTimeout;
}

void System::worker() {
//    this->workingWorkers++;
//    while (true) {
//        std::unique_lock<std::mutex> lock(this->mutex);
//        this->cond.wait(lock, [this]() {
//            return !this->queue.empty() || !this->running;
//        });
//        if (!this->running) {
//            break;
//        }
//        auto order = std::move(this->queue.front());
//        this->queue.pop_front();
//        lock.unlock();
//        std::vector<std::unique_ptr<Product>> products;
//        for (auto &product: order) {
//            products.push_back(this->machines[product]->make());
//        }
//        lock.lock();
//        this->collectedOrders.push_back(std::move(products));
//        lock.unlock();
//    }
//    this->workingWorkers--;
}