/*
 * Copyright 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <gui/SurfaceComposerClient.h>

#include "Tracing/RingBuffer.h"
#include "Tracing/TransactionTracing.h"

using namespace android::surfaceflinger;

namespace android {

class TransactionTracingTest : public testing::Test {
protected:
    static constexpr size_t SMALL_BUFFER_SIZE = 1024;
    std::unique_ptr<android::TransactionTracing> mTracing;
    void SetUp() override { mTracing = std::make_unique<android::TransactionTracing>(); }

    void TearDown() override {
        mTracing->disable();
        mTracing.reset();
    }

    auto getCommittedTransactions() {
        std::scoped_lock<std::mutex> lock(mTracing->mMainThreadLock);
        return mTracing->mCommittedTransactions;
    }

    auto getQueuedTransactions() {
        std::scoped_lock<std::mutex> lock(mTracing->mTraceLock);
        return mTracing->mQueuedTransactions;
    }

    auto getUsedBufferSize() {
        std::scoped_lock<std::mutex> lock(mTracing->mTraceLock);
        return mTracing->mBuffer->used();
    }

    auto flush(int64_t vsyncId) { return mTracing->flush(vsyncId); }

    auto bufferFront() {
        std::scoped_lock<std::mutex> lock(mTracing->mTraceLock);
        return mTracing->mBuffer->front();
    }

    bool threadIsJoinable() {
        std::scoped_lock lock(mTracing->mMainThreadLock);
        return mTracing->mThread.joinable();
    }

    proto::TransactionTraceFile writeToProto() { return mTracing->writeToProto(); }

    auto getCreatedLayers() {
        std::scoped_lock<std::mutex> lock(mTracing->mTraceLock);
        return mTracing->mCreatedLayers;
    }

    auto getStartingStates() {
        std::scoped_lock<std::mutex> lock(mTracing->mTraceLock);
        return mTracing->mStartingStates;
    }

    void queueAndCommitTransaction(int64_t vsyncId) {
        TransactionState transaction;
        transaction.id = static_cast<uint64_t>(vsyncId * 3);
        transaction.originUid = 1;
        transaction.originPid = 2;
        mTracing->addQueuedTransaction(transaction);
        std::vector<TransactionState> transactions;
        transactions.emplace_back(transaction);
        mTracing->addCommittedTransactions(transactions, vsyncId);
        flush(vsyncId);
    }

    // Test that we clean up the tracing thread and free any memory allocated.
    void verifyDisabledTracingState() {
        EXPECT_FALSE(mTracing->isEnabled());
        EXPECT_FALSE(threadIsJoinable());
        EXPECT_EQ(getCommittedTransactions().size(), 0u);
        EXPECT_EQ(getQueuedTransactions().size(), 0u);
        EXPECT_EQ(getUsedBufferSize(), 0u);
        EXPECT_EQ(getStartingStates().size(), 0u);
    }

    void verifyEntry(const proto::TransactionTraceEntry& actualProto,
                     const std::vector<TransactionState> expectedTransactions,
                     int64_t expectedVsyncId) {
        EXPECT_EQ(actualProto.vsync_id(), expectedVsyncId);
        EXPECT_EQ(actualProto.transactions().size(),
                  static_cast<int32_t>(expectedTransactions.size()));
        for (uint32_t i = 0; i < expectedTransactions.size(); i++) {
            EXPECT_EQ(actualProto.transactions(static_cast<int32_t>(i)).pid(),
                      expectedTransactions[i].originPid);
        }
    }
};

TEST_F(TransactionTracingTest, enable) {
    EXPECT_FALSE(mTracing->isEnabled());
    mTracing->enable();
    EXPECT_TRUE(mTracing->isEnabled());
    mTracing->disable();
    verifyDisabledTracingState();
}

TEST_F(TransactionTracingTest, addTransactions) {
    mTracing->enable();
    std::vector<TransactionState> transactions;
    transactions.reserve(100);
    for (uint64_t i = 0; i < 100; i++) {
        TransactionState transaction;
        transaction.id = i;
        transaction.originPid = static_cast<int32_t>(i);
        transactions.emplace_back(transaction);
        mTracing->addQueuedTransaction(transaction);
    }

    // Split incoming transactions into two and commit them in reverse order to test out of order
    // commits.
    std::vector<TransactionState> firstTransactionSet =
            std::vector<TransactionState>(transactions.begin() + 50, transactions.end());
    int64_t firstTransactionSetVsyncId = 42;
    mTracing->addCommittedTransactions(firstTransactionSet, firstTransactionSetVsyncId);

    int64_t secondTransactionSetVsyncId = 43;
    std::vector<TransactionState> secondTransactionSet =
            std::vector<TransactionState>(transactions.begin(), transactions.begin() + 50);
    mTracing->addCommittedTransactions(secondTransactionSet, secondTransactionSetVsyncId);
    flush(secondTransactionSetVsyncId);

    proto::TransactionTraceFile proto = writeToProto();
    EXPECT_EQ(proto.entry().size(), 3);
    // skip starting entry
    verifyEntry(proto.entry(1), firstTransactionSet, firstTransactionSetVsyncId);
    verifyEntry(proto.entry(2), secondTransactionSet, secondTransactionSetVsyncId);

    mTracing->disable();
    verifyDisabledTracingState();
}

class TransactionTracingLayerHandlingTest : public TransactionTracingTest {
protected:
    void SetUp() override {
        TransactionTracingTest::SetUp();
        mTracing->enable();
        // add layers
        mTracing->setBufferSize(SMALL_BUFFER_SIZE);
        const sp<IBinder> fakeLayerHandle = new BBinder();
        mTracing->onLayerAdded(fakeLayerHandle->localBinder(), mParentLayerId, "parent",
                               123 /* flags */, -1 /* parentId */);
        const sp<IBinder> fakeChildLayerHandle = new BBinder();
        mTracing->onLayerAdded(fakeChildLayerHandle->localBinder(), mChildLayerId, "child",
                               456 /* flags */, mParentLayerId);

        // add some layer transaction
        {
            TransactionState transaction;
            transaction.id = 50;
            ComposerState layerState;
            layerState.state.surface = fakeLayerHandle;
            layerState.state.what = layer_state_t::eLayerChanged;
            layerState.state.z = 42;
            transaction.states.add(layerState);
            ComposerState childState;
            childState.state.surface = fakeChildLayerHandle;
            childState.state.what = layer_state_t::eLayerChanged;
            childState.state.z = 43;
            transaction.states.add(childState);
            mTracing->addQueuedTransaction(transaction);

            std::vector<TransactionState> transactions;
            transactions.emplace_back(transaction);
            VSYNC_ID_FIRST_LAYER_CHANGE = ++mVsyncId;
            mTracing->addCommittedTransactions(transactions, VSYNC_ID_FIRST_LAYER_CHANGE);
            flush(VSYNC_ID_FIRST_LAYER_CHANGE);
        }

        // add transactions that modify the layer state further so we can test that layer state
        // gets merged
        {
            TransactionState transaction;
            transaction.id = 51;
            ComposerState layerState;
            layerState.state.surface = fakeLayerHandle;
            layerState.state.what = layer_state_t::eLayerChanged | layer_state_t::ePositionChanged;
            layerState.state.z = 41;
            layerState.state.x = 22;
            transaction.states.add(layerState);
            mTracing->addQueuedTransaction(transaction);

            std::vector<TransactionState> transactions;
            transactions.emplace_back(transaction);
            VSYNC_ID_SECOND_LAYER_CHANGE = ++mVsyncId;
            mTracing->addCommittedTransactions(transactions, VSYNC_ID_SECOND_LAYER_CHANGE);
            flush(VSYNC_ID_SECOND_LAYER_CHANGE);
        }

        // remove child layer
        mTracing->onLayerRemoved(2);
        VSYNC_ID_CHILD_LAYER_REMOVED = ++mVsyncId;
        queueAndCommitTransaction(VSYNC_ID_CHILD_LAYER_REMOVED);

        // remove layer
        mTracing->onLayerRemoved(1);
        queueAndCommitTransaction(++mVsyncId);
    }

    void TearDown() override {
        mTracing->disable();
        verifyDisabledTracingState();
        TransactionTracingTest::TearDown();
    }

    int mParentLayerId = 1;
    int mChildLayerId = 2;
    int64_t mVsyncId = 0;
    int64_t VSYNC_ID_FIRST_LAYER_CHANGE;
    int64_t VSYNC_ID_SECOND_LAYER_CHANGE;
    int64_t VSYNC_ID_CHILD_LAYER_REMOVED;
};

