#include "order_manager.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <random>
#include <thread>
#include "catalog.h"
#include "expiry_sort.h"
#include "login.h"
#include "ui.h"
#include "util.h"

bool OrderQueue::enqueue(int id) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (count_ == QUEUE_CAPACITY) return false;
        items_[tail_] = id;
        tail_ = (tail_ + 1) % QUEUE_CAPACITY;
        ++count_;
    }
    cv_.notify_one();
    return true;
}

bool OrderQueue::dequeue(int& id) {
    std::lock_guard<std::mutex> lk(m_);
    if (count_ == 0) return false;
    id = items_[head_];
    head_ = (head_ + 1) % QUEUE_CAPACITY;
    --count_;
    return true;
}

bool OrderQueue::peek(int& id) const {
    std::lock_guard<std::mutex> lk(m_);
    if (count_ == 0) return false;
    id = items_[head_];
    return true;
}

bool OrderQueue::isFull() const { std::lock_guard<std::mutex> lk(m_); return count_ == QUEUE_CAPACITY; }
bool OrderQueue::isEmpty() const { std::lock_guard<std::mutex> lk(m_); return count_ == 0; }
int OrderQueue::size() const { std::lock_guard<std::mutex> lk(m_); return count_; }

std::vector<int> OrderQueue::snapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<int> v;
    for (int i = 0; i < count_; ++i) v.push_back(items_[(head_ + i) % QUEUE_CAPACITY]);
    return v;
}

bool OrderQueue::waitDequeue(int& id) {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [this] { return stopped_ || (!paused_ && count_ > 0); });
    if (stopped_) return false;
    id = items_[head_];
    head_ = (head_ + 1) % QUEUE_CAPACITY;
    --count_;
    return true;
}

void OrderQueue::setPaused(bool p) {
    { std::lock_guard<std::mutex> lk(m_); paused_ = p; }
    cv_.notify_all();
}
bool OrderQueue::paused() const { std::lock_guard<std::mutex> lk(m_); return paused_; }
void OrderQueue::stop() {
    { std::lock_guard<std::mutex> lk(m_); stopped_ = true; }
    cv_.notify_all();
}

static OrderQueue g_queue;
OrderQueue& orderQueue() { return g_queue; }

namespace {
std::mutex g_noteMutex;
std::vector<std::pair<std::string, std::string>> g_notices;
std::map<std::string, Alert> g_alerts;

void addNotice(const std::string& customer, const std::string& text) {
    std::lock_guard<std::mutex> lk(g_noteMutex);
    g_notices.push_back({toLower(customer), text});
    if (g_notices.size() > 500) g_notices.erase(g_notices.begin());
}
void raiseAlert(const std::string& key, const std::string& text) {
    std::lock_guard<std::mutex> lk(g_noteMutex);
    g_alerts[key] = Alert{key, text};
}
}

std::vector<std::string> takeNotices(const std::string& customer) {
    std::lock_guard<std::mutex> lk(g_noteMutex);
    std::vector<std::string> out;
    std::string key = toLower(customer);
    for (auto it = g_notices.begin(); it != g_notices.end();) {
        if (it->first == key) { out.push_back(it->second); it = g_notices.erase(it); }
        else ++it;
    }
    return out;
}
int noticeCount(const std::string& customer) {
    std::lock_guard<std::mutex> lk(g_noteMutex);
    std::string key = toLower(customer);
    return static_cast<int>(std::count_if(g_notices.begin(), g_notices.end(),
                                          [&](const auto& n) { return n.first == key; }));
}
std::vector<Alert> currentAlerts() {
    std::lock_guard<std::mutex> lk(g_noteMutex);
    std::vector<Alert> v;
    for (auto& kv : g_alerts) v.push_back(kv.second);
    return v;
}
int alertCount() {
    std::lock_guard<std::mutex> lk(g_noteMutex);
    return static_cast<int>(g_alerts.size());
}

SourcePlan planSource(const AppState& s, int preferred, const std::string& code, int qty) {
    SourcePlan plan;
    auto canFill = [&](int b) { return s.inv[b].sellableStock(code) >= qty; };
    if (canFill(preferred)) {
        plan.found = true;
        plan.branch = preferred;
        return plan;
    }
    BranchGraph::Found f = s.graph.nearest(preferred, canFill);
    if (f.found) { plan.found = true; plan.branch = f.branch; plan.hops = f.hops; }
    return plan;
}

