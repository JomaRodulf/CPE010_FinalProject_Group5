#pragma once
#include <vector>
#include "inventory_queue.h"

void insertionSortByExpiry(BranchQueue& q);
void restockBranch(BranchQueue& q, const std::vector<Batch>& incoming);
void restockMenu();