TEST_F(TransactionTracingLayerHandlingTest, addStartingState) {
    // add transactions until we drop the transaction with the first layer change
    while (bufferFront().vsync_id() <= VSYNC_ID_FIRST_LAYER_CHANGE) {
        queueAndCommitTransaction(++mVsyncId);
    }
    proto::TransactionTraceFile proto = writeToProto();
    // verify we can still retrieve the layer change from the first entry containing starting
    // states.
    EXPECT_GT(proto.entry().size(), 0);
    EXPECT_GT(proto.entry(0).transactions().size(), 0);
    EXPECT_GT(proto.entry(0).added_layers().size(), 0);
    EXPECT_EQ(proto.entry(0).transactions(0).layer_changes().size(), 2);
    EXPECT_EQ(proto.entry(0).transactions(0).layer_changes(0).layer_id(), mParentLayerId);
    EXPECT_EQ(proto.entry(0).transactions(0).layer_changes(0).z(), 42);
    EXPECT_EQ(proto.entry(0).transactions(0).layer_changes(1).layer_id(), mChildLayerId);
    EXPECT_EQ(proto.entry(0).transactions(0).layer_changes(1).z(), 43);
}

TEST_F(TransactionTracingLayerHandlingTest, updateStartingState) {
    // add transactions until we drop the transaction with the second layer change
    while (bufferFront().vsync_id() <= VSYNC_ID_SECOND_LAYER_CHANGE) {
        queueAndCommitTransaction(++mVsyncId);
    }
    proto::TransactionTraceFile proto = writeToProto();
    // verify starting states are updated correctly
    EXPECT_EQ(proto.entry(0).transactions(0).layer_changes(0).z(), 41);
}

