/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/memory/Memory.h"
#include "velox/common/memory/MmapAllocator.h"
#include "velox/exec/HashBitRange.h"

using namespace ::testing;

constexpr int64_t KB = 1024L;
constexpr int64_t MB = 1024L * KB;
constexpr int64_t GB = 1024L * MB;

namespace facebook {
namespace velox {
namespace memory {

class MemoryPoolTest : public testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    useMmap_ = GetParam();
    // For duration of the test, make a local MmapAllocator that will not be
    // seen by any other test.
    if (useMmap_) {
      MmapAllocatorOptions opts{8UL << 30};
      mmapAllocator_ = std::make_unique<MmapAllocator>(opts);
      MappedMemory::setDefaultInstance(mmapAllocator_.get());
    } else {
      MappedMemory::setDefaultInstance(nullptr);
    }
  }

  void TearDown() override {
    MmapAllocator::setDefaultInstance(nullptr);
  }

  std::shared_ptr<IMemoryManager> getMemoryManager(int64_t quota) {
    if (useMmap_) {
      return std::make_shared<MemoryManager<MmapMemoryAllocator>>(quota);
    }
    return std::make_shared<MemoryManager<MemoryAllocator>>(quota);
  }

  bool useMmap_;
  std::unique_ptr<MmapAllocator> mmapAllocator_;
};

TEST(MemoryPoolTest, Ctor) {
  MemoryManager<MemoryAllocator, 64> manager{8 * GB};
  // While not recomended, the root allocator should be valid.
  auto& root =
      dynamic_cast<MemoryPoolImpl<MemoryAllocator, 64>&>(manager.getRoot());

  ASSERT_EQ(8 * GB, root.cap_);
  ASSERT_EQ(0, root.getCurrentBytes());
  ASSERT_EQ(root.parent(), nullptr);

  {
    auto fakeRoot = std::make_shared<MemoryPoolImpl<MemoryAllocator, 64>>(
        manager, "fake_root", nullptr, 4 * GB);
    ASSERT_EQ("fake_root", fakeRoot->name());
    ASSERT_EQ(4 * GB, fakeRoot->cap_);
    ASSERT_EQ(&root.allocator_, &fakeRoot->allocator_);
    ASSERT_EQ(0, fakeRoot->getCurrentBytes());
    ASSERT_EQ(fakeRoot->parent(), nullptr);
  }
  {
    auto child = root.addChild("favorite_child");
    ASSERT_EQ(child->parent(), &root);
    auto& favoriteChild =
        dynamic_cast<MemoryPoolImpl<MemoryAllocator, 64>&>(*child);
    ASSERT_EQ("favorite_child", favoriteChild.name());
    ASSERT_EQ(std::numeric_limits<int64_t>::max(), favoriteChild.cap_);
    ASSERT_EQ(&root.allocator_, &favoriteChild.allocator_);
    ASSERT_EQ(0, favoriteChild.getCurrentBytes());
  }
  {
    auto child = root.addChild("naughty_child", 3 * GB);
    ASSERT_EQ(child->parent(), &root);
    auto& naughtyChild =
        dynamic_cast<MemoryPoolImpl<MemoryAllocator, 64>&>(*child);
    ASSERT_EQ("naughty_child", naughtyChild.name());
    ASSERT_EQ(3 * GB, naughtyChild.cap_);
    ASSERT_EQ(&root.allocator_, &naughtyChild.allocator_);
    ASSERT_EQ(0, naughtyChild.getCurrentBytes());
  }
}

TEST(MemoryPoolTest, AddChild) {
  MemoryManager<MemoryAllocator> manager{};
  auto& root = manager.getRoot();

  ASSERT_EQ(0, root.getChildCount());
  auto childOne = root.addChild("child_one");
  auto childTwo = root.addChild("child_two", 4L * 1024L * 1024L);

  std::vector<MemoryPool*> nodes{};
  ASSERT_EQ(2, root.getChildCount());
  root.visitChildren(
      [&nodes](MemoryPool* child) { nodes.emplace_back(child); });
  EXPECT_THAT(
      nodes, UnorderedElementsAreArray({childOne.get(), childTwo.get()}));

  // We no longer care about name uniqueness.
  auto childTree = root.addChild("child_one");
  EXPECT_EQ(3, root.getChildCount());

  // Adding child while capped.
  root.capMemoryAllocation();
  auto childFour = root.addChild("child_four");
  EXPECT_TRUE(childFour->isMemoryCapped());
}

TEST_P(MemoryPoolTest, dropChild) {
  MemoryManager<MemoryAllocator> manager{};
  auto& root = manager.getRoot();
  ASSERT_EQ(root.parent(), nullptr);

  ASSERT_EQ(0, root.getChildCount());
  auto childOne = root.addChild("child_one");
  ASSERT_EQ(childOne->parent(), &root);
  auto childTwo = root.addChild("child_two", 4L * 1024L * 1024L);
  ASSERT_EQ(childTwo->parent(), &root);
  ASSERT_EQ(2, root.getChildCount());

  childOne.reset();
  ASSERT_EQ(1, root.getChildCount());

  // Remove invalid address.
  childTwo.reset();
  ASSERT_EQ(0, root.getChildCount());

  // Check parent pool is alive until all the children has been destroyed.
  auto child = root.addChild("child");
  ASSERT_EQ(child->parent(), &root);
  auto* rawChild = child.get();
  auto grandChild1 = child->addChild("grandChild1");
  ASSERT_EQ(grandChild1->parent(), child.get());
  auto grandChild2 = child->addChild("grandChild1");
  ASSERT_EQ(grandChild2->parent(), child.get());
  ASSERT_EQ(1, root.getChildCount());
  ASSERT_EQ(2, child->getChildCount());
  ASSERT_EQ(0, grandChild1->getChildCount());
  ASSERT_EQ(0, grandChild2->getChildCount());
  child.reset();
  ASSERT_EQ(1, root.getChildCount());
  ASSERT_EQ(2, rawChild->getChildCount());
  grandChild1.reset();
  ASSERT_EQ(1, root.getChildCount());
  ASSERT_EQ(1, rawChild->getChildCount());
  grandChild2.reset();
  ASSERT_EQ(0, root.getChildCount());
}

TEST(MemoryPoolTest, CapSubtree) {
  MemoryManager<MemoryAllocator> manager{};
  auto& root = manager.getRoot();

  // left subtree.
  auto node_a = root.addChild("node_a");
  auto node_aa = node_a->addChild("node_aa");
  auto node_ab = node_a->addChild("node_ab");
  auto node_aba = node_ab->addChild("node_aba");

  // right subtree
  auto node_b = root.addChild("node_b");
  auto node_ba = node_b->addChild("node_ba");
  auto node_bb = node_b->addChild("node_bb");
  auto node_bc = node_b->addChild("node_bc");

  // Cap left subtree and check that right subtree is not impacted.
  node_a->capMemoryAllocation();
  ASSERT_TRUE(node_a->isMemoryCapped());
  ASSERT_TRUE(node_aa->isMemoryCapped());
  ASSERT_TRUE(node_ab->isMemoryCapped());
  ASSERT_TRUE(node_aba->isMemoryCapped());

  ASSERT_FALSE(root.isMemoryCapped());
  ASSERT_FALSE(node_b->isMemoryCapped());
  ASSERT_FALSE(node_ba->isMemoryCapped());
  ASSERT_FALSE(node_bb->isMemoryCapped());
  ASSERT_FALSE(node_bc->isMemoryCapped());

  // Cap the entire tree.
  root.capMemoryAllocation();
  ASSERT_TRUE(root.isMemoryCapped());
  ASSERT_TRUE(node_a->isMemoryCapped());
  ASSERT_TRUE(node_aa->isMemoryCapped());
  ASSERT_TRUE(node_ab->isMemoryCapped());
  ASSERT_TRUE(node_aba->isMemoryCapped());
  ASSERT_TRUE(node_b->isMemoryCapped());
  ASSERT_TRUE(node_ba->isMemoryCapped());
  ASSERT_TRUE(node_bb->isMemoryCapped());
  ASSERT_TRUE(node_bc->isMemoryCapped());
}

TEST(MemoryPoolTest, UncapMemory) {
  MemoryManager<MemoryAllocator> manager{};
  auto& root = manager.getRoot();

  auto node_a = root.addChild("node_a");
  auto node_aa = node_a->addChild("node_aa");
  auto node_ab = node_a->addChild("node_ab", 31);
  auto node_aba = node_ab->addChild("node_aba");

  auto node_b = root.addChild("node_b");
  auto node_ba = node_b->addChild("node_ba");
  auto node_bb = node_b->addChild("node_bb");
  auto node_bc = node_b->addChild("node_bc");

  // Uncap should be recursive.
  node_a->capMemoryAllocation();
  node_b->capMemoryAllocation();
  ASSERT_FALSE(root.isMemoryCapped());
  ASSERT_TRUE(node_a->isMemoryCapped());
  ASSERT_TRUE(node_aa->isMemoryCapped());
  ASSERT_TRUE(node_ab->isMemoryCapped());
  ASSERT_TRUE(node_aba->isMemoryCapped());
  ASSERT_TRUE(node_b->isMemoryCapped());
  ASSERT_TRUE(node_ba->isMemoryCapped());
  ASSERT_TRUE(node_bb->isMemoryCapped());
  ASSERT_TRUE(node_bc->isMemoryCapped());

  node_a->uncapMemoryAllocation();
  ASSERT_FALSE(root.isMemoryCapped());
  ASSERT_FALSE(node_a->isMemoryCapped());
  ASSERT_FALSE(node_aa->isMemoryCapped());
  ASSERT_FALSE(node_ab->isMemoryCapped());
  ASSERT_FALSE(node_aba->isMemoryCapped());

  ASSERT_TRUE(node_b->isMemoryCapped());
  ASSERT_TRUE(node_ba->isMemoryCapped());
  ASSERT_TRUE(node_bb->isMemoryCapped());
  ASSERT_TRUE(node_bc->isMemoryCapped());

  // Cannot uncap a node when parent is still capped.
  ASSERT_TRUE(node_b->isMemoryCapped());
  ASSERT_TRUE(node_bb->isMemoryCapped());
  node_bb->uncapMemoryAllocation();
  ASSERT_TRUE(node_b->isMemoryCapped());
  ASSERT_TRUE(node_bb->isMemoryCapped());

  // Don't uncap if the local cap is exceeded when intermediate
  // caps are supported again.
}

// Mainly tests how it tracks externally allocated memory.
TEST(MemoryPoolTest, ReserveTest) {
  MemoryManager<MemoryAllocator> manager{8 * GB};
  auto& root = manager.getRoot();

  auto child = root.addChild("elastic_quota");

  const int64_t kChunkSize{32L * MB};

  child->reserve(kChunkSize);
  ASSERT_EQ(child->getCurrentBytes(), kChunkSize);

  child->reserve(2 * kChunkSize);
  ASSERT_EQ(child->getCurrentBytes(), 3 * kChunkSize);

  child->release(1 * kChunkSize);
  ASSERT_EQ(child->getCurrentBytes(), 2 * kChunkSize);

  child->release(2 * kChunkSize);
  ASSERT_EQ(child->getCurrentBytes(), 0);
}

MachinePageCount numPagesNeeded(
    const MappedMemory* mappedMemory,
    MachinePageCount numPages) {
  auto& sizeClasses = mappedMemory->sizeClasses();
  if (numPages > sizeClasses.back()) {
    return numPages;
  }
  for (auto& sizeClass : sizeClasses) {
    if (sizeClass >= numPages) {
      return sizeClass;
    }
  }
  VELOX_UNREACHABLE();
}

void testMmapMemoryAllocation(
    const MmapAllocator* mmapAllocator,
    MachinePageCount allocPages,
    size_t allocCount) {
  MemoryManager<MmapMemoryAllocator> manager(8 * GB);
  const auto kPageSize = 4 * KB;

  auto& root = manager.getRoot();
  auto child = root.addChild("elastic_quota");

  std::vector<void*> allocations;
  uint64_t totalPageAllocated = 0;
  uint64_t totalPageMapped = 0;
  const auto pageIncrement = numPagesNeeded(mmapAllocator, allocPages);
  const auto isSizeClassAlloc =
      allocPages <= mmapAllocator->sizeClasses().back();
  const auto byteSize = allocPages * kPageSize;
  const std::string buffer(byteSize, 'x');
  for (size_t i = 0; i < allocCount; i++) {
    void* allocResult = nullptr;
    ASSERT_NO_THROW(allocResult = child->allocate(byteSize));
    ASSERT_TRUE(allocResult != nullptr);

    // Write data to let mapped address to be backed by physical memory
    memcpy(allocResult, buffer.data(), byteSize);
    allocations.emplace_back(allocResult);
    totalPageAllocated += pageIncrement;
    totalPageMapped += pageIncrement;
    ASSERT_EQ(mmapAllocator->numAllocated(), totalPageAllocated);
    ASSERT_EQ(
        isSizeClassAlloc ? mmapAllocator->numMapped()
                         : mmapAllocator->numExternalMapped(),
        totalPageMapped);
  }
  for (size_t i = 0; i < allocCount; i++) {
    ASSERT_NO_THROW(child->free(allocations[i], byteSize));
    totalPageAllocated -= pageIncrement;
    ASSERT_EQ(mmapAllocator->numAllocated(), totalPageAllocated);
    if (isSizeClassAlloc) {
      ASSERT_EQ(mmapAllocator->numMapped(), totalPageMapped);
    } else {
      totalPageMapped -= pageIncrement;
      ASSERT_EQ(mmapAllocator->numExternalMapped(), totalPageMapped);
    }
  }
}

TEST(MemoryPoolTest, SmallMmapMemoryAllocation) {
  MmapAllocatorOptions options;
  options.capacity = 8 * GB;
  auto mmapAllocator = std::make_unique<memory::MmapAllocator>(options);
  MappedMemory::setDefaultInstance(mmapAllocator.get());
  testMmapMemoryAllocation(mmapAllocator.get(), 6, 100);
}

TEST(MemoryPoolTest, BigMmapMemoryAllocation) {
  MmapAllocatorOptions options;
  options.capacity = 8 * GB;
  auto mmapAllocator = std::make_unique<memory::MmapAllocator>(options);
  MappedMemory::setDefaultInstance(mmapAllocator.get());
  testMmapMemoryAllocation(
      mmapAllocator.get(), mmapAllocator->sizeClasses().back() + 56, 20);
}

// Mainly tests how it updates the memory usage in Memorypool->
TEST_P(MemoryPoolTest, AllocTest) {
  auto manager = getMemoryManager(8 * GB);
  auto& root = manager->getRoot();

  auto child = root.addChild("elastic_quota");

  const int64_t kChunkSize{32L * MB};

  void* oneChunk = child->allocate(kChunkSize);
  ASSERT_EQ(kChunkSize, child->getCurrentBytes());
  ASSERT_EQ(kChunkSize, child->getMaxBytes());

  void* threeChunks = child->allocate(3 * kChunkSize);
  ASSERT_EQ(4 * kChunkSize, child->getCurrentBytes());
  ASSERT_EQ(4 * kChunkSize, child->getMaxBytes());

  child->free(threeChunks, 3 * kChunkSize);
  ASSERT_EQ(kChunkSize, child->getCurrentBytes());
  ASSERT_EQ(4 * kChunkSize, child->getMaxBytes());

  child->free(oneChunk, kChunkSize);
  ASSERT_EQ(0, child->getCurrentBytes());
  ASSERT_EQ(4 * kChunkSize, child->getMaxBytes());
}

TEST_P(MemoryPoolTest, ReallocTestSameSize) {
  auto manager = getMemoryManager(8 * GB);
  auto& root = manager->getRoot();

  auto pool = root.addChild("elastic_quota");

  const int64_t kChunkSize{32L * MB};

  // Realloc the same size.

  void* oneChunk = pool->allocate(kChunkSize);
  ASSERT_EQ(kChunkSize, pool->getCurrentBytes());
  ASSERT_EQ(kChunkSize, pool->getMaxBytes());

  void* anotherChunk = pool->reallocate(oneChunk, kChunkSize, kChunkSize);
  ASSERT_EQ(kChunkSize, pool->getCurrentBytes());
  ASSERT_EQ(kChunkSize, pool->getMaxBytes());

  pool->free(anotherChunk, kChunkSize);
  ASSERT_EQ(0, pool->getCurrentBytes());
  ASSERT_EQ(kChunkSize, pool->getMaxBytes());
}

TEST_P(MemoryPoolTest, ReallocTestHigher) {
  auto manager = getMemoryManager(8 * GB);
  auto& root = manager->getRoot();

  auto pool = root.addChild("elastic_quota");

  const int64_t kChunkSize{32L * MB};
  // Realloc higher.
  void* oneChunk = pool->allocate(kChunkSize);
  EXPECT_EQ(kChunkSize, pool->getCurrentBytes());
  EXPECT_EQ(kChunkSize, pool->getMaxBytes());

  void* threeChunks = pool->reallocate(oneChunk, kChunkSize, 3 * kChunkSize);
  EXPECT_EQ(3 * kChunkSize, pool->getCurrentBytes());
  EXPECT_EQ(3 * kChunkSize, pool->getMaxBytes());

  pool->free(threeChunks, 3 * kChunkSize);
  EXPECT_EQ(0, pool->getCurrentBytes());
  EXPECT_EQ(3 * kChunkSize, pool->getMaxBytes());
}

TEST_P(MemoryPoolTest, ReallocTestLower) {
  auto manager = getMemoryManager(8 * GB);
  auto& root = manager->getRoot();
  auto pool = root.addChild("elastic_quota");

  const int64_t kChunkSize{32L * MB};
  // Realloc lower.
  void* threeChunks = pool->allocate(3 * kChunkSize);
  EXPECT_EQ(3 * kChunkSize, pool->getCurrentBytes());
  EXPECT_EQ(3 * kChunkSize, pool->getMaxBytes());

  void* oneChunk = pool->reallocate(threeChunks, 3 * kChunkSize, kChunkSize);
  EXPECT_EQ(kChunkSize, pool->getCurrentBytes());
  EXPECT_EQ(3 * kChunkSize, pool->getMaxBytes());

  pool->free(oneChunk, kChunkSize);
  EXPECT_EQ(0, pool->getCurrentBytes());
  EXPECT_EQ(3 * kChunkSize, pool->getMaxBytes());
}

TEST_P(MemoryPoolTest, CapAllocation) {
  auto manager = getMemoryManager(8 * GB);
  auto& root = manager->getRoot();

  auto pool = root.addChild("static_quota", 64L * MB);

  // Capping malloc.
  {
    ASSERT_EQ(0, pool->getCurrentBytes());
    ASSERT_FALSE(pool->isMemoryCapped());
    void* oneChunk = pool->allocate(32L * MB);
    ASSERT_EQ(32L * MB, pool->getCurrentBytes());
    EXPECT_THROW(pool->allocate(34L * MB), velox::VeloxRuntimeError);
    EXPECT_FALSE(pool->isMemoryCapped());

    pool->free(oneChunk, 32L * MB);
  }
  // Capping realloc.
  {
    ASSERT_EQ(0, pool->getCurrentBytes());
    ASSERT_FALSE(pool->isMemoryCapped());
    void* oneChunk = pool->allocate(32L * MB);
    ASSERT_EQ(32L * MB, pool->getCurrentBytes());
    EXPECT_THROW(
        pool->reallocate(oneChunk, 32L * MB, 66L * MB),
        velox::VeloxRuntimeError);
    EXPECT_FALSE(pool->isMemoryCapped());

    pool->free(oneChunk, 32L * MB);
  }
}

TEST(MemoryPoolTest, MemoryCapExceptions) {
  MemoryManager<MemoryAllocator> manager{127L * MB};
  auto& root = manager.getRoot();

  auto pool = root.addChild("static_quota", 63L * MB);

  // Capping locally.
  {
    ASSERT_EQ(0, pool->getCurrentBytes());
    ASSERT_FALSE(pool->isMemoryCapped());
    try {
      pool->allocate(64L * MB);
    } catch (const velox::VeloxRuntimeError& ex) {
      EXPECT_EQ(error_source::kErrorSourceRuntime.c_str(), ex.errorSource());
      EXPECT_EQ(error_code::kMemCapExceeded.c_str(), ex.errorCode());
      EXPECT_TRUE(ex.isRetriable());
      EXPECT_EQ(
          "Exceeded memory cap of 63.00MB when requesting 64.00MB",
          ex.message());
    }
    ASSERT_FALSE(pool->isMemoryCapped());
  }
  // Capping memory manager.
  {
    ASSERT_EQ(0, pool->getCurrentBytes());
    ASSERT_FALSE(pool->isMemoryCapped());
    try {
      pool->allocate(128L * MB);
    } catch (const velox::VeloxRuntimeError& ex) {
      EXPECT_EQ(error_source::kErrorSourceRuntime.c_str(), ex.errorSource());
      EXPECT_EQ(error_code::kMemCapExceeded.c_str(), ex.errorCode());
      EXPECT_TRUE(ex.isRetriable());
      EXPECT_EQ("Exceeded memory manager cap of 127 MB", ex.message());
    }
    ASSERT_FALSE(pool->isMemoryCapped());
  }
  // Capping manually.
  {
    ASSERT_EQ(0, pool->getCurrentBytes());
    pool->capMemoryAllocation();
    ASSERT_TRUE(pool->isMemoryCapped());
    try {
      pool->allocate(8L * MB);
    } catch (const velox::VeloxRuntimeError& ex) {
      EXPECT_EQ(error_source::kErrorSourceRuntime.c_str(), ex.errorSource());
      EXPECT_EQ(error_code::kMemCapExceeded.c_str(), ex.errorCode());
      EXPECT_TRUE(ex.isRetriable());
      EXPECT_EQ("Memory allocation manually capped", ex.message());
    }
  }
}

TEST(MemoryPoolTest, GetAlignment) {
  {
    EXPECT_EQ(
        kNoAlignment,
        MemoryManager<MemoryAllocator>{32 * MB}.getRoot().getAlignment());
  }
  {
    MemoryManager<MemoryAllocator, 64> manager{32 * MB};
    EXPECT_EQ(64, manager.getRoot().getAlignment());
  }
}

TEST(MemoryPoolTest, MemoryManagerGlobalCap) {
  MemoryManager<MemoryAllocator> manager{32 * MB};

  auto& root = manager.getRoot();
  auto pool = root.addChild("unbounded");
  auto child = pool->addChild("unbounded");
  void* oneChunk = child->allocate(32L * MB);
  ASSERT_FALSE(root.isMemoryCapped());
  ASSERT_EQ(0L, root.getCurrentBytes());
  ASSERT_FALSE(child->isMemoryCapped());
  EXPECT_THROW(child->allocate(32L * MB), velox::VeloxRuntimeError);
  ASSERT_FALSE(root.isMemoryCapped());
  ASSERT_EQ(0L, root.getCurrentBytes());
  ASSERT_FALSE(child->isMemoryCapped());
  EXPECT_THROW(
      child->reallocate(oneChunk, 32L * MB, 64L * MB),
      velox::VeloxRuntimeError);
  child->free(oneChunk, 32L * MB);
}

// Tests how child updates itself and its parent's memory usage
// and what it returns for getCurrentBytes()/getMaxBytes and
// with memoryUsageTracker.
TEST(MemoryPoolTest, childUsageTest) {
  MemoryManager<MemoryAllocator> manager{8 * GB};
  auto& root = manager.getRoot();

  auto pool = root.addChild("main_pool");

  auto verifyUsage = [](std::vector<std::shared_ptr<MemoryPool>>& tree,
                        std::vector<int> currentBytes,
                        std::vector<int> maxBytes,
                        std::vector<int> trackerCurrentBytes,
                        std::vector<int> trackerMaxBytes) {
    ASSERT_TRUE(
        tree.size() == currentBytes.size() && tree.size() == maxBytes.size() &&
        tree.size() == trackerCurrentBytes.size() &&
        tree.size() == trackerMaxBytes.size());
    for (unsigned i = 0, e = tree.size(); i != e; ++i) {
      EXPECT_EQ(tree[i]->getCurrentBytes(), currentBytes[i]);
      EXPECT_EQ(tree[i]->getMaxBytes(), maxBytes[i]);
      auto tracker = tree[i]->getMemoryUsageTracker();
      ASSERT_TRUE(tracker);
      EXPECT_GE(tracker->getCurrentUserBytes(), trackerCurrentBytes[i]);
      EXPECT_GE(tracker->getPeakTotalBytes(), trackerMaxBytes[i]);
    }
  };

  // Create the following MemoryPool tree.
  //              p0
  //              |
  //      +-------+--------+
  //      |                |
  //     p1                p2
  //      |                |
  //  +------+         +---+---+
  // p3      p4       p5       p6
  //
  std::vector<std::shared_ptr<MemoryPool>> tree;
  tree.push_back(pool->addChild("p0"));
  tree[0]->setMemoryUsageTracker(MemoryUsageTracker::create());

  // first level: p1, p2.
  tree.push_back(tree[0]->addChild("p1"));
  tree.push_back(tree[0]->addChild("p2"));

  // second level: p3, p4, p5, p6.
  tree.push_back(tree[1]->addChild("p3"));
  tree.push_back(tree[1]->addChild("p4"));
  tree.push_back(tree[2]->addChild("p5"));
  tree.push_back(tree[2]->addChild("p6"));

  verifyUsage(
      tree,
      {0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0});

  void* p3Chunk0 = tree[3]->allocate(16);
  verifyUsage(
      tree,
      {0, 0, 0, 16, 0, 0, 0},
      {0, 0, 0, 16, 0, 0, 0},
      {16, 16, 0, 16, 0, 0, 0},
      {16, 16, 0, 16, 0, 0, 0});

  void* p5Chunk0 = tree[5]->allocate(64);
  verifyUsage(
      tree,
      {0, 0, 0, 16, 0, 64, 0},
      {0, 0, 0, 16, 0, 64, 0},
      {80, 16, 64, 16, 0, 64, 0},
      {80, 16, 64, 16, 0, 64, 0});

  tree[3]->free(p3Chunk0, 16);

  verifyUsage(
      tree,
      {0, 0, 0, 0, 0, 64, 0},
      {0, 0, 0, 16, 0, 64, 0},
      {64, 0, 64, 0, 0, 64, 0},
      {80, 16, 64, 16, 0, 64, 0});

  tree[5]->free(p5Chunk0, 64);
  verifyUsage(
      tree,
      {0, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 16, 0, 64, 0},
      {0, 0, 0, 0, 0, 0, 0},
      {80, 16, 64, 16, 0, 64, 0});

  std::vector<std::shared_ptr<MemoryUsageTracker>> trackers;
  for (unsigned i = 0, e = tree.size(); i != e; ++i) {
    trackers.push_back(tree[i]->getMemoryUsageTracker());
  }

  // Release all memory pool->
  tree.clear();

  std::vector<int64_t> expectedCurrentBytes({0, 0, 0, 0, 0, 0, 0});
  std::vector<int64_t> expectedMaxBytes({80, 16, 64, 16, 0, 64, 0});

  // Verify the stats still holds the correct stats.
  for (unsigned i = 0, e = trackers.size(); i != e; ++i) {
    ASSERT_GE(trackers[i]->getCurrentUserBytes(), expectedCurrentBytes[i]);
    ASSERT_GE(trackers[i]->getPeakTotalBytes(), expectedMaxBytes[i]);
  }
}

TEST(MemoryPoolTest, setMemoryUsageTrackerTest) {
  MemoryManager<MemoryAllocator> manager{};
  auto& root = manager.getRoot();
  const int64_t kChunkSize{32L * MB};
  {
    auto pool = root.addChild("empty_pool");
    auto tracker = SimpleMemoryTracker::create();
    pool->setMemoryUsageTracker(tracker);
    ASSERT_EQ(0, pool->getCurrentBytes());
    EXPECT_EQ(0, tracker->getCurrentUserBytes());
    void* chunk = pool->allocate(kChunkSize);
    ASSERT_EQ(kChunkSize, pool->getCurrentBytes());
    EXPECT_EQ(kChunkSize, tracker->getCurrentUserBytes());
    chunk = pool->reallocate(chunk, kChunkSize, 2 * kChunkSize);
    ASSERT_EQ(2 * kChunkSize, pool->getCurrentBytes());
    EXPECT_EQ(2 * kChunkSize, tracker->getCurrentUserBytes());
    pool->free(chunk, 2 * kChunkSize);
    ASSERT_EQ(0, pool->getCurrentBytes());
    EXPECT_EQ(0, tracker->getCurrentUserBytes());
  }
  {
    auto pool = root.addChild("nonempty_pool");
    ASSERT_EQ(0, pool->getCurrentBytes());
    auto tracker = SimpleMemoryTracker::create();
    void* chunk = pool->allocate(kChunkSize);
    ASSERT_EQ(kChunkSize, pool->getCurrentBytes());
    EXPECT_EQ(0, tracker->getCurrentUserBytes());
    pool->setMemoryUsageTracker(tracker);
    EXPECT_EQ(kChunkSize, tracker->getCurrentUserBytes());
    chunk = pool->reallocate(chunk, kChunkSize, 2 * kChunkSize);
    ASSERT_EQ(2 * kChunkSize, pool->getCurrentBytes());
    EXPECT_EQ(2 * kChunkSize, tracker->getCurrentUserBytes());
    pool->free(chunk, 2 * kChunkSize);
    ASSERT_EQ(0, pool->getCurrentBytes());
    EXPECT_EQ(0, tracker->getCurrentUserBytes());
  }
  {
    auto pool = root.addChild("switcheroo_pool");
    ASSERT_EQ(0, pool->getCurrentBytes());
    auto tracker = SimpleMemoryTracker::create();
    void* chunk = pool->allocate(kChunkSize);
    ASSERT_EQ(kChunkSize, pool->getCurrentBytes());
    EXPECT_EQ(0, tracker->getCurrentUserBytes());
    pool->setMemoryUsageTracker(tracker);
    EXPECT_EQ(kChunkSize, tracker->getCurrentUserBytes());
    pool->setMemoryUsageTracker(tracker);
    EXPECT_EQ(kChunkSize, tracker->getCurrentUserBytes());
    auto newTracker = SimpleMemoryTracker::create();
    pool->setMemoryUsageTracker(newTracker);
    EXPECT_EQ(0, tracker->getCurrentUserBytes());
    EXPECT_EQ(kChunkSize, newTracker->getCurrentUserBytes());

    chunk = pool->reallocate(chunk, kChunkSize, 2 * kChunkSize);
    ASSERT_EQ(2 * kChunkSize, pool->getCurrentBytes());
    EXPECT_EQ(0, tracker->getCurrentUserBytes());
    EXPECT_EQ(2 * kChunkSize, newTracker->getCurrentUserBytes());
    pool->free(chunk, 2 * kChunkSize);
    ASSERT_EQ(0, pool->getCurrentBytes());
    EXPECT_EQ(0, tracker->getCurrentUserBytes());
    EXPECT_EQ(0, newTracker->getCurrentUserBytes());
  }
}

TEST(MemoryPoolTest, mockUpdatesTest) {
  MemoryManager<MemoryAllocator> manager{};
  auto& root = manager.getRoot();
  const int64_t kChunkSize{32L * MB};
  {
    auto defaultTrackerPool = root.addChild("default_tracker_pool");
    auto defaultTracker = MemoryUsageTracker::create();
    defaultTrackerPool->setMemoryUsageTracker(defaultTracker);
    EXPECT_EQ(0, defaultTracker->getCurrentUserBytes());
    void* twoChunks = defaultTrackerPool->allocate(2 * kChunkSize);
    EXPECT_EQ(2 * kChunkSize, defaultTracker->getCurrentUserBytes());
    twoChunks =
        defaultTrackerPool->reallocate(twoChunks, 2 * kChunkSize, kChunkSize);
    EXPECT_EQ(kChunkSize, defaultTracker->getCurrentUserBytes());
    // We didn't do any real reallocation.
    defaultTrackerPool->free(twoChunks, 2 * kChunkSize);
  }
  {
    auto simpleTrackerPool = root.addChild("simple_tracker_pool");
    auto simpleTracker = SimpleMemoryTracker::create();
    simpleTrackerPool->setMemoryUsageTracker(simpleTracker);
    EXPECT_EQ(0, simpleTracker->getCurrentUserBytes());
    void* twoChunks = simpleTrackerPool->allocate(2 * kChunkSize);
    EXPECT_EQ(2 * kChunkSize, simpleTracker->getCurrentUserBytes());
    twoChunks =
        simpleTrackerPool->reallocate(twoChunks, 2 * kChunkSize, kChunkSize);
    EXPECT_EQ(2 * kChunkSize, simpleTracker->getCurrentUserBytes());
    // We didn't do any real reallocation.
    simpleTrackerPool->free(twoChunks, 2 * kChunkSize);
  }
}

TEST(MemoryPoolTest, getPreferredSize) {
  MemoryManager<MemoryAllocator, 64> manager{};
  auto& pool =
      dynamic_cast<MemoryPoolImpl<MemoryAllocator, 64>&>(manager.getRoot());

  // size < 8
  EXPECT_EQ(8, pool.getPreferredSize(1));
  EXPECT_EQ(8, pool.getPreferredSize(2));
  EXPECT_EQ(8, pool.getPreferredSize(4));
  EXPECT_EQ(8, pool.getPreferredSize(7));
  // size >=8, pick 2^k or 1.5 * 2^k
  EXPECT_EQ(8, pool.getPreferredSize(8));
  EXPECT_EQ(24, pool.getPreferredSize(24));
  EXPECT_EQ(32, pool.getPreferredSize(25));
  EXPECT_EQ(1024 * 1536, pool.getPreferredSize(1024 * 1024 + 1));
  EXPECT_EQ(1024 * 1024 * 2, pool.getPreferredSize(1024 * 1536 + 1));
}

TEST(MemoryPoolTest, getPreferredSizeOverflow) {
  MemoryManager<MemoryAllocator, 64> manager{};
  auto& pool =
      dynamic_cast<MemoryPoolImpl<MemoryAllocator, 64>&>(manager.getRoot());

  EXPECT_EQ(1ULL << 32, pool.getPreferredSize((1ULL << 32) - 1));
  EXPECT_EQ(1ULL << 63, pool.getPreferredSize((1ULL << 62) - 1 + (1ULL << 62)));
}

TEST(MemoryPoolTest, allocatorOverflow) {
  MemoryManager<MemoryAllocator, 64> manager{};
  auto& pool =
      dynamic_cast<MemoryPoolImpl<MemoryAllocator, 64>&>(manager.getRoot());
  Allocator<int64_t> alloc(pool);
  EXPECT_THROW(alloc.allocate(1ULL << 62), VeloxException);
  EXPECT_THROW(alloc.deallocate(nullptr, 1ULL << 62), VeloxException);
}

VELOX_INSTANTIATE_TEST_SUITE_P(
    MemoryPoolTestSuite,
    MemoryPoolTest,
    testing::Values(true, false));

} // namespace memory
} // namespace velox
} // namespace facebook