int placeOrder(const std::string& customer, int branchId, const std::string& code, const std::string& name,
               double unitPrice, int qty) {
    Transaction tx;
    AppState& s = tx.state();
    if (g_queue.isFull()) return -1;
    Order o;
    o.id = static_cast<int>(s.orders.size()) + 1;
    o.customer = customer;
    o.branchId = branchId;
    o.code = code;
    o.name = name;
    o.qty = qty;
    o.unitPrice = unitPrice;
    o.total = unitPrice * qty;
    o.placedAt = nowStr();
    s.orders.push_back(o);
    g_queue.enqueue(o.id);
    tx.commit();
    return o.id;
}

bool cancelOrder(int id, const std::string& customer) {
    Transaction tx;
    Order* o = findOrder(tx.state(), id);
    if (!o || o->status != OrderStatus::PENDING) return false;
    if (!customer.empty() && toLower(o->customer) != toLower(customer)) return false;
    o->status = OrderStatus::CANCELLED;
    o->servedAt = nowStr();
    tx.commit();
    return true;
}

bool voidServedOrder(int id) {
    Transaction tx;
    AppState& s = tx.state();
    Order* o = findOrder(s, id);
    if (!o || o->status != OrderStatus::SERVED) return false;
    int b = o->fulfilledBranch >= 0 ? o->fulfilledBranch : o->branchId;
    std::string category = "General";
    for (const BranchQueue& q : s.inv) {
        Batch info;
        if (q.findInfo(o->code, info)) { category = info.category; break; }
    }
    for (const auto& part : parseAllocation(o->allocation)) {
        Batch back;
        back.code = o->code; back.name = o->name; back.price = o->unitPrice; back.category = category;
        back.expiry = part.first; back.qty = part.second;
        s.inv[b].restore(back);
    }
    insertionSortByExpiry(s.inv[b]);
    o->status = OrderStatus::CANCELLED;
    o->servedAt = nowStr();
    tx.commit();
    return true;
}

std::vector<Order> ordersOf(const std::string& customer) {
    Transaction tx(false);
    std::vector<Order> out;
    for (const Order& o : tx.state().orders)
        if (toLower(o.customer) == toLower(customer)) out.push_back(o);
    return out;
}

int queuePosition(int id) {
    std::vector<int> ids = g_queue.snapshot();
    Transaction tx(false);
    int pos = 0;
    for (int qid : ids) {
        Order* o = findOrder(tx.state(), qid);
        if (o && o->status == OrderStatus::PENDING) {
            ++pos;
            if (qid == id) return pos;
        }
    }
    return 0;
}

int pendingCount() {
    Transaction tx(false);
    int n = 0;
    for (const Order& o : tx.state().orders)
        if (o.status == OrderStatus::PENDING) ++n;
    return n;
}

