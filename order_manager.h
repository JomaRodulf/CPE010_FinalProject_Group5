#pragma once
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>
#include "data_store.h"
#include "types.h"

class OrderQueue {
public:
    bool enqueue(int orderId);
    bool dequeue(int& orderId);
    bool peek(int& orderId) const;
    bool isFull() const;
    bool isEmpty() const;
    int size() const;
    std::vector<int> snapshot() const;
    bool waitDequeue(int& orderId);
    void setPaused(bool paused);
    bool paused() const;
    void stop();

private:
    int items_[QUEUE_CAPACITY] = {};
    int head_ = 0, tail_ = 0, count_ = 0;
    bool paused_ = false, stopped_ = false;
    mutable std::mutex m_;
    std::condition_variable cv_;
};

OrderQueue& orderQueue();

struct SourcePlan {
    bool found = false;
    int branch = -1;
    int hops = 0;
};
SourcePlan planSource(const AppState& s, int preferredBranch, const std::string& code, int qty);

int placeOrder(const std::string& customer, int branchId, const std::string& code, const std::string& name,
               double unitPrice, int qty);
bool cancelOrder(int id, const std::string& customer);
bool voidServedOrder(int id);
std::vector<Order> ordersOf(const std::string& customer);
int queuePosition(int id);
int pendingCount();

std::vector<std::string> takeNotices(const std::string& customer);
int noticeCount(const std::string& customer);
struct Alert { std::string key, text; };
std::vector<Alert> currentAlerts();
int alertCount();

void requeuePendingOrders();
void startBackgroundThreads();
void stopBackgroundThreads();

void simulateRush(int customers, int ordersEach);
void orderAdminMenu();
