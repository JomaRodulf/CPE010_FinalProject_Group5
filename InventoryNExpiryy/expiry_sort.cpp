#include "expiry_sort.h"
#include <iostream>
#include "data_store.h"
#include "login.h"
#include "util.h"

void insertionSortByExpiry(BranchQueue& q) {
    BatchNode* cur = q.detachAll();
    BatchNode* sorted = nullptr;
    while (cur) {
        BatchNode* nxt = cur->next;
        if (!sorted || cur->data.expiry < sorted->data.expiry) {
            cur->next = sorted;
            sorted = cur;
        } else {
            BatchNode* p = sorted;
            while (p->next && p->next->data.expiry <= cur->data.expiry) p = p->next;
            cur->next = p->next;
            p->next = cur;
        }
        cur = nxt;
    }
    q.attachList(sorted);
}

void restockBranch(BranchQueue& q, const std::vector<Batch>& incoming) {
    std::cout << "Leftover batches: " << q.size() << " | New batches: " << incoming.size() << "\n";
    for (const Batch& b : incoming) q.enqueue(b);
    insertionSortByExpiry(q);
    std::cout << "Merged and sorted by expiry date (soonest first). First batches in queue:\n";
    q.display(8);
}

void restockMenu() {
    while (true) {
        std::cout << "\n=== RESTOCK / SORT BY EXPIRY (Insertion Sort) ===\n"
                  << " 1. Restock a branch (new batches + leftovers, then sort)\n"
                  << " 2. Re-sort one branch\n"
                  << " 3. Re-sort all branches\n"
                  << " 0. Back\n";
        int c = readInt("Choice: ", 0, 3);
        if (c == 0) return;
        if (c == 1) {
            int b = chooseBranchFromAll("Restock which branch");
            std::vector<Batch> incoming;
            do {
                Batch nb;
                if (promptNewBatch(nb)) incoming.push_back(nb);
            } while (confirm("Add another new batch?"));
            if (incoming.empty()) { std::cout << "Nothing to restock.\n"; continue; }
            Transaction tx;
            restockBranch(tx.state().inv[b], incoming);
            tx.state().undo.push(makeAdded(b, incoming));
            tx.commit();
        } else if (c == 2) {
            int b = chooseBranchFromAll("Sort which branch");
            Transaction tx;
            insertionSortByExpiry(tx.state().inv[b]);
            tx.commit();
            tx.state().inv[b].display(8);
        } else {
            Transaction tx;
            for (auto& q : tx.state().inv) insertionSortByExpiry(q);
            tx.commit();
            std::cout << "All " << NUM_BRANCHES << " branches sorted by expiry date.\n";
        }
    }
}