namespace {
std::mutex g_stopMutex;
std::condition_variable g_stopCv;
bool g_stopping = false;
std::atomic<int> g_delayMs{1500};
std::thread g_worker, g_monitor;

bool sleepOrStop(int ms) {
    std::unique_lock<std::mutex> lk(g_stopMutex);
    return g_stopCv.wait_for(lk, std::chrono::milliseconds(ms), [] { return g_stopping; });
}

void processOrder(int id) {
    std::string customerNote;
    std::string alertKey, alertText;
    std::string customer;
    {
        Transaction tx;
        AppState& s = tx.state();
        Order* o = findOrder(s, id);
        if (!o || o->status != OrderStatus::PENDING) return;
        customer = o->customer;

        SourcePlan plan = planSource(s, o->branchId, o->code, o->qty);
        std::string alloc;
        if (!plan.found || !s.inv[plan.branch].deductFIFO(o->code, o->qty, alloc)) {
            o->status = OrderStatus::CANCELLED;
            o->servedAt = nowStr();
            customerNote = "Order #" + std::to_string(id) + " REJECTED: not enough stock of " + o->name +
                           " (x" + std::to_string(o->qty) + ") in any connected branch.";
        } else {
            o->status = OrderStatus::SERVED;
            o->servedAt = nowStr();
            o->allocation = alloc;
            o->fulfilledBranch = plan.branch;
            customerNote = "Order #" + std::to_string(id) + " CONFIRMED: " + std::to_string(o->qty) + " x " + o->name +
                           " (" + money(o->total) + "). Pick up at " + branchName(plan.branch) + ".";
            if (plan.branch != o->branchId)
                customerNote += " Your branch was short, so it was sourced " + std::to_string(plan.hops) +
                                " hop(s) away.";
            int left = s.inv[plan.branch].sellableStock(o->code);
            if (left < LOW_STOCK_THRESHOLD) {
                alertKey = "L|" + std::to_string(plan.branch) + "|" + o->code;
                alertText = "[" + branchName(plan.branch) + "] " + o->name + ": " + std::to_string(left) +
                            " units left (minimum " + std::to_string(LOW_STOCK_THRESHOLD) + ")";
            }
        }
        tx.commit();
    }
    addNotice(customer, customerNote);
    if (!alertKey.empty()) raiseAlert(alertKey, alertText);
}

void workerLoop() {
    int id;
    while (g_queue.waitDequeue(id)) {
        if (sleepOrStop(g_delayMs)) break;
        processOrder(id);
    }
}

void scanStock() {
    std::map<std::string, Alert> found;
    {
        Transaction tx(false);
        const Inventory& inv = tx.state().inv;
        for (int b = 0; b < NUM_BRANCHES; ++b) {
            std::map<std::string, std::pair<std::string, int>> totals;
            int expired = 0;
            for (const Batch& x : inv[b].toVector()) {
                auto& e = totals[x.code];
                e.first = x.name;
                if (daysUntil(x.expiry) >= 0) e.second += x.qty;
                else ++expired;
            }
            for (auto& kv : totals) {
                if (kv.second.second >= LOW_STOCK_THRESHOLD) continue;
                std::string key = "L|" + std::to_string(b) + "|" + kv.first;
                found[key] = Alert{key, "[" + branchName(b) + "] " + kv.second.first + ": " +
                                            std::to_string(kv.second.second) + " units left (minimum " +
                                            std::to_string(LOW_STOCK_THRESHOLD) + ")"};
            }
            if (expired > 0) {
                std::string key = "E|" + std::to_string(b);
                found[key] = Alert{key, "[" + branchName(b) + "] " + std::to_string(expired) +
                                            " expired batch(es) still in the queue - remove them"};
            }
        }
    }
    std::lock_guard<std::mutex> lk(g_noteMutex);
    g_alerts = found;
}

void monitorLoop() {
    do { scanStock(); } while (!sleepOrStop(3000));
}
}

void requeuePendingOrders() {
    Transaction tx(false);
    for (const Order& o : tx.state().orders)
        if (o.status == OrderStatus::PENDING) g_queue.enqueue(o.id);
}

void startBackgroundThreads() {
    { std::lock_guard<std::mutex> lk(g_stopMutex); g_stopping = false; }
    g_worker = std::thread(workerLoop);
    g_monitor = std::thread(monitorLoop);
}

void stopBackgroundThreads() {
    { std::lock_guard<std::mutex> lk(g_stopMutex); g_stopping = true; }
    g_stopCv.notify_all();
    g_queue.stop();
    if (g_worker.joinable()) g_worker.join();
    if (g_monitor.joinable()) g_monitor.join();
}

void simulateRush(int customers, int ordersEach) {
    Catalog cat = buildCatalog();
    if (cat.empty()) { ui::warn("Catalog is empty."); return; }
    std::atomic<int> placed{0}, rejected{0};
    std::random_device rd;
    unsigned baseSeed = rd();
    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < customers; ++i) {
        threads.emplace_back([&, i] {
            std::mt19937 rng(baseSeed + static_cast<unsigned>(i));
            std::string who = "Walkin " + std::string(1, static_cast<char>('A' + i % 26));
            for (int k = 0; k < ordersEach; ++k) {
                const Product& p = cat.products()[rng() % cat.products().size()];
                int branch = static_cast<int>(rng() % NUM_BRANCHES);
                int qty = 1 + static_cast<int>(rng() % 5);
                if (placeOrder(who, branch, p.code, p.name, p.price, qty) > 0) ++placed;
                else ++rejected;
            }
        });
    }
    for (auto& t : threads) t.join();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ui::ok(std::to_string(customers) + " customer threads placed " + std::to_string(placed.load()) + " order(s) in " +
           std::to_string(static_cast<int>(ms)) + " ms" +
           (rejected ? " (" + std::to_string(rejected.load()) + " rejected: queue full)" : "") + ".");
    std::cout << "The single worker thread now serves them first-come, first-served.\n";
}

