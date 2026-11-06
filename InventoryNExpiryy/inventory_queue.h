#pragma once
#include <array>
#include <string>
#include <vector>
#include "types.h"

struct BatchNode {
    Batch data;
    BatchNode* next;
};

class BranchQueue {
public:
    BranchQueue() = default;
    ~BranchQueue() { clear(); }
    BranchQueue(const BranchQueue&) = delete;
    BranchQueue& operator=(const BranchQueue&) = delete;

    void enqueue(const Batch& b);
    bool dequeue(Batch& out);
    const Batch* peekFront() const { return head_ ? &head_->data : nullptr; }
    int size() const { return count_; }
    bool empty() const { return count_ == 0; }
    void clear();

    int sellableStock(const std::string& code) const;
    int totalUnits() const;
    bool findInfo(const std::string& code, Batch& out) const;
    bool deductFIFO(const std::string& code, int qty, std::string& allocation);
    void restore(const Batch& b);
    bool updateQty(const std::string& code, const std::string& expiry, int newQty);
    int removeExpired();
    std::vector<Batch> toVector() const;
    void display(int limit = -1) const;

    BatchNode* detachAll();
    void attachList(BatchNode* newHead);

private:
    BatchNode* head_ = nullptr;
    BatchNode* tail_ = nullptr;
    int count_ = 0;
    void unlink(BatchNode* prev, BatchNode* cur);
};

using Inventory = std::array<BranchQueue, NUM_BRANCHES>;

bool isValidCode(const std::string& code);
bool promptNewBatch(Batch& out);
void printLowStock(const BranchQueue& q);
void printExpiringSoon(const BranchQueue& q);
void inventoryMenu();
