#include "inventory_queue.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include "data_store.h"
#include "login.h"
#include "util.h"

void BranchQueue::enqueue(const Batch& b) {
    BatchNode* n = new BatchNode{b, nullptr};
    if (tail_) tail_->next = n; else head_ = n;
    tail_ = n;
    ++count_;
}
bool BranchQueue::dequeue(Batch& out) {
    if (!head_) return false;
    BatchNode* n = head_;
    out = n->data;
    head_ = n->next;
    if (!head_) tail_ = nullptr;
    delete n;
    --count_;
    return true;
}
void BranchQueue::clear() {
    while (head_) { BatchNode* n = head_; head_ = n->next; delete n; }
    tail_ = nullptr;
    count_ = 0;
}
void BranchQueue::unlink(BatchNode* prev, BatchNode* cur) {
    if (prev) prev->next = cur->next; else head_ = cur->next;
    if (cur == tail_) tail_ = prev;
    delete cur;
    --count_;
}
BatchNode* BranchQueue::detachAll() {
    BatchNode* h = head_;
    head_ = tail_ = nullptr;
    count_ = 0;
    return h;
}
void BranchQueue::attachList(BatchNode* newHead) {
    head_ = newHead; tail_ = nullptr; count_ = 0;
    for (BatchNode* n = head_; n; n = n->next) { tail_ = n; ++count_; }
}

int BranchQueue::sellableStock(const std::string& code) const {
    int total = 0;
    for (BatchNode* n = head_; n; n = n->next)
        if (n->data.code == code && daysUntil(n->data.expiry) >= 0) total += n->data.qty;
    return total;
}
int BranchQueue::totalUnits() const {
    int total = 0;
    for (BatchNode* n = head_; n; n = n->next)
        if (daysUntil(n->data.expiry) >= 0) total += n->data.qty;
    return total;
}
bool BranchQueue::findInfo(const std::string& code, Batch& out) const {
    for (BatchNode* n = head_; n; n = n->next)
        if (n->data.code == code) { out = n->data; return true; }
    return false;
}

bool BranchQueue::deductFIFO(const std::string& code, int qty, std::string& allocation) {
    if (qty <= 0 || sellableStock(code) < qty) return false;
    allocation.clear();
    int remaining = qty;
    while (remaining > 0) {
        BatchNode *best = nullptr, *bestPrev = nullptr, *prev = nullptr;
        for (BatchNode* n = head_; n; prev = n, n = n->next) {
            if (n->data.code != code || n->data.qty <= 0 || daysUntil(n->data.expiry) < 0) continue;
            if (!best || n->data.expiry < best->data.expiry) { best = n; bestPrev = prev; }
        }
        if (!best) return false;
        int take = std::min(remaining, best->data.qty);
        best->data.qty -= take;
        remaining -= take;
        if (!allocation.empty()) allocation += ";";
        allocation += best->data.expiry + ":" + std::to_string(take);
        if (best->data.qty == 0) unlink(bestPrev, best);
    }
    return true;
}
void BranchQueue::restore(const Batch& b) {
    for (BatchNode* n = head_; n; n = n->next)
        if (n->data.code == b.code && n->data.expiry == b.expiry) { n->data.qty += b.qty; return; }
    enqueue(b);
}
bool BranchQueue::updateQty(const std::string& code, const std::string& expiry, int newQty) {
    BatchNode* prev = nullptr;
    for (BatchNode* n = head_; n; prev = n, n = n->next) {
        if (n->data.code == code && n->data.expiry == expiry) {
            if (newQty <= 0) unlink(prev, n); else n->data.qty = newQty;
            return true;
        }
    }
    return false;
}
int BranchQueue::removeExpired() {
    int removed = 0;
    BatchNode *prev = nullptr, *n = head_;
    while (n) {
        BatchNode* next = n->next;
        if (daysUntil(n->data.expiry) < 0) { unlink(prev, n); ++removed; }
        else prev = n;
        n = next;
    }
    return removed;
}
std::vector<Batch> BranchQueue::toVector() const {
    std::vector<Batch> v;
    for (BatchNode* n = head_; n; n = n->next) v.push_back(n->data);
    return v;
}
void BranchQueue::display(int limit) const {
    printLine('=', 90);
    std::cout << fit("#", 5) << fit("CODE", 9) << fit("MEDICINE", 30) << fit("QTY", 7)
              << fit("PRICE", 11) << fit("EXPIRY", 12) << "STATUS\n";
    printLine('-', 90);
    int i = 0;
    for (BatchNode* n = head_; n; n = n->next) {
        if (limit >= 0 && i >= limit) { std::cout << "  ... (" << count_ - i << " more batches)\n"; break; }
        int d = daysUntil(n->data.expiry);
        std::string st = d < 0 ? "EXPIRED" : (d <= EXPIRY_WARN_DAYS ? "SOON (" + std::to_string(d) + "d)" : "");
        ++i;
        std::cout << fit(std::to_string(i) + (i == 1 ? ">" : ""), 5) << fit(n->data.code, 9)
                  << fit(n->data.name, 30) << fit(std::to_string(n->data.qty), 7)
                  << fit(money(n->data.price), 11) << fit(n->data.expiry, 12) << st << "\n";
    }
    printLine('=', 90);
    std::cout << "Batches: " << count_ << "   (> = FRONT of queue, released first)\n";
}

