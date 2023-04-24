/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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


namespace facebook {
namespace cachelib {

/* when linkedAtHead uses atomic op,
 * it is possible to conflict with the node that unlink the head when using
 */

/* Linked list implemenation */
template <typename T, SieveListBufferedHook<T> T::*HookPtr>
void SieveListBuffered<T, HookPtr>::linkAtHead(T& node) noexcept {
  setPrev(node, nullptr);

  T* oldHead = head_.load();
  setNext(node, oldHead);

  while (!head_.compare_exchange_weak(oldHead, &node)) {
    setNext(node, oldHead);
  }

  if (oldHead == nullptr) {
    // this is the thread that first makes head_ points to the node
    // other threads must follow this, o.w. oldHead will be nullptr
    XDCHECK_EQ(tail_, nullptr);
    XDCHECK_EQ(curr_, nullptr);

    T *tail = nullptr, *curr = nullptr;
    tail_.compare_exchange_strong(tail, &node);
    curr_.compare_exchange_strong(curr, &node);
  } else {
    setPrev(*oldHead, &node);
  }

  size_++;
}

template <typename T, SieveListBufferedHook<T> T::*HookPtr>
void SieveListBuffered<T, HookPtr>::unlink(const T& node) noexcept {
  if (mtx_->try_lock()) {
    // we should have locked the mutex
    abort();
  }

  XDCHECK_GT(size_, 0u);
  auto* const prev = getPrev(node);
  auto* const next = getNext(node);

  if (&node == head_) {
    head_ = next;
    XDCHECK_NE(head_.load(), nullptr);
  }
  if (&node == tail_) {
    if (prev == nullptr || tail_.load() == nullptr) {
      printf("node %p prev %p next %p tail_ %p head_ %p size %lu\n", &node,
             prev, next, tail_.load(), head_.load(), size_.load());
      abort();
    }
    tail_ = prev;
  }
  if (&node == curr_) {
    curr_ = prev;
  }

  // fix the next and prev ptrs of the node before and after us.
  if (prev != nullptr) {
    setNextFrom(*prev, node);
  }
  if (next != nullptr) {
    setPrevFrom(*next, node);
  }
  size_--;
}

template <typename T, SieveListBufferedHook<T> T::*HookPtr>
void SieveListBuffered<T, HookPtr>::remove(T& node) noexcept {
  auto* const prev = getPrev(node);
  auto* const next = getNext(node);
  if (prev == nullptr && next == nullptr) {
    return;
  }

  LockHolder l(*mtx_);
  unlink(node);
  setNext(node, nullptr);
  setPrev(node, nullptr);
}

template <typename T, SieveListBufferedHook<T> T::*HookPtr>
void SieveListBuffered<T, HookPtr>::replace(T& oldNode, T& newNode) noexcept {
  printf("replace\n");
  LockHolder l(*mtx_);

  // Update head and tail links if needed
  if (&oldNode == head_) {
    head_ = &newNode;
  }
  if (&oldNode == tail_) {
    tail_ = &newNode;
  }
  if (&oldNode == curr_) {
    curr_ = &newNode;
  }

  // Make the previous and next nodes point to the new node
  auto* const prev = getPrev(oldNode);
  auto* const next = getNext(oldNode);
  if (prev != nullptr) {
    setNext(*prev, &newNode);
  }
  if (next != nullptr) {
    setPrev(*next, &newNode);
  }

  // Make the new node point to the previous and next nodes
  setPrev(newNode, prev);
  setNext(newNode, next);

  // Cleanup the old node
  setPrev(oldNode, nullptr);
  setNext(oldNode, nullptr);
}

template <typename T, SieveListBufferedHook<T> T::*HookPtr>
void SieveListBuffered<T, HookPtr>::moveToHead(T& node) noexcept {
  if (&node == head_) {
    return;
  }
  unlink(node);
  linkAtHead(node);
}

#ifdef USE_MPMC_QUEUE
template <typename T, SieveListBufferedHook<T> T::*HookPtr>
T* SieveListBuffered<T, HookPtr>::getEvictionCandidate() noexcept {
  // LockHolder l(*mtx_);
  if (evictCandidateQueue_.sizeGuess() < nMaxEvictionCandidates_ / 4) {
    if (size_.load() == 0) {
      return NULL;
    }

    prepareEvictionCandidates();
  }

  T* ret = nullptr;
  int nTries = 0;
  while (!evictCandidateQueue_.read(ret)) {
    if ((nTries++) % 100 == 0) {
      prepareEvictionCandidates();
    } else {
      if (nTries % 100000 == 0) {
        printf("thread %ld has %d attempts\n", pthread_self(), nTries);
      }
    }
  }

  return ret;
}

template <typename T, SieveListBufferedHook<T> T::*HookPtr>
void SieveListBuffered<T, HookPtr>::prepareEvictionCandidates() noexcept {
  LockHolder l(*mtx_);
  if (evictCandidateQueue_.sizeGuess() > nMaxEvictionCandidates_ / 4 * 3) {
    return;
  }
  int nCandidate = nCandidateToPrepare();
  int n_iters = 0;

  T* curr = curr_.load();
  T* headWhenStart = head_.load();
  T* next;
  T* firstRetainedNode = curr;
  while (nCandidate > 0) {
    if (curr == headWhenStart)
    // we turn around when we reach headWhenStart to avoid
    // delinking the head and conflicting with the insert operation
    /****** clock *******/
    // if (curr == head_.load())
    /****** clock *******/
    {
      curr = tail_.load();
      if (n_iters++ > 2) {
        printf("n_iters = %d\n", n_iters);
        abort();
      }
    }
    if (isAccessed(*curr)) {
      unmarkAccessed(*curr);
      curr = getPrev(*curr);

      /****** clock *******/
      // next = getPrev(*curr);
      // // move the node to the head of the list
      // moveToHead(*curr);
      // curr = next;
      /****** clock *******/

    } else {
      nCandidate--;
      next = getPrev(*curr);
      if (evictCandidateQueue_.write(curr)) {
        unlink(*curr);
        setNext(*curr, nullptr);
        setPrev(*curr, nullptr);
      }
      curr = next;
    }
  }
  curr_.store(curr);
}
#endif

#ifdef USE_EVICTION_BUFFER
template <typename T, SieveListBufferedHook<T> T::*HookPtr>
T* SieveListBuffered<T, HookPtr>::getEvictionCandidate() noexcept {
  // TODO it may happen that the prepare happen while some other threads are
  // still using the old eviction candidates. This is not a correctness issue
  // we can use two buffers and atomic swap to avoid this
  // or we can have a per-thread buffer
  // LockHolder l(*mtx_);
  size_t idx = bufIdx_.fetch_add(1);
  while (idx >= nEvictionCandidates_.load()) {
    if (size_.load() == 0) {
      return NULL;
    }

    prepareEvictionCandidates();
    idx = bufIdx_.fetch_add(1);
  }
  T* ret = evictCandidateBuf_[idx];
  evictCandidateBuf_[idx].store(nullptr);
  return ret;
}

template <typename T, SieveListBufferedHook<T> T::*HookPtr>
void SieveListBuffered<T, HookPtr>::prepareEvictionCandidates() noexcept {
  // TODO: we can release the candidate early
  LockHolder l(*mtx_);
  if (bufIdx_.load() < nEvictionCandidates_.load()) {
    return;
  }

  size_t idx = 0;
  int nCandidate = nCandidateToPrepare();
  int n_iters = 0;

  T* curr = curr_.load();
  T* headWhenStart = head_.load();
  T* next;
  // the nodes between first and last retaiend ndoes (curr)
  // are moved to eviction candidate buffer
  T* firstRetainedNode = curr;
  while (idx < nCandidate) {
    if (curr == headWhenStart || curr == nullptr)
    // we turn around when we reach headWhenStart to avoid
    // delinking the head and conflicting with the insert operation
    // the nullptr check is because of a bug that appears rarely
    /****** clock *******/
    // if (curr == head_.load())
    /****** clock *******/
    {
      curr = tail_.load();
      if (n_iters++ > 2) {
        printf("n_iters = %d\n", n_iters);
        abort();
      }
    }
    if (isAccessed(*curr)) {
      unmarkAccessed(*curr);
      curr = getPrev(*curr);
      /****** clock *******/
      // next = getPrev(*curr);
      // // move the node to the head of the list
      // moveToHead(*curr);
      // curr = next;
    /****** clock *******/
    } else {
      while (evictCandidateBuf_[idx] != nullptr) {
        // spin and wait for the evict candidate to be fetched
        ;
      }
      evictCandidateBuf_[idx++] = curr;
      next = getPrev(*curr);
      unlink(*curr);
      setNext(*curr, nullptr);
      setPrev(*curr, nullptr);
      curr = next;
    }
    if (curr == nullptr) {
      // this can happen if head_ is updated to a newly inserted node,
      // but the old head has not pointed to the new head yet.
      curr = tail_.load();
    }
  }
  curr_.store(curr);
  nEvictionCandidates_.store(nCandidate);
  bufIdx_.store(0);
}
#endif


/* Iterator Implementation */
template <typename T, SieveListBufferedHook<T> T::*HookPtr>
void SieveListBuffered<T, HookPtr>::Iterator::goForward() noexcept {
  if (dir_ == Direction::FROM_TAIL) {
    curr_ = SieveListBuffered_->getPrev(*curr_);
  } else {
    curr_ = SieveListBuffered_->getNext(*curr_);
  }
}

template <typename T, SieveListBufferedHook<T> T::*HookPtr>
void SieveListBuffered<T, HookPtr>::Iterator::goBackward() noexcept {
  if (dir_ == Direction::FROM_TAIL) {
    curr_ = SieveListBuffered_->getNext(*curr_);
  } else {
    curr_ = SieveListBuffered_->getPrev(*curr_);
  }
}

template <typename T, SieveListBufferedHook<T> T::*HookPtr>
typename SieveListBuffered<T, HookPtr>::Iterator&
SieveListBuffered<T, HookPtr>::Iterator::operator++() noexcept {
  XDCHECK(curr_ != nullptr);
  if (curr_ != nullptr) {
    goForward();
  }
  return *this;
}

template <typename T, SieveListBufferedHook<T> T::*HookPtr>
typename SieveListBuffered<T, HookPtr>::Iterator&
SieveListBuffered<T, HookPtr>::Iterator::operator--() noexcept {
  XDCHECK(curr_ != nullptr);
  if (curr_ != nullptr) {
    goBackward();
  }
  return *this;
}

template <typename T, SieveListBufferedHook<T> T::*HookPtr>
typename SieveListBuffered<T, HookPtr>::Iterator
SieveListBuffered<T, HookPtr>::begin() const noexcept {
  return SieveListBuffered<T, HookPtr>::Iterator(
      head_, Iterator::Direction::FROM_HEAD, *this);
}

template <typename T, SieveListBufferedHook<T> T::*HookPtr>
typename SieveListBuffered<T, HookPtr>::Iterator
SieveListBuffered<T, HookPtr>::rbegin() const noexcept {
  return SieveListBuffered<T, HookPtr>::Iterator(
      tail_, Iterator::Direction::FROM_TAIL, *this);
}

template <typename T, SieveListBufferedHook<T> T::*HookPtr>
typename SieveListBuffered<T, HookPtr>::Iterator
SieveListBuffered<T, HookPtr>::end() const noexcept {
  return SieveListBuffered<T, HookPtr>::Iterator(
      nullptr, Iterator::Direction::FROM_HEAD, *this);
}

template <typename T, SieveListBufferedHook<T> T::*HookPtr>
typename SieveListBuffered<T, HookPtr>::Iterator
SieveListBuffered<T, HookPtr>::rend() const noexcept {
  return SieveListBuffered<T, HookPtr>::Iterator(
      nullptr, Iterator::Direction::FROM_TAIL, *this);
}

}  // namespace cachelib
}  // namespace facebook
