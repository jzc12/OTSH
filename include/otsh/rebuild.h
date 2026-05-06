#pragma once

#include <deque>
#include <functional>

namespace otsh {

// 后台重建/调度
// - 维护一个任务队列，每次操作消耗固定 budget 执行少量重建工作
class RebuildScheduler {
public:
  using Task = std::function<void()>;

  void enqueue(Task t) { tasks_.push_back(std::move(t)); }

  // 执行最多 units 个任务。
  void step_budget(int units) {
    while (units-- > 0 && !tasks_.empty()) {
      Task t = std::move(tasks_.front());
      tasks_.pop_front();
      if (t)
        t();
    }
  }

  bool empty() const { return tasks_.empty(); }
  void clear() { tasks_.clear(); }

private:
  std::deque<Task> tasks_;
};

} // namespace otsh