void printLowStock(const BranchQueue& q) {
    std::map<std::string, std::pair<std::string, int>> totals;
    for (const Batch& b : q.toVector()) {
        auto& e = totals[b.code];
        e.first = b.name;
        if (daysUntil(b.expiry) >= 0) e.second += b.qty;
    }
    int n = 0;
    std::cout << "\nLOW STOCK (below " << LOW_STOCK_THRESHOLD << " units):\n";
    for (auto& kv : totals)
        if (kv.second.second < LOW_STOCK_THRESHOLD) {
            std::cout << "  " << fit(kv.first, 9) << fit(kv.second.first, 30) << kv.second.second << " units\n";
            ++n;
        }
    if (!n) std::cout << "  None.\n";
}
void printExpiringSoon(const BranchQueue& q) {
    int n = 0;
    std::cout << "\nEXPIRING WITHIN " << EXPIRY_WARN_DAYS << " DAYS:\n";
    for (const Batch& b : q.toVector()) {
        int d = daysUntil(b.expiry);
        if (d >= 0 && d <= EXPIRY_WARN_DAYS) {
            std::cout << "  " << fit(b.code, 9) << fit(b.name, 30) << fit(std::to_string(b.qty) + " pcs", 10)
                      << b.expiry << " (" << d << " days)\n";
            ++n;
        }
    }
    if (!n) std::cout << "  None.\n";
}

bool isValidCode(const std::string& c) {
    if (c.size() != 7) return false;
    for (int i = 0; i < 3; ++i) if (!std::isalpha(static_cast<unsigned char>(c[i]))) return false;
    for (int i = 3; i < 7; ++i) if (!std::isdigit(static_cast<unsigned char>(c[i]))) return false;
    return true;
}

bool promptNewBatch(Batch& out) {
    std::string code = toUpper(trim(readLine("Product code (3 letters + 4 digits, e.g. PAR0001): ")));
    if (!isValidCode(code)) { std::cout << "Invalid code format.\n"; return false; }
    Batch info;
    bool known = false;
    {
        Transaction tx(false);
        for (const BranchQueue& q : tx.state().inv)
            if (q.findInfo(code, info)) { known = true; break; }
    }
    out = Batch();
    out.code = code;
    if (known) {
        out.name = info.name; out.price = info.price; out.category = info.category;
        std::cout << "Known medicine: " << out.name << " @ " << money(out.price) << "\n";
    } else {
        std::cout << "New medicine code.\n";
        while (true) {
            out.name = trim(readLine("Medicine name: "));
            if (!out.name.empty() && out.name.find(',') == std::string::npos) break;
            std::cout << "  Name must not be empty or contain commas.\n";
        }
        out.price = readDouble("Unit price (PHP): ", 0.01, 1000000);
        while (true) {
            out.category = trim(readLine("Category (e.g. Antibiotic, Pain & Fever; blank = General): "));
            if (out.category.empty()) out.category = "General";
            if (out.category.find(',') == std::string::npos) break;
            std::cout << "  Category must not contain commas.\n";
        }
    }
    out.qty = readInt("Quantity: ", 1, 100000);
    while (true) {
        std::string d = trim(readLine("Expiry date (YYYY-MM-DD, blank to cancel): "));
        if (d.empty()) return false;
        if (!isValidDate(d)) { std::cout << "  Invalid date.\n"; continue; }
        if (daysUntil(d) < 0) { std::cout << "  That date has already passed.\n"; continue; }
        out.expiry = d;
        break;
    }
    return true;
}

