#pragma once


#include <atomic>
#include <memory>
#include <set>

#include "tbb/concurrent_vector.h"
#include "tbb/queuing_mutex.h"

#include "react/ReactiveDomain.h"
#include "react/common/Concurrency.h"

namespace react {

enum class ETransactionMode
{
	none,
	exclusive
};

////////////////////////////////////////////////////////////////////////////////////////
/// IReactiveEngine
////////////////////////////////////////////////////////////////////////////////////////
template
<
	typename TNodeInterface,
	typename TTurnInterface
>
struct IReactiveEngine
{
	using NodeInterface = TNodeInterface;
	using TurnInterface = TTurnInterface;

	void OnNodeCreate(NodeInterface& node)								{}
	void OnNodeDestroy(NodeInterface& node)								{}

	void OnNodeAttach(NodeInterface& node, NodeInterface& parent)		{}
	void OnNodeDetach(NodeInterface& node, NodeInterface& parent)		{}

	void OnTurnAdmissionStart(TurnInterface& turn)						{}
	void OnTurnAdmissionEnd(TurnInterface& turn)						{}
	void OnTurnEnd(TurnInterface& turn)									{}

	void OnTurnInputChange(NodeInterface& node, TurnInterface& turn)	{}
	void OnTurnPropagate(TurnInterface& turn)							{}

	void OnNodePulse(NodeInterface& node, TurnInterface& turn)			{}
	void OnNodeIdlePulse(NodeInterface& node, TurnInterface& turn)		{}

	void OnNodeShift(NodeInterface& node, NodeInterface& oldParent, NodeInterface& newParent, TurnInterface& turn)	{}
};

////////////////////////////////////////////////////////////////////////////////////////
/// TurnBase
////////////////////////////////////////////////////////////////////////////////////////
class TurnBase
{
public:
	TurnBase(TurnIdT id, TurnFlagsT flags) :
		id_{ id }
	{
	}

	inline TurnIdT Id() const		{ return id_; }

	inline void QueueForDetach(IObserverNode& obs)
	{
		if (detachedObserversPtr_ == nullptr)
			detachedObserversPtr_ = std::unique_ptr<ObsVectT>(new ObsVectT());

		detachedObserversPtr_->push_back(&obs);
	}

	template <typename D, typename TPolicy>
	friend class DomainBase;

private:
	using ObsVectT = tbb::concurrent_vector<IObserverNode*>;

	TurnIdT	id_;

	template <typename TRegistry>
	void detachObservers(TRegistry& registry)
	{
		if (detachedObserversPtr_ != nullptr)
			for (auto* o : *detachedObserversPtr_)
				registry.Unregister(o);
	}


	std::unique_ptr<ObsVectT>	detachedObserversPtr_;
	ContinuationInput continuation_;
};

////////////////////////////////////////////////////////////////////////////////////////
/// ExclusiveTurnManager
////////////////////////////////////////////////////////////////////////////////////////
class ExclusiveTurnManager
{
public:
	class ExclusiveTurn
	{
	public:
		explicit ExclusiveTurn(TurnFlagsT flags) :
			isMergeable_{ (flags & enable_input_merging) != 0 }
		{
		}

		inline void Append(ExclusiveTurn& tr)
		{
			successor_ = &tr;
			tr.blockCondition_.Block();
		}

		inline void WaitForUnblock()
		{
			blockCondition_.WaitForUnblock();
		}

		inline void RunMergedInputs()
		{
			for (const auto& e : merged_)
				e.first();
		}

		inline void UnblockSuccessors()
		{
			for (const auto& e : merged_)
				e.second->Unblock();

			if (successor_)
				successor_->blockCondition_.Unblock();
		}

		template <typename F>
		inline bool TryMerge(F&& inputFunc, BlockingCondition& caller)
		{
			if (!isMergeable_)
				return false;

			// Only merge if target is still blocked
			bool merged = blockCondition_.RunIfBlocked([&] {
				caller.Block();
				merged_.emplace_back(std::make_pair(std::forward<F>(inputFunc), &caller));
			});

			return merged;
		}

	private:
		using MergedDataVectT = std::vector<std::pair<std::function<void()>,BlockingCondition*>>;

		bool				isMergeable_;
		ExclusiveTurn*		successor_ = nullptr;
		MergedDataVectT		merged_;
		BlockingCondition	blockCondition_;
	};

	template <typename F>
	inline bool TryMerge(F&& inputFunc)
	{
		bool merged = false;

		BlockingCondition caller;

		{// seqMutex_
			SeqMutexT::scoped_lock lock(seqMutex_);

			if (tail_)
				merged = tail_->TryMerge(std::forward<F>(inputFunc), caller);
		}// ~seqMutex_

		if (merged)
			caller.WaitForUnblock();

		return merged;
	}

	inline void StartTurn(ExclusiveTurn& turn)
	{
		{// seqMutex_
			SeqMutexT::scoped_lock lock(seqMutex_);

			if (tail_)
				tail_->Append(turn);

			tail_ = &turn;
		}// ~seqMutex_

		turn.WaitForUnblock();
	}

	inline void EndTurn(ExclusiveTurn& turn)
	{// seqMutex_
		SeqMutexT::scoped_lock lock(seqMutex_);

		turn.UnblockSuccessors();

		if (tail_ == &turn)
			tail_ = nullptr;
	}// ~seqMutex_

private:
	using SeqMutexT = tbb::queuing_mutex;

	SeqMutexT		seqMutex_;
	ExclusiveTurn*	tail_ = nullptr;
};

// ---
}