static std::string statusColor(OrderStatus s) {
    std::string t = fit(statusToString(s), 11);
    if (s == OrderStatus::SERVED) return ui::green(t);
    if (s == OrderStatus::CANCELLED) return ui::red(t);
    return ui::yellow(t);
}

static void printOrderRows(const std::vector<Order>& rows) {
    printLine('-', 90);
    std::cout << fit("ORDER", 7) << fit("CUSTOMER", 16) << fit("MEDICINE", 24) << fit("QTY", 5) << fit("TOTAL", 13)
              << fit("STATUS", 11) << "BRANCH\n";
    printLine('-', 90);
    for (const Order& o : rows) {
        int b = o.fulfilledBranch >= 0 ? o.fulfilledBranch : o.branchId;
        std::cout << fit("#" + std::to_string(o.id), 7) << fit(o.customer, 16) << fit(o.name, 24)
                  << fit(std::to_string(o.qty), 5) << fit(money(o.total), 13) << statusColor(o.status)
                  << branchName(b) << "\n";
    }
    printLine('-', 90);
}

void orderAdminMenu() {
    while (true) {
        ui::header("ORDER QUEUE & PROCESSOR (circular array queue + worker thread)");
        ui::statusBar("Worker: " + std::string(g_queue.paused() ? "PAUSED" : "RUNNING") + "  |  Processing time: " +
                      std::to_string(g_delayMs.load()) + " ms/order  |  In queue: " + std::to_string(g_queue.size()) +
                      "/" + std::to_string(QUEUE_CAPACITY) + "  |  Pending orders: " + std::to_string(pendingCount()));
        std::cout << " 1. View waiting orders (front -> rear)\n"
                  << " 2. View recent orders (last 15)\n"
                  << " 3. Pause / resume the order processor\n"
                  << " 4. Set processing time per order\n"
                  << " 5. Cancel a pending order\n"
                  << " 6. Void a served order (return stock to the branch)\n"
                  << " 7. Simulate rush hour (many customers order at once)\n"
                  << " 0. Back\n";
        int c = readInt("Choice: ", 0, 7);
        if (c == 0) return;

        if (c == 1) {
            std::vector<int> ids = g_queue.snapshot();
            std::vector<Order> rows;
            {
                Transaction tx(false);
                for (int id : ids) {
                    Order* o = findOrder(tx.state(), id);
                    if (o && o->status == OrderStatus::PENDING) rows.push_back(*o);
                }
            }
            if (rows.empty()) std::cout << "\nThe queue is empty.\n";
            else { std::cout << "\nFRONT of queue is first:\n"; printOrderRows(rows); }
            pauseScreen();
        } else if (c == 2) {
            std::vector<Order> rows;
            {
                Transaction tx(false);
                const auto& all = tx.state().orders;
                size_t from = all.size() > 15 ? all.size() - 15 : 0;
                rows.assign(all.begin() + from, all.end());
            }
            if (rows.empty()) std::cout << "\nNo orders yet.\n";
            else { std::cout << "\n"; printOrderRows(rows); }
            pauseScreen();
        } else if (c == 3) {
            g_queue.setPaused(!g_queue.paused());
            ui::ok(g_queue.paused() ? "Order processor paused. New orders will wait in the queue."
                                    : "Order processor resumed.");
            pauseScreen();
        } else if (c == 4) {
            g_delayMs = readInt("Milliseconds per order (0-10000): ", 0, 10000);
            ui::ok("Processing time updated.");
            pauseScreen();
        } else if (c == 5) {
            int id = readInt("Order number to cancel (0 = back): ", 0, 100000000);
            if (id == 0) continue;
            if (cancelOrder(id, "")) ui::ok("Order #" + std::to_string(id) + " cancelled.");
            else ui::warn("That order does not exist or is no longer pending.");
            pauseScreen();
        } else if (c == 6) {
            int id = readInt("Served order number to void (0 = back): ", 0, 100000000);
            if (id == 0) continue;
            if (voidServedOrder(id)) ui::ok("Order #" + std::to_string(id) + " voided and its stock returned.");
            else ui::warn("That order does not exist or was not served.");
            pauseScreen();
        } else {
            std::cout << "\nThis places REAL orders and will deduct stock from the branches.\n";
            if (!confirm("Continue?")) continue;
            int n = readInt("Number of customer threads (1-20): ", 1, 20);
            int k = readInt("Orders per customer (1-5): ", 1, 5);
            simulateRush(n, k);
            pauseScreen();
        }
    }
}
