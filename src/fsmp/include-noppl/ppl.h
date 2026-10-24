// Stand-in for the Microsoft Parallel Patterns Library on compilers without
// it, for FSMP and Bullet's PPL task scheduler. The loops and tasks run in
// order on the calling thread.
#pragma once

#include <functional>
#include <iterator>
#include <utility>
#include <vector>

namespace concurrency
{
	template <class Index, class Function>
	void parallel_for(Index a_first, Index a_last, const Function& a_function)
	{
		for (Index i = a_first; i < a_last; ++i)
			a_function(i);
	}

	template <class Index, class Function>
	void parallel_for(Index a_first, Index a_last, Index a_step, const Function& a_function)
	{
		for (Index i = a_first; i < a_last; i += a_step)
			a_function(i);
	}

	template <class Iterator, class Function>
	void parallel_for_each(Iterator a_first, Iterator a_last, const Function& a_function)
	{
		for (; a_first != a_last; ++a_first)
			a_function(*a_first);
	}

	template <class T>
	class task
	{};

	// The calling thread is the only one.
	inline unsigned int GetProcessorCount()
	{
		return 1;
	}

	template <class T>
	class combinable
	{
	public:
		T& local() { return _value; }

		template <class Function>
		T combine(Function) const
		{
			return _value;
		}

		void clear() { _value = T(); }

	private:
		T _value{};
	};

	class SchedulerPolicy
	{
	public:
		void SetConcurrencyLimits(unsigned int, unsigned int) {}
	};

	class CurrentScheduler
	{
	public:
		static long Id() { return -1; }
		static void Create(const SchedulerPolicy&) {}
		static void Detach() {}
	};

	enum task_group_status
	{
		not_complete,
		completed,
		canceled
	};

	// Like PPL's task group, run() returns before the task has run: tasks run in
	// order when wait() is called.
	class task_group
	{
	public:
		template <class Function>
		void run(const Function& a_function)
		{
			_pending.emplace_back(a_function);
		}

		task_group_status wait()
		{
			while (!_pending.empty()) {
				auto task = std::move(_pending.front());
				_pending.erase(_pending.begin());
				task();
			}
			return completed;
		}

	private:
		std::vector<std::function<void()>> _pending;
	};
}