void inventoryMenu() {
    while (true) {
        std::cout << "\n=== INVENTORY MANAGEMENT (FIFO queue per branch) ===\n"
                  << " 1. View branch inventory (queue order)\n"
                  << " 2. Add batch (enqueue at rear)\n"
                  << " 3. Release front batch (dequeue)\n"
                  << " 4. Update batch quantity\n"
                  << " 5. Remove expired batches\n"
                  << " 6. Low-stock alert\n"
                  << " 7. Expiring-soon list\n"
                  << " 8. Undo last stock update (stack)\n"
                  << " 9. View stock-update history\n"
                  << " 0. Back\n";
        int c = readInt("Choice: ", 0, 9);
        if (c == 0) return;
        if (c == 8) { undoLastUpdate(); continue; }
        if (c == 9) { showUpdateHistory(); continue; }
        int b = chooseBranchFromAll("Select branch");

        if (c == 1) {
            { Transaction tx(false); std::cout << "\n" << branchName(b) << "\n"; tx.state().inv[b].display(60); }
            pauseScreen();
        } else if (c == 2) {
            Batch nb;
            if (promptNewBatch(nb)) {
                Transaction tx;
                tx.state().inv[b].enqueue(nb);
                tx.state().undo.push(makeAdded(b, {nb}));
                tx.commit();
                std::cout << "Batch enqueued at the rear of " << branchName(b)
                          << ". Run 'Restock / Sort by expiry' to keep FIFO order correct.\n";
            }
        } else if (c == 3) {
            Batch front;
            bool has = false;
            { Transaction tx(false); const Batch* f = tx.state().inv[b].peekFront(); if (f) { front = *f; has = true; } }
            if (!has) { std::cout << "Queue is empty.\n"; continue; }
            std::cout << "Front batch: " << front.code << " " << front.name << " x" << front.qty
                      << " exp " << front.expiry << "\n";
            if (confirm("Release (dequeue) this batch?")) {
                Transaction tx;
                Batch out;
                if (tx.state().inv[b].dequeue(out)) { tx.state().undo.push(makeReleased(b, out)); tx.commit(); std::cout << "Batch released.\n"; }
            }
        } else if (c == 4) {
            std::string code = toUpper(trim(readLine("Product code: ")));
            std::string exp = trim(readLine("Batch expiry date (YYYY-MM-DD): "));
            int q = readInt("New quantity (0 removes the batch): ", 0, 100000);
            Transaction tx;
            Batch before;
            bool had = false;
            for (const Batch& x : tx.state().inv[b].toVector())
                if (x.code == code && x.expiry == exp) { before = x; had = true; break; }
            if (tx.state().inv[b].updateQty(code, exp, q)) {
                if (had) tx.state().undo.push(makeUpdated(b, before, q));
                tx.commit();
                std::cout << "Updated.\n";
            }
            else std::cout << "Batch not found.\n";
        } else if (c == 5) {
            if (confirm("Remove all expired batches from " + branchName(b) + "?")) {
                Transaction tx;
                std::vector<Batch> gone;
                for (const Batch& x : tx.state().inv[b].toVector())
                    if (daysUntil(x.expiry) < 0) gone.push_back(x);
                int n = tx.state().inv[b].removeExpired();
                if (n > 0) tx.state().undo.push(makeExpiredRemoved(b, gone));
                tx.commit();
                std::cout << n << " expired batch(es) removed.\n";
            }
        } else if (c == 6) {
            { Transaction tx(false); printLowStock(tx.state().inv[b]); }
            pauseScreen();
        } else if (c == 7) {
            { Transaction tx(false); printExpiringSoon(tx.state().inv[b]); }
            pauseScreen();
        }
    }
}