TEST_F(TransactionTracingLayerHandlingTest, removeStartingState) {
    // add transactions until we drop the transaction which removes the child layer
    while (bufferFront().vsync_id() <= VSYNC_ID_CHILD_LAYER_REMOVED) {
        queueAndCommitTransaction(++mVsyncId);
    }
    proto::TransactionTraceFile proto = writeToProto();
    // verify the child layer has been removed from the trace
    EXPECT_EQ(proto.entry(0).transactions(0).layer_changes().size(), 1);
    EXPECT_EQ(proto.entry(0).transactions(0).layer_changes(0).layer_id(), mParentLayerId);
}

TEST_F(TransactionTracingLayerHandlingTest, startingStateSurvivesBufferFlush) {
    // add transactions until we drop the transaction with the second layer change
    while (bufferFront().vsync_id() <= VSYNC_ID_SECOND_LAYER_CHANGE) {
        queueAndCommitTransaction(++mVsyncId);
    }
    proto::TransactionTraceFile proto = writeToProto();
    // verify we have two starting states
    EXPECT_EQ(proto.entry(0).transactions(0).layer_changes().size(), 2);

    // Continue adding transactions until child layer is removed
    while (bufferFront().vsync_id() <= VSYNC_ID_CHILD_LAYER_REMOVED) {
        queueAndCommitTransaction(++mVsyncId);
    }
    proto = writeToProto();
    // verify we still have the parent layer state
    EXPECT_EQ(proto.entry(0).transactions(0).layer_changes().size(), 1);
    EXPECT_EQ(proto.entry(0).transactions(0).layer_changes(0).layer_id(), mParentLayerId);
}

} // namespace android
