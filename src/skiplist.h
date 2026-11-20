#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace strata {

template <class Key, class Value, class Compare = std::less<>>
class SkipList {
 public:
  ~SkipList() {
    // Release the owning chain iteratively, including very large memtables.
    while (first_) {
      auto head = std::move(first_);
      first_ = std::move(head->next);
    }
  }

  template <class ShouldReplace>
  void Upsert(Key key, Value value, ShouldReplace should_replace) {
    std::unique_lock lock(mutex_);
    std::array<Node*, kMaxHeight> previous{};
    Node* match = Find(key, &previous);
    if (match && !compare_(key, match->key)) {
      if (should_replace(match->value, value)) {
        match->value = std::move(value);
      }
      return;
    }

    const auto height = RandomHeight();
    auto node = std::make_unique<Node>(std::move(key), std::move(value), height);
    Node* inserted = node.get();
    auto& owner = previous[0] ? previous[0]->next : first_;
    node->next = std::move(owner);
    owner = std::move(node);

    for (std::size_t level = 1; level < height; ++level) {
      auto& link = HigherLink(previous[level], level);
      inserted->higher[level - 1] = link;
      link = inserted;
    }
  }

  template <class LookupKey>
  [[nodiscard]] std::optional<Value> Get(const LookupKey& key) const {
    std::shared_lock lock(mutex_);
    const Node* node = Find(key);
    if (node && !compare_(key, node->key)) {
      return node->value;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::vector<std::pair<Key, Value>> OrderedEntries() const {
    std::shared_lock lock(mutex_);
    std::vector<std::pair<Key, Value>> entries;
    for (const Node* node = first_.get(); node; node = node->next.get()) {
      entries.emplace_back(node->key, node->value);
    }
    return entries;
  }

 private:
  static constexpr std::size_t kMaxHeight = 20;

  struct Node {
    Node(Key key, Value value, std::size_t height)
        : key(std::move(key)), value(std::move(value)), higher(height - 1) {}

    Key key;
    Value value;
    std::vector<Node*> higher;
    std::unique_ptr<Node> next;
  };

  Node*& HigherLink(Node* previous, std::size_t level) {
    return previous ? previous->higher[level - 1] : higher_[level - 1];
  }

  Node* Next(Node* previous, std::size_t level) const {
    if (level == 0) {
      return previous ? previous->next.get() : first_.get();
    }
    return previous ? previous->higher[level - 1] : higher_[level - 1];
  }

  template <class LookupKey>
  Node* Find(const LookupKey& key, std::array<Node*, kMaxHeight>* previous = nullptr) const {
    Node* node = nullptr;
    for (std::size_t level = kMaxHeight; level-- > 0;) {
      while (Node* next = Next(node, level)) {
        if (!compare_(next->key, key)) {
          break;
        }
        node = next;
      }
      if (previous) {
        (*previous)[level] = node;
      }
    }
    return Next(node, 0);
  }

  std::size_t RandomHeight() {
    std::size_t height = 1;
    while (height < kMaxHeight && (random_() & 3) == 0) {
      ++height;
    }
    return height;
  }

  mutable std::shared_mutex mutex_;
  Compare compare_;
  std::mt19937 random_;
  std::unique_ptr<Node> first_;
  std::array<Node*, kMaxHeight - 1> higher_{};
};

}  // namespace strata